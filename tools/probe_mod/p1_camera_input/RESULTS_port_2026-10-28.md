# P1 checks run on the port build (2026-10-28)

**Build:** `build-msvc\bin\alamo_game.exe`, built 20:33. It already holds every P1 change (`ninja: no work to do`).

**Run:** 1280×720 window, `--no-intro`, default options. Driven with Claude computer use.

**Data:** FoC data from the Steam install, with the original game not running.

**Maps:**
- land skirmish: `_MP_LAND_TATOOINE` (Rebel vs AI Empire, bought Speeders, Infantry Platoon, T4-B);
- galactic conquest: Heart of the Maw (Zann Consortium).

**Tool limits:** the computer-use tools cannot hold the middle or right button while moving the mouse. So P1 steps
2–3, P2 and the facing drags could not be run here, and P7/P8 were only partly run.

| Check | Result | What was seen |
|---|---|---|
| P1 middle mouse | **partly** | Middle click (step 4): angle back to default, position kept. This matches the spec (§3, reset keeps the look-at). Middle-drag pan / Ctrl rotate: not run (tool limit). |
| P2 drag amounts | not run | Tool limit. |
| P3 scroll speed | **matches** | Port scroll keys are W A S D ([port: Gabriel]). 1 s of D at the start zoom moved about one screen width; 1 s of W moved up further. The minimap view outline moved with it. The left arrow rotates the view ([port: Gabriel]). |
| P4 wheel on land | **matches** | Start view is the widest zoom: wheel toward you does nothing. Each notch in changes tilt and distance together, smoothly. About 15 notches go from the overhead view to the close-up behind the units. |
| P5 galaxy map | **matches** (edge scroll inconclusive) | Wheel up over Felucia zooms into the planet; wheel down zooms back out. D scrolls the map and stops a little past Yavin 4: 3 more seconds of D moved nothing. Screen-edge scroll did not trigger with the cursor about 13 px inside the window's right edge. The spec's edge region is 2 px, so this is not counted as a failure. |
| P6 empty-ground click | **matches** | Land: tanks selected, left click on empty ground cleared the selection. Galaxy: Felucia selected, left click on empty space deselected it. |
| P7 double-click (land) | **not verified** | Two right-clicks 0 ms apart were sent, but the selection was empty: my units had been destroyed in the fight. Needs a rerun with live units. |
| P8 double-click (space) | not run | No space battle in this run. |
| P9 modifier moves | **partly** | Ctrl+right and Ctrl+Alt+right both gave moves with path lines. Attack vs guard behaviour can't be told apart in screenshots. Three Alt right-clicks showed a path line to the last point only, so waypoint chaining was not visible. |

## Other things seen (outside P1)
- **Battle pauses in the background:** the battle load and the battle only advance while the window has focus. The title-bar tick stayed at 172 until the window was brought to the front.
- **Skirmish shows a campaign planet:**
  - the `_MP_LAND_TATOOINE` load screen shows a green planet with a station ring (same as `_MP_LAND_ALDERAAN`);
  - the tactical HUD planet name reads **KUAT**.

  The skirmish map's planet looks like a default, not Tatooine.
- **Minimap click:** a left click on the tactical minimap did not move the camera.
- **First drag lost:** a selection box drawn with the tool's instant drag (press, move, release in one step) selected
  nothing. A slow drag worked. Very fast drags may be dropped by the drag threshold or by frame timing.

## Still for Gabriel (needs real mouse drags)
- P1 steps 2–3 and P2: middle-drag pan direction and amount; Ctrl + middle-drag rotation (150° per screen width).
- P7 / P8: a right double-click with a mixed fast/slow selection on land, and in space.
- Land right-drag facing (MoveToPositionFacing).
- Compare against the original FoC with the same checks (README.md).

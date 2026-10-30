# P1 camera and input checks (observations, no mod files)

Specs: `docs/re/P1_CAMERA_INPUT.md` (camera, keys) and `docs/re/P1_DOUBLE_CLICK_MOVE.md` (double-click move).

Use the unmodified **FoC** (`corruption\StarWarsG.exe`) with default options:
- Options → Gameplay → **Defaults**;
- a key binding profile that was never changed.

Nothing is installed, so there is nothing to clean up.

**How to record:**
- **Screenshots:** F12 (Steam) or PrtScn.
- **Videos:** Win+Alt+R starts and stops an Xbox Game Bar clip, saved to `Videos\Captures`. Videos are better wherever timing matters.
- **Naming:** `P<n>_<what>.jpg` / `.mp4`, dropped in this folder.
- **Resolution and window mode:** note your screen resolution and whether the game runs fullscreen or windowed. Fullscreen and windowed scroll at different speeds.

Report each item as matching or not matching, with what you saw. Items marked *(measure)* are compared against the
port's numbers. Rough values are fine.

## P1 Middle mouse button on land (camera mode)
1. Start a skirmish **land** battle (any map). Don't touch the wheel.
2. Hold the **middle** button and drag the mouse slowly to the right, about a quarter of the screen.
3. Hold **Ctrl**, then middle-drag to the right again.
4. **Middle-click** without moving the mouse.

Expected:
- Step 2 **pans** the view sideways. Note which way the ground moves: with the mouse, or against it.
- Step 3 **rotates** the view around the screen centre.
- Step 4 snaps the angle, tilt and zoom back to their defaults and keeps the camera's position (End does the same,
  Home also resets the position).

If plain middle-drag rotates instead, say so. Players often remember it that way, but the code says otherwise.

## P2 Drag amounts *(measure)*
1. On land at the start zoom, Ctrl + middle-drag across the **full screen width** in one stroke.
   Note roughly how far the view turns (a quarter turn = 90°).
   - Expected about 150°.
2. Middle-drag (no Ctrl) across the full width. Note how far the ground moves, in screen widths.
   - Expected: the ground moves by more than one screen width.
   - The rule is 4 × camera distance per screen width.
   - A screenshot before and after, with a landmark in both, is enough.

## P3 Arrow-key scroll speed *(measure, video)*
1. On land at the start zoom, record a video, then hold the **right arrow** for about 3 seconds.
2. Zoom fully **out** with the wheel and repeat.
3. Repeat step 1 with the **up arrow**.

Expected:
- **Steady speed:** the view reaches it almost at once and stops quickly after release.
- **Zoomed out:** clearly faster than at the start zoom. Speed grows with camera distance.
- **Up arrow:** about 1.2× the sideways speed.

## P4 Mouse wheel on land
1. Start a land battle. Take a screenshot before touching anything.
2. Wheel **one notch** toward you (zoom out), then take a screenshot.
3. Wheel one notch away from you (zoom in), then take a screenshot.

Expected:
- **Start view:** already tilted. The port starts at pitch 55° and distance 460.
- **Each notch:** changes the tilt and the distance together, in one smooth glide of about a tenth of a second.
- **Zoom range:** the full range takes about 15 notches.

## P5 Galaxy map wheel and scroll
1. In a galactic conquest, zoomed out, hover a planet and wheel **one notch** away from you.
2. Wheel back toward you once.
3. With nothing hovered, wheel away from you once.
4. Zoomed out, hold the middle button and drag.
5. Edge-scroll to the far edge of the galaxy.

Expected:
- **Step 1:** zooms into that planet in a smooth move of about 0.65 s.
- **Step 2:** zooms back out.
- **Step 3:** zooms into the selected planet, or does nothing if none is selected. There is no gradual zoom on the galaxy map.
- **Step 4:** scrolls the map.
- **Step 5:** scrolling stops a little past the outermost planets, not far beyond them.

## P6 Left-click on empty ground
1. In a land battle, select a unit.
2. Left-click empty ground.
3. On the galaxy map, select a planet, then left-click empty space.

Expected:
- **Step 2:** the selection is cleared.
- **Step 3:** the planet is deselected.

## P7 Double-click move (land) *(video)*
1. In a land skirmish, select a mix of **fast and slow** units.
   - For example, speeders (T2-B or Rebel speeders) with AT-ATs or AT-ATs with infantry.
   - Hold Shift and click to add units.
2. Record a video. **Right-click** far across the map and watch until all units arrive.
3. Gather them again. **Right double-click** the same far point and watch until all units arrive.

Expected:
- **Single click:** one formation moving at the slowest unit's speed.
- **Double-click:** the fast units split off as their own formation and arrive first.
- **Acknowledgement:** the double-click shows a different click marker on the ground. It doesn't play a second voice line.

## P8 Double-click move (space)
1. In a space skirmish, select a fast and a slow ship. Right-click a far point, then right double-click a far point.

Expected:
- **Movement:** the same both times.
- **Acknowledgement:** only the click marker differs, and the double-click has no second voice line.

## P9 Modifier moves
1. In a land battle, select a group. **Ctrl + right-click** ground next to enemies.
2. **Ctrl + Alt + right-click** ground.
3. Hold **Alt** and right-click three points in a row.

Expected:
- **Step 1:** an attack-move. Units engage enemies they meet on the way.
- **Step 2:** a guard-move.
- **Step 3:** the first Alt click sets a new path, and the second and third add waypoints to it.

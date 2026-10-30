# P1 camera and input: scroll plane, rates, smoothing, mouse modes, default keys

Behaviour recovered from StarWarsG.exe (FoC Steam x64) for the P1 open items in docs/PHASE4.md (P1 row: "[port]
guesses"). Confidence: [H] read directly from code, [M] inferred from code with one open link, [L] guess.
Addresses are RVAs. GameConstants base = 0x140b15800 (field offsets from xml_field_tables.md).
Camera field names below are this doc's names for offsets in the camera objects.

Related: STRATEGIC_GAPS.md §7.1 (scroll acceleration), RENDER.md §3.4–3.5 (camera poses), UI.md §4 (input routing,
default key table), P1_DOUBLE_CLICK_MOVE.md (double-click move, not covered here).

---

## 0. Summary for the P1 engineer

1. **`Galactic_Scroll_Plane` (10.0) is the height (z) of the galactic camera's look-at point.** Reset and every
   "set camera position" put the target's z on that plane. The eye sits `pullback` behind the target along the pitched
   view axis. Scrolling and dragging move the target in x/y only. It is **not** used for mouse-ray picking, clamping or
   as a zoom pivot. [H]
2. **Scroll speed does not depend on zoom through Min/Max.** `*_Min/Max_Scroll_Speed` give the speed from how far the
   cursor is past the screen edge (windowed mode) or their midpoint (fullscreen, and keys in fullscreen). Tactical
   scrolling then scales with the current camera distance: world speed = S × 0.0003 × distance. Galactic scrolling
   is S world units/s with no distance factor. The vertical component is ×1.2. Everything is × the options "scroll
   speed" slider (0..3, at least 0.1). [H]
3. **There are no keyboard rotate or zoom actions.** Zoom is the mouse wheel only. Rotation is mouse-only: Ctrl +
   middle-drag, or both buttons when IDC_BOTH_MOUSE_BUTTONS_ROTATE is on. **A middle-drag without Ctrl pans the
   camera.** A middle click without moving resets the camera. The keyboard camera actions are reset / centre / home /
   overview / tether (slots 73–77). [H]
4. **Rotation is not smoothed.** Yaw/pitch change at once by mouse units × `*_Per_Mouse_Unit`. Distance, FOV, the
   zoom-spline fraction and the terrain height are smoothed by a **critically damped spring** (Game Programming Gems
   "SmoothCD") with the XML `*_Smooth_Time`s. It uses real dt, so it does not depend on frame rate. [H]
5. **Land splines are always active** when `Use_Splines` is set. The spline fraction starts at 1.0 (zoomed out), so a
   land battle starts at pitch 55 / distance 460 from the splines, not at Pitch_Default 50. [H]
6. **Galactic wheel = discrete planet zoom.** Wheel up zooms into the hovered or selected planet. Wheel down zooms back
   out. There is no continuous galactic zoom range. `Max_Galactic_Zoom_Distance`, `Min/Max_Galactic_Zoom_Speed` and
   `Galactic_Zoom_Acceleration` have no reader. [H for the behaviour, M for "no reader"]
7. **A plain left click on nothing deselects**: it submits a "set selection = empty" event. [H; M for which click
   classes count as "nothing"]
8. **Default keys come from a table built in code** (UI.md §4.3), not from a data file. Overrides are stored in the
   registry profile. [H]

---

## 1. Galactic camera and `Galactic_Scroll_Plane` [H unless marked]

Object `GalacticCameraClass` (global 0x140b2eea0; ctor 0x3c2b20). Fields:

| off | meaning |
|---|---|
| +0x08 | scroll plane z (= `Galactic_Scroll_Plane`) |
| +0x0c/+0x10/+0x14 | look-at target x, y, z |
| +0x18 | render camera (taken from the current mode's camera +0x90) |
| +0x20 | planet being zoomed to |
| +0x28..+0x30 | that planet's position; +0x34 its zoom radius |
| +0x38 | zoom start time (ms); +0x3c zoom progress 0..1; +0x40 saved light level |
| +0x44 | zoom state: 0 free, 1 zooming in, 2 zooming out, 3 zoomed in |
| +0x48 | mouse-drag scroll active (set by 0x3c3570, cleared by 0x3c3580) |
| +0x4c / +0x50 | alternative pullback / pitch (used only when the mode sub-type at +0x618 is 2 or 3 [L: which galactic variant]) |

- **Reset (0x3c2c00, called by the ctor and on mode setup):** builds the pose (below), then sets plane := `Galactic_Scroll_Plane`,
  target.z := plane, zoom state := 0.
- **Set position (0x448c20, used by the Lua/story camera calls in galactic mode):** target := point, then target.z
  := plane (0x3c2be0), then rebuild the pose (0x3c3590). In tactical modes the same entry calls the tactical
  look-at setter 0x2d03d0 instead.
- **Pose (0x3c3590 per frame, same code in 0x3c2c00):** start with a rotation of +90° about X, then rotate about X by the
  pitch (`GMC_InitialPitchAngleDegrees`, −30). Translation = target + pullback × (the matrix's third axis), pullback =
  `GMC_InitialPullbackDistance` (700). The camera looks down its −Z at the target. In the engine's Z-up frame this
  puts the eye 700 units from the target, 30° above the galactic plane (z ≈ plane + 350) [M: the direction depends
  on 0x2cf8d0's multiply order]. No yaw, no roll. The FOV is kept and only the aspect is refreshed.
- **Zoomed pose (states 1–3):** s = smoothstep(p) with p = progress (state 2: 1 − progress).
  target' = lerp(target, planet + R × `GMC_ZoomedPositionOffsetPlanetRadiusFractions` (0, 0, 1.05), s);
  pitch' = lerp(−30, `GMC_ZoomedPitchAngleDegrees` −35, s); pullback' = lerp(700, R × `GMC_ZoomedPullbackPlanetRadiusFraction` 3.0, s).
  R = planet model radius (field +0x658 of the planet's model) × 4.92. The scroll target itself does not change, so
  zooming out returns to the same spot.
- **Zoom timing (0x3c3bf0):** progress = (now_ms − start_ms) / 1000 / `GMC_ZoomTime` (0.65 s), clamped to [0,1].
  This is wall time, so it does not depend on frame rate. During the zoom the scene light blends between
  `Galactic_Zoom_Light_Level` and its saved level, and the light angles blend between `Galactic_Zoom_Out_Light_Angle`
  and `_In_Light_Angle` (elements 1 and 2). At the end it notifies "zoomed_in" (state 3, then an optional callback)
  or "zoomed_out" (state 0). Zoom in (0x3c3ee0) and zoom out (0x3c4080) play the `GUI_Zoom` SFX.
- **Scroll (0x3c3210, argument = displacement d):** only when the zoom state is 0 and the "camera locked" flag
  0x140b303c0 is clear. target.x += d.x, target.y −= d.y (d.y is screen-down positive). Then, in galactic mode, the
  target is clamped to the union AABB of all category-3 objects (the planets; per object: centre ± half extent),
  widened by 15 % of each half extent: x ∈ [cx − 1.15·hx, cx + 1.15·hx], same for y. z is untouched.
- **What `Galactic_Scroll_Plane` is not:** nothing reads camera +0x08 except the two copies into target.z
  (0x3c2c00, 0x3c2be0). The galactic camera has no ground-ray intersection, and its scroll clamp does not use the plane. [M for "no other reader": no
  other access to the global camera +8 in the decompiled output]

Port: `GalacticCameraConstants::scroll_plane` should be the look-at z (eye = target + pullback · view-back). Remove any
use as a picking plane.

---

## 2. Scrolling: keys, screen edge, push-scroll [H]

Per main-loop iteration the world input update (0x2ee40, time t in ms) runs one step when a tactical or galactic mode is
active and not paused-for-input (0x140b15418+0x98 set, +0x2d1 clear).

### 2.1 Target displacement T (0x3c360)

- Constants: tactical mode → `Tactical_*`, else `Strategic_*` (speeds 4000/1000 and 400/80, edge region 2 px,
  offscreen region 50 px).
- dt = min(1, (t − t_last)/1000) s. t_last is the time of the previous scroll step.
- Distances past each edge, each clamped to [0, Offscreen_Region]:
  right = x − (W − Edge − 1), left = Edge − x, bottom = y − (H − Edge − 1), top = Edge − y (client pixels).
  - **Push mode** (absolute scrolling off and a mouse-drag active, §3): the distances are taken from the press point
    instead, with a dead zone of 5 × `Tactical_Edge_Scroll_Region` (10 px, the tactical value even in galactic).
  - **Keys** (slots 69–72 held, flags set in 0x33460 and cleared in 0x33580): that direction's distance := Offscreen_Region.
- Speed per direction, for d > 0:
  - **fullscreen**: S = Min + (Max − Min)/2 (2500 tactical, 240 galactic), whatever d is;
  - **windowed** (global 0x140a61aac: `WINDOWED` command line or the windowed profile setting): S = Min + (d / Offscreen) × (Max − Min).
    The cursor can leave the window, so speed grows with how far past the edge it is. Keys give Max.
- T = (S_right − S_left, S_bottom − S_top) × dt × max(0.1, ScrollSpeedOption).
  - × `Push_Scroll_Speed_Modifier` (2.0) only in push mode;
  - then T.y × 1.2.
- No edge scroll while: absolute scrolling is on and a mouse drag is active; Ctrl-rotate is active; the left button is
  held (drag-select pending, except in a targeting mode); the cursor is outside the window and no drag is active (flag +0x38 [M]);
  a GUI capture is active (0x140a785a8 [L]).
- A non-zero T cancels the camera tether/follow (0x140b27d59 / mode +0x1f8 := −1) [M].
- **ScrollSpeedOption** = `IDC_SCROLL_SPEED_SLIDER`: range 0..3, 10 steps. The options "Defaults" button sets 1.0.
  The initial profile value was not traced [L: 1.0].

### 2.2 Velocity smoothing (STRATEGIC_GAPS §7.1, restated with units)

v (input object +0x30/+0x34) is the **displacement per update**, in T's units (speed × seconds):
- T = 0, v ≠ 0: v /= `Scroll_Deceleration_Factor` (1.5); when |v.x| < 0.1 and |v.y| < 0.1: v := 0.
- T ≠ 0: v += `Scroll_Acceleration_Factor` (0.08) × T, per component clamped so it does not pass T. (A zero component of
  T with a non-zero v component moves that component toward 0 by the clamp, so it is cut at once, not decelerated.)

### 2.3 Applying v

- **Tactical** (0x2d0820(cam, −0.03·v.x, +0.03·v.y)): with ψ = yaw (deg), a = −ψ·π/180, k = 0.01 × current distance
  (camera +0x30, the smoothed wheel distance, not the spline distance):
  `x −= k·(cos a · p − sin a · q)`, `y −= k·(cos a · q + sin a · p)`, with p, q = the two arguments.
  **World speed = S × 3·10⁻⁴ × distance per second** (fullscreen tactical: 0.75 × distance/s sideways, 0.9 × distance/s
  forward/back). At land distance 460 that is 345 u/s. The look-at point is then clamped (§4.6).
- **Galactic:** 0x3c3210(v): S world units/s (240 sideways, 288 vertical in fullscreen), clamped per §1.

### 2.4 Frame-rate dependence and the per-second port form

The target displacement is time-based. Only the acceleration and deceleration steps run once per update. With a reference update rate R
([port] choice; the original updated once per rendered frame):
- full speed is reached after 1/0.08 = 12.5 updates, so u = |v|/dt ramps linearly to S over 12.5/R s;
- release: u(t) = u₀ · (1/1.5)^(R·t), stopping when u·(1/R) < 0.1. In per-second terms the stop threshold is 0.1·R speed
  units/s (tactical: × 3·10⁻⁴ × distance world u/s).
- The port's current `scroll_smooth` treats v as **normalised** (1 = full speed) and its 0.1 cut-off as a fraction.
  In the original the 0.1 is in speed units (e.g. galactic at 60 fps: full speed is 4 per update, so it stops at 2.5 % of full speed).

---

## 3. Mouse camera modes [H unless marked]

### 3.1 Button → action tables

World mouse events go through a remap table, then the world handler (0x30fc0). Raw codes: 0/1/6 left
down/up/double, 2/3/7 right, 4/5/8 middle, 9/10 wheel up/down, 11 move.
- Default controls (`IDC_ALTERNATE_MOUSE_CONTROLS` off): table 0x9cf2a0 → actions 9..17, 18/19 wheel, 20 move.
- Alternate controls: table 0x9cf230 → actions 0..8 (left 0/1/2, right 3/4/5, middle 6/7/8), the same wheel and move.

Option defaults (Defaults button in 0xa0f30): alternate mouse **off**, `IDC_ABSOLUTE_SCROLLING` **on**,
`IDC_RIGHT_BUTTON_SCROLL` off, `IDC_BOTH_MOUSE_BUTTONS_ROTATE` off, scroll speed 1.0.

### 3.2 Default controls

| input | effect |
|---|---|
| middle down | start a mouse drag (+0xfc, press point saved). Tactical: **pan** mode (camera +0x10d). With Ctrl held: **rotate** mode (camera +0x10c). Galactic: drag-scroll (+0x48). Ignored while the galactic camera is zoomed in (state 3) |
| Ctrl pressed / released during the drag | switches pan → rotate / rotate → pan (0x2ee40 head) |
| middle up | end the drag. **If Ctrl is not held and the cursor is ≤ 0.01 px from the press point: camera reset** (0x2cf800(cam, 0): pitch, yaw, distance, FOV and spline fraction back to defaults; look-at kept). In galactic nothing happens (there is no tactical camera) |
| wheel | tactical: zoom (§4.2); galactic: planet zoom (§5). Ignored while a mouse drag is active |
| right drag | only with `IDC_RIGHT_BUTTON_SCROLL`: once accumulated movement > 0.08 drag units (≈ 2 % of the screen width): same as a middle drag. A release under that threshold is a normal right-click order. In galactic the drag starts at once and the release still gives the right-click order |
| left + right together | only with `IDC_BOTH_MOUSE_BUTTONS_ROTATE`, tactical: rotate mode while both are held |

Alternate controls: middle up (action 7) always resets the camera; a right drag (action 3) starts the mouse drag as above.

### 3.3 Mouse units

On each move (action 20): n = (x/W, 1 − y/H) (y up), d = (n − n_last) × f, with f = 4, or 1 while Ctrl is held. The
camera receives d. In **mouse units** that is mu = 100·d: **400 mu per screen width** (100 with Ctrl). Mouse-up gives mu_y > 0.
- If the camera used the drag (pan, rotate or galactic drag), the OS cursor is warped back to its last position, so a
  drag has no limit and the cursor does not move. The press point is then zeroed, so the middle-up reset does not fire after a drag.
- With absolute scrolling **off**, a pan drag is not applied by the mouse. It becomes push-scroll (§2.1) instead.

### 3.4 Pan (tactical, camera +0x10d) — 0x2cef10 → 0x2d0820(cam, −mu_x, −mu_y)

Same transform as §2.3 with p = −mu_x, q = −mu_y: world shift = 0.01 × distance × mu, i.e. **4 × distance / W world
units per pixel** (vertical: per H). Sign: the camera moves the same way as the mouse, the same as a key scroll that way
(mouse right = scroll right, mouse up = scroll up). It is not "grab the map" [H for the code relation; M for how it
looks on screen, see §7].

### 3.5 Galactic drag (+0x48) — 0x3c3510

d_world = (0.25·1000·d.x, −0.25·1000·d.y) = `Galactic_Right_Button_Scroll_Speed_Factor` × 1000 × d, then passed to the
scroll (0x3c3210) with its clamp. With f = 4: **1000 × Δpx/W world units** sideways (250 with Ctrl). It runs only in
zoom state 0. This is the only use of `Galactic_Right_Button_Scroll_Speed_Factor`. Despite the name it applies to
middle drags too.

### 3.6 Rotate (camera +0x10c) — 0x2cef10, no smoothing

Not while the tactical overview is active (camera +0x8c = 1).
- yaw += mu_x × `Yaw_Per_Mouse_Unit` (1.5). Ctrl+middle (f = 1): **150° per screen width**. Both-buttons (f = 4): 600°.
  Then yaw is wrapped to (−180, 180] and clamped to [Yaw_Min, Yaw_Max] (±1000 in vanilla, so no limit).
- pitch += mu_y × `Pitch_Per_Mouse_Unit` (space −1.5: mouse up lowers the pitch toward the horizon; land 0: none),
  clamped to [Pitch_Min, Pitch_Max]. Pitch is skipped while +0x8c ≠ 0.
- The FOV-drag branch (distance/FOV by vertical drag) exists but both callers pass its flag as 0, so it is dead.

---

## 4. Tactical camera rates and smoothing [H unless marked]

Constants are the mode's `TacticalCameras.xml` record (camera +0x148 when +0x10e is set). The built-in fallback table at
0x140a2a6f0 is: Pitch 45/34/80, Pitch/mu −1.5, Yaw ±360, Yaw/mu 1.5, FOV 30/10/30, Fov/mu 0.1, Fov_Smooth 0.1,
Distance 900/300/900, Distance/mu 100, Distance_Smooth 0.1, no splines, Spline_Smooth_Time 0.1, Spline_Steps 10,
overview 900/62/55, clicks 99, terrain smooth 0.1/0.1, Min_Height_Above_Terrain 10.

### 4.1 Smoothed channels (per frame 0x2cf2f0 with dt in seconds; 0x2d0d40)

Each channel is (current, target, velocity, smooth time T):

```
ω = 2/T;  x = ω·dt;  e = 1/(1 + x + 0.48x² + 0.235x³)
c = current − target;  tmp = (vel + ω·c)·dt
vel = (vel − ω·tmp)·e;  current = target + (c + tmp)·e
```

| channel | fields | T |
|---|---|---|
| distance | +0x30 / +0x34 / +0x38 | `Distance_Smooth_Time` (land 0.21, space 0.1) |
| FOV | +0x44 / +0x48 / +0x4c | `Fov_Smooth_Time` (0.05 / 0.1) |
| terrain height | +0x58 / +0x5c / +0x60 | `Location_Height_Up_Smooth_Time` (0.3) if target > current, else `_Down_` (1.0) |
| spline fraction | +0xf8 / +0xfc / +0x100 | `Spline_Smooth_Time` (not in vanilla XML: record default [L], built-in 0.1) |

A global "snap" flag (0x140b153d0) copies target → current with zero velocity instead of smoothing. Yaw, pitch and the
look-at x/y are never smoothed. Port: use this closed form with real dt; it replaces the port's linear smoothing.

### 4.2 Wheel (0x2cf190; n = notches, wheel up = zoom in)

Per notch (wheel up subtracts, down adds), all on **targets**:
- distance target ∓ n × `Distance_Per_Mouse_Unit` (land 100, space 500), clamped to [Distance_Min, Max]
  (0x2ce520, which also runs the tactical-overview click counting at max distance — already in the port);
- FOV target ∓ n × `Fov_Per_Mouse_Unit` (0 in vanilla land and space), clamped to [Fov_Min, Max];
- spline fraction target ∓ n / `Spline_Steps` (land 1/15), clamped to [0, 1].

### 4.3 Pitch and distance used for the pose (0x2d0df0)

- `Use_Splines` and not in overview: **pitch = Pitch_Spline(f), distance = Distance_Spline(f)**, f = smoothed spline
  fraction (+0xf8, initial 1.0 from reset 0x2cf800). It is in force from the first frame, not only after the first
  zoom. Land at f = 1: pitch 55, distance 460. The wheel distance field (+0x30) still moves (100/notch, 170..460) and
  is what scales scrolling and panning (§2.3, §3.4), even though the pose uses the spline distance. [H]
  Spline interpolation type (vtable +0x10 of the spline objects at record +0x58 / +0x80) was not traced [L: the port's
  piecewise linear].
- No splines: f = (distance − Dmin)/(Dmax − Dmin) (or the FOV equivalent when `Distance_Per_Mouse_Unit` ≤ 0),
  clamped to [0, 1]. If f < `Pitch_Zoom_Begin_Fraction` b: pitch_used = pitch + (`Pitch_When_Zoomed_In` − pitch) × (b − f)/b
  (local only, the stored pitch is unchanged). Space b = −1 → off.
- `Pitch_Per_Zoom_Unit` (land −30) is not read in these paths [M: unused].

### 4.4 Keyboard

There is no keyboard yaw, pitch, roll or zoom in the original. The only keyboard camera actions (hotkey dispatch
0x63f00) are:
- slot 73 (End) reset: 0x2cf800(cam, 0) (orientation and zoom);
- slot 74 (PgUp) centre: 0x2ce460, XML defaults + look-at to the map centre (land) or origin (space);
- slot 75 (Home): galactic → HUD home action 0x3d2330; tactical → full reset including position 0x2cf800(cam, 1);
- slot 76 (Insert) overview toggle: 0x2d07b0;
- slot 77 tether: UI.md §4.3.

The port's key rotate/zoom/roll rates (`key_yaw_deg_per_s` 90, `key_zoom_notches_per_s` 4, …) are [port: Gabriel]
additions and have no original value. If they are kept, matching the mouse feel suggests yaw ≈ 1.5°/mu and zoom =
one notch per press.

### 4.5 Terrain following (land, `Location_Follows_Terrain`)

Height target = max(terrain height at the look-at, 0x136650 [M: map minimum / water level]). Look-at z = the smoothed
height. After the pose is built, the eye is raised so that eye.z ≥ terrain(eye.xy) + `Min_Height_Above_Terrain` (20).

### 4.6 Look-at bounds (0x2d0820 after each move; 0x2d0df0 at pose build)

Map box: centre (cx, cy) = camera +0x128/+0x12c, half extents (hx, hy) = +0x134/+0x138.
- x ∈ [cx − hx + Side, cx + hx − Side];
- y ∈ [cy − hy + Bottom, cy + hy − Top] (Side/Top/Bottom = `*_Bounds_Buffer`).

**A positive buffer shrinks the area and a negative one enlarges it**: space −300 lets the look-at go 300 past the box.
The port's `limits()` ("added outward; negative shrinks") has the opposite sign. [H]
At pose build an extra margin is subtracted, depending on view height, pitch and vertical FOV (FOV × 1.25 when the aspect is > 4/3)
(m = h/sin(pitch) − h/sin(pitch + fov/2), applied as ½m forward). It is not applied in space (mode type 2) [M].

---

## 5. Galactic wheel and ground-click deselect

- **Wheel up** (0x3e4660), gated by the tutorial input lock (0x446220 id 4) [M]:
  - zoom state 0: select the hovered planet (HUD +0x2030), or the planet of the hovered fleet. Then, if a planet is
    selected and nothing blocks it (0x3d4130), start the zoom-in (0x3c3ee0) and open the planet panel;
  - state 3: a further wheel up runs 0x3c5fc0 [L: probably the next planet level].
- **Wheel down** (0x3e48a0): from state 3, zoom out (0x3c4080; the planet sub-view first closes via 0x3ce970 if open).
- So there is no continuous galactic zoom: the port's "0.5–1 × pullback" range should become the two-state planet
  zoom. `Max_Galactic_Zoom_Distance` / `Min/Max_Galactic_Zoom_Speed` / `Galactic_Zoom_Acceleration` are parsed only. [M]
- **Ground-click deselect** (left up, action 10, not a drag, 0x30fc0): the click is classified (0x39980). If the class is
  not an order or object class, and the two one-shot suppress flags 0x140b29fc1/fc2 are clear, 0x3af30 submits a
  selection event with an **empty** list → deselect, through the command queue. In galactic mode with the camera not
  zoomed it also clears the HUD planet selection (0x3de570(0,0)). [H for the path; M for the exact class set:
  bitmask 0x41ffd002e plus 4-with-object and 10/11 are kept]

---

## 6. Default keymap and storage

- **Source:** a fixed 153-slot table built in code (0x211940). The full list is UI.md §4.3. **No XML or text data file**
  (nothing in foc/eaw `Data\XML` or the GUI resources). "Keyboard Mapping" strings are GUI-dialog names. [H]
- **Camera/command defaults (from UI.md §4.3):**
  - scroll: arrows (69–72);
  - camera: reset End, centre PgUp, home Home, overview Insert, tether ScrollLock;
  - select: groups 1..0; assign Ctrl+n; add Shift+n; Alt+n [M];
  - select all Ctrl+A, select like Ctrl+Q, next/prev unit F/D;
  - orders: guard G, attack A, stop S, move M, waypoint W, retreat E, reinforce R, beacon B, superweapon Ctrl+W,
    attack-move T, deploy squad Q;
  - other: movie mode Space, cinematic C, screenshot F12, quick save F6, quick load F5, holocron Tab;
  - pause: the game-speed / pause HUD buttons. No pause slot in the table [M].
- **Modifier rule (0x2127f0):** a slot with modifier 0 matches only when Shift, Ctrl and Alt are all up. Modifier 1/2/3
  matches when Shift/Ctrl/Alt (respectively) is held. Other modifiers are ignored. The first matching slot in index order
  wins. Binding a key removes it from every other slot. [H]
- **Key-up quirk:** scroll flags are cleared on key-up through the same lookup. If a modifier changes between down and
  up, the flag stays set until the next matching key-up [M: shows up as a stuck scroll].
- **Storage:** registry profile (`Software\Petroglyph\StarWars FOC\Profiles\…`, MAIN_LOOP §2) values
  `KeySettingsCount`, `KeySettingKeycode<n>` (VK, −1 unbound), `KeySettingModifier<n>` (load 0x212e60, save 0x212d80).
  The mouse options are profile booleans named after their `IDC_` checkboxes [M: exact value names].
- Port: Gabriel's W A S D for camera movement (PHASE4 "Camera and controls") overrides the arrow defaults and moves
  waypoint (W), stop (S), attack (A) and next-unit (D). That is a [port: Gabriel] decision, not an original rule.

---

## 7. Open / needs an in-game probe

- Visual sense of the middle-drag pan (§3.4). The code makes mouse motion scroll the same way as the matching key.
  A probe should confirm it on screen, together with "middle-drag pans, Ctrl+middle rotates" against
  common player memory of "middle rotates".
- Galactic eye direction (§1, which side of the target the eye sits on) and the mode +0x618 sub-type 2/3 variant.
- Initial profile scroll-speed value; `Spline_Smooth_Time` record default; spline interpolation type.
- Wheel-up in planet close-up (0x3c5fc0).

Probe suggestion (tools/probe_mod, Gabriel to run): a land and a space skirmish. Record (a) the camera after 1 s of
held right-arrow at default zoom and at max zoom (expected ≈ 345 and 345 × distance ratio u/s, ×1.2 for up),
(b) a middle-drag across the full screen width with and without Ctrl (pan ≈ 4 × distance; rotate 150°),
(c) a middle click (camera reset), (d) one wheel notch on land (spline step 1/15).

# Double-click "fast" move (StarWarsG.exe FoC x64 v1.121): behaviour spec

Closes the P1 item "Double-click fast move: MoveToPosition flag byte → move flag 0x80 needs RE".
Covers mouse input → order flags → network event → movement. Camera and keymap input are in
P1_CAMERA_INPUT.md and are not repeated here.

**Conventions** (same as MOVEMENT.md):
- Addresses are RVAs unless they begin with `0x140…`, which are absolute VAs of globals.
- GC+x = GameConstants offset (base `0x140b15800`, field names from xml_field_tables.md).
- Mode type = current mode (`0x140b15418`) vf +0xe0: 0 galactic, 1 land, 2 space.
- Confidence: **[H]** read directly from the code, **[M]** inferred from structure or partial reading, **[L]** guess.

The tactical input object is the global `0x140a60df0` ("TI" below). Its event handler is `0x30fc0`.
The world-click order builder is `0x3b360`, the radar-map order builder is `0x3b090`, and the move
acknowledgement (particle and radar ping) is `0x418b0`.

---

## 0. Summary

- **No game timer.** The window class has `CS_DBLCLKS` (style 0x200b), and the window procedure turns
  `WM_LBUTTONDBLCLK` / `WM_RBUTTONDBLCLK` into input events 6 / 7. The time window and distance tolerance
  are therefore the **Windows system settings**: `GetDoubleClickTime()` (default 500 ms) and the
  `SM_CXDOUBLECLK` × `SM_CYDOUBLECLK` rectangle (default 4×4 px, centred on the first click). No
  GameConstants tag is involved. [H]
- **The first click is a normal move**, issued and acknowledged in full. The double-click then sends a
  **second, complete order** with flag 0x80 at the double-click point. It replaces the first order. Its
  acknowledgement kills the first click's ack particle and radar ping and shows the double-click ones. [H]
- **Wire:** MoveToPosition, MoveToPositionFacing, MoveToRay and MoveToRayFacing carry an **8-bit flags
  field**. Execute copies it unchanged into the movement-parameter flags word (+0x48). So the
  **event flag bits are the movement flag bits**. [H]
- **UI flag values:** 0x04 always; |0x01 attack-move (Ctrl); |0x02 guard-move (Ctrl+Alt); |0x08 waypoint
  append (Alt, from the second click on); |0x80 double-click. A plain right-click move sends **0x04**, a
  double-click sends **0x84**. [H]
- **Effect of 0x80** (only two readers, both in the land coordinator):
  1. Formation recruitment refuses a candidate whose speed ratio to the seed is > **DoubleClickMoveMaxSpeedRatio
     = 1.25** (`0x43c1b0`).
  2. LargeHover formations are ordered fastest-first instead of front-to-back (`0x439360`).

  A formation moves at its slowest member's speed (`0x41b2a0` → formation +0x20c). The effect of 0x80 is
  therefore that fast units no longer wait for slow ones: each speed class forms its own formation and
  moves at its own pace. Units still keep formation inside their group. [H for the readers, M for the
  net effect]
- **What 0x80 does not change:** no animation switch, no speed bonus, no change to individual max speed. In
  **space** nothing reads the bit: a double-click is acknowledged differently but moves like a normal
  click. [M]

---

## 1. Input side

### 1.1 Mouse events [H]

Window procedure `0x60c60` → TI handler `0x30fc0(TI, event, screen xy, …)`:

| Win32 message | event |
|---|---|
| WM_LBUTTONDOWN / UP | 0 / 1 |
| WM_RBUTTONDOWN / UP | 2 / 3 |
| WM_MBUTTONDOWN / UP | 4 / 5 |
| WM_LBUTTONDBLCLK | 6 |
| WM_RBUTTONDBLCLK | 7 |
| WM_MBUTTONDBLCLK | 8 |

- The window class is registered with style **0x200b** (`CS_DBLCLKS | CS_OWNDC | CS_HREDRAW |
  CS_VREDRAW`) at `0x97935` / `0x64177`. [H]
- `GetDoubleClickTime` and `GetSystemMetrics(SM_C?DOUBLECLK)` are not imported. Windows does the
  detection. [H]
- **Message sequence for a double right-click:** RDOWN(2), RUP(3), RDBLCLK(7), RUP(3). There is no
  second RDOWN. [H, Win32 semantics]
- **The button-up after a double-click is swallowed.** The handler remembers the last event at
  `0x1409cf22c`. It drops event 1 after 6, event 3 after 7 and event 5 after 8 (it only resets state
  through `0x370d0`). The release therefore does not issue a third order. [H]
- Each GUI layer can consume the event first: the HUD (`0x44b3f0`), the dialog (`0x2cc3e0`) and the GUI
  system (`0x1c3660`). A double-click on a HUD element never reaches the world. [H]

### 1.2 Event → action tables (mouse scheme) [H]

The handler maps the event through a {event, action} table, terminated by −1:

| option | TI flag | table | ground orders on |
|---|---|---|---|
| `IDC_ALTERNATE_MOUSE_CONTROLS` off (default) | +0x120 | `0x1409cf2a0` | right button |
| `IDC_ALTERNATE_MOUSE_CONTROLS` on | +0x121 | `0x1409cf230` | left button |

`0x36d60` sets +0x120 or +0x121 from the option.

**Table values (read from the exe):**
- **Default** `0x1409cf2a0`: 0→9, 1→10, 6→11, 2→12, 3→13, 7→14, 4→15, 5→16, 8→17, 9→18, 10→19, 11→20.
- **Alternate** `0x1409cf230`: 0→0, 1→1, 6→2, 2→3, 3→4, 7→5, 4→6, 5→7, 8→8, 9→18, 10→19, 11→20.

**Which action is the double-click move:**
- Default scheme: **right double-click (event 7 → action 14)**.
- Alternate scheme: **left double-click (event 6 → action 2)**. In that scheme right double-click maps to
  action 5, which has no handler.

### 1.3 Double-click action [H]

Action 14 (default scheme), and action 2 / 0xb (alternate scheme), in `0x30fc0`:

1. Tactical modes only. The handler returns at once when mode vf +0xf0 ("is tactical") is false.
   **Galactic has no double-click move.** In galactic, left double-click on a planet zooms in. Fleet moves
   are MoveThroughObjects and have no flag byte. [M]
2. Pick the object under the cursor (`0x36600`, `0x3fe80`) and compute the cursor action `0x39980` from it.
3. Dispatch on the cursor action:

| cursor action | effect |
|---|---|
| **4** (move to ground) | `0x3b360(TI, cursor xy, double = 1, voice = 0)`. This is the double-click move, §2. |
| **5** or **0x22** (attack target) | `0x3c220(…, double = 1)`: an Attack event with its "double" byte set (§3.4). |
| anything else | nothing. A double-click on a friendly object adds no order; the first click's order stands. |

### 1.4 The first click [H]

**The first click is an ordinary order.** In the default scheme, RDOWN (action 12) stores the anchor
(TI +0x118/+0x11c, flag +0x110). RUP (action 13) resolves the cursor action and calls `0x41f70`.
For cursor action 4 this calls `0x3b360(TI, xy, double = 0, voice = 1)`. That sends MoveToPosition with
flags 0x04 (plus modifiers), spawns the normal ack, and plays the unit's move voice (`0x442f0`, or
`0x44590` for attack-move).

**The double-click order then:**
- **replaces** the first order. It is a new group move: bit 0x08 is clear unless waypoint mode is on, so
  it is not queued (MOVEMENT.md §6.2);
- **does not replay the move voice** (voice = 0);
- **replaces the acknowledgement** (§2.3).

**Second-click position:**
- The double-click order uses the cursor position of the WM_RBUTTONDBLCLK message, not the first click's.
- Windows guarantees that it lies inside the system double-click rectangle around the first click, so the
  two world points differ by a few pixels at most.
- No game-side same-spot test exists. [H]

**Facing drag:**
- The first RUP clears the drag state (+0x110/+0x111 = 0), and RDBLCLK does not set a new anchor.
- The double-click order therefore never becomes a facing move (§2.2). [M]

### 1.5 Radar map [M]

`0x3b090` is the radar-minimap counterpart of `0x3b360`. It is reached through `0x37410` from a
function-pointer table at `0x75249`. It takes the same `double` argument and builds the same flags (§2.1).
It converts the radar point with `0x269b30` and always sends a plain MoveToPosition (no facing variant).
Whether the radar widget forwards double-clicks with double = 1 was not traced. [L]

---

## 2. Order construction (`0x3b360`) [H]

### 2.1 Flags word

Built before the event, in this order:

1. **Base value:**
   - start with 0x04;
   - **Ctrl** (`GetKeyState(VK_CONTROL)`, `0x20e2b0`) or the attack-move toggle `0x140b29fc3` → 0x05 (attack-move);
   - **Ctrl+Alt** (`0x20e290` = VK_MENU) or the guard toggle `0x140b29fc0` → 0x06 (guard-move). Guard wins over attack-move.
2. **Double-click:** |0x80 when double = 1.
3. **Waypoint mode** (GameModeManager +0xb4 = `0x140b15494`, setter `0x28d930`):
   - mode is "on" while **Alt** (alone, not with Ctrl) or the waypoint toggle `0x140b29fc4` is held;
   - any other order clears it to 0;
   - if it was already 1 when this order is built, |0x08 (append as waypoint);
   - then, if Alt is held and it was 0, set it to 1.

   The first Alt-click therefore starts a fresh path and later Alt-clicks append to it.
4. **After sending:**
   - the one-shot toggles `0x140b29fc0…fc4` are cleared;
   - with no selected unit that is not movement-locked (`0x3afe0`), the order is refused with the "can't"
     sound (`0x2d5290`, `0x140b301c8`) and nothing is sent.

| input | flags |
|---|---|
| right-click ground | 0x04 |
| right double-click ground | **0x84** |
| Ctrl + click / double-click | 0x05 / 0x85 |
| Ctrl+Alt + click / double-click | 0x06 / 0x86 |
| Alt + click (2nd and later) / double-click | 0x0C / 0x8C |

**Alt + double-click:**
- the first click starts or extends the path;
- the double-click order sees mode = 1 and is sent with 0x8C;
- it therefore **appends** a second waypoint a few pixels from the first instead of replacing it. [M]

### 2.2 Which event [H]

**Land** (mode type 1):
- The ground point comes from `0x3bb2d0`, the terrain pick.
- **Facing variant** when *all* of these hold:
  - a press anchor exists (TI +0x121 with +0xef, or +0x120 with +0x111);
  - the Chebyshev screen distance max(|dx|, |dy|) from the anchor > **MinimumDragDistance** (GC+0x11f4, shipped **4.0** px).

  The facing variant has two forms:
  - **anchor on one of the player's selected objects** (`0x36600`, `0x20a980`) → **Facing** (event 0x250 bytes, `0x409b40`/`0x40a0b0`), rotate only;
  - **otherwise** → **MoveToPositionFacing** (`0x3add90`; `0x3ae420`: +0x48 point at the anchor, +0x54 facing, +0x58 flags).
- **Otherwise** → **MoveToPosition** (`0x3acee0`, 0x60 bytes; `0x3ad330`):

  | offset | field |
  |---|---|
  | +0x48 | point (x, y, z) |
  | +0x54 | flags |
  | +0x58 | 1-bit field, UI sends 0 |

**Space** (mode type 2):
- The camera ray comes from `0x261a70` and is intersected with the plane z = 0.
- **Facing variant** (same drag rule) → **MoveToRayFacing** (`0x3ae950`; `0x3aedc0`):
  - +0x48 ray origin, +0x54 plane point;
  - +0x60 facing value;
  - +0x64 flags.
- **Otherwise** → **MoveToRay** (`0x3ae4a0`, 0x68 bytes; `0x3ae8b0`): +0x48 ray origin, +0x54 plane point, +0x60 flags.

**Debug branch:** in single-player with TI +0x8e4 set (a debug mode), `0x41ad0` handles the click
instead and no event is sent. [L]

The event goes into the outgoing queue (`0x3b01f0`), then the ack (§2.3) runs. **In multiplayer** the two
orders (single, then double) are both scheduled. The outgoing duplicate filter (COMMANDS.md §4.2) does
not collapse them, because their flags differ. [M]

### 2.3 Acknowledgement (`0x418b0`) [H]

Arguments: point, double, attack, guard. Only in tactical modes.

**Particle object:**

| priority | case | particle |
|---|---|---|
| 1 | attack | `GUI_Attack_Move_Command_Ack_Effect` (GC+0x1f0, type at +0x1f8) |
| 2 | guard | `GUI_Guard_Move_Command_Ack_Effect` (GC+0x200 / +0x208) |
| 3 | double | first, the **nearest live object of the normal ack type** (GC+0x1d8; store query `0x2a8c40`, no radius) is **killed silently** (`0x391310`, flag 1). Then `GUI_Double_Click_Move_Command_Ack_Effect` (GC+0x1e0 / +0x1e8) is used |
| 4 | otherwise | `GUI_Move_Command_Ack_Effect` (GC+0x1d0 / +0x1d8) |

**Spawn** (`0x29f810`): for the local player at the point, scaled by `GUI_Move_Acknowledge_Scale_Land`
(1.0) or `_Space` (5.0).

**Radar event:**
- attack → `GUI_Attack_Movement_Click_Radar_Event_Name` (GC+0x250, `Default_Attack_Click`);
- otherwise `GUI_Movement_Click_Radar_Event_Name` (GC+0x210, `Default_Click`);
- with double, the existing `Default_Click` event is looked up (`0x356a30`) and removed (`0x358630`).
  `GUI_Movement_Double_Click_Radar_Event_Name` (GC+0x230, `Default_Double_Click`) is played instead.

**Shipped values (GAMECONSTANTS.XML):**
- `GUI_Double_Click_Move_Command_Particle`;
- `Default_Double_Click`.

**Combinations:** Ctrl+double-click shows the attack ack. Guard+double-click shows the guard ack. Both
still carry 0x80.

---

## 3. Command and movement flags

### 3.1 Wire layout [H]

MoveToPosition (id 1) pack/unpack is `0x3ad050` / `0x3acf70`, in this order:
- x, y, z (quantised, channels 1–3);
- **flags: 8 bits** (event +0x54);
- 1 bit (event +0x58).

The other three move events carry the same 8-bit flags field:
- MoveToRay (14);
- MoveToRayFacing (19);
- MoveToPositionFacing (47).

### 3.2 Event → movement parameters [H]

Execute `0x3ad100`:
- round-trips the point through the quantiser;
- builds the movement parameter block (0x68 bytes, MOVEMENT.md §6.1) with **+0x48 = event flags
  (unchanged)** and:

  | offset | value |
  |---|---|
  | +0x4c | 0x3f |
  | +0x18 | 1e18 |
  | +0x24 | 1 (position) |
  | bytes +0x60 / +0x63 | 1 |
  | +0x5c…+0x5f | 0 |
- calls the group move `0x2a7b80`, or the waypoint-queue move `0x2a8240` when bit 0x08 is set.

The UI flag byte is the movement flag word. No translation happens.

### 3.3 Movement flag bits (corrected / extended table)

| bit | name | set by | readers and effect | conf |
|---|---|---|---|---|
| 0x01 | attack-move | Ctrl, attack-move toggle; AttackEvent with Ctrl (`0x3af790`: +0x51 → flags = 1) | `0x4160a0` uses `Attack_Move_Response_Range` (type +0x538) as the chase range. Also `0x42d2a0` (locomotor vf77) | H |
| 0x02 | guard-move | Ctrl+Alt, guard toggle | `0x4160a0` uses `Guard_Chase_Range` (+0x52c). With neither bit set: `Idle_Chase_Range` (+0x530). Also `0x41c1a0` formup (bit clear and `0x40edd0` → wait), `0x5ca390`/`0x5ca430` (fighters) | H for range, M for the rest |
| 0x04 | player order | every UI move | `0x41b940` and `0x420f70` collect waypoints with it, probably for the waypoint line. `0x4192b0`: at the end of the path, a bit-clear order re-forms or releases (`0x4220d0` / `0x4105a0`) | L |
| 0x08 | waypoint append | Alt (2nd and later click) | dispatch to `0x2a8240` (MOVEMENT.md §6.2) | H |
| 0x10 | — | not from the UI | `0x40edd0`, `0x417520` (skips formation placement when set), `0x4192b0`, `0x579f10` | L |
| 0x20 | skip destination validation | not from the UI | MOVEMENT.md §4.2 step 2; also skips formation splitting (`0x43c1b0` tail) | M |
| 0x40 | no response chase | not from the UI | `0x4160a0` returns range 0; `0x5d4730`, `0x5d55f0`, `0x6ed230` | L |
| **0x80** | **double-click** | double-click | `0x43c1b0` and `0x439360` only (§4) | H |

**Correction to MOVEMENT.md §6.1:**
- Bit 0x02 is **guard-move**, not attack. Attack-move is 0x01.
- The 0x02 note in MOVEMENT.md §6.2 ("attack-while-moving behaviour comes from flag 0x02") should read
  0x01/0x02 (attack-move and guard-move response ranges).

Lua `Move_To` (`Move_Internal` `0x57da90`) passes its own flags argument (0 from `Move_To`). The AI and
Lua therefore never set 0x80. [H for Move_To, M for the others]

### 3.4 Double-clicking an enemy (Attack event) [M]

`0x3c220` → AttackEvent `0x3afd90`, three 1-bit flags at:

| offset | meaning |
|---|---|
| +0x50 | "double": 1 from the double-click branches, 0 from a single click via `0x41f70` |
| +0x51 | Ctrl |
| +0x52 | passed through to `0x38d730` |

Execute `0x3af790` copies **+0x50 into movement parameter byte +0x5d** (Ctrl → flags 0x01). The only
reader found is `0x417520`, in formations of type +0x218 = 2:
- when +0x5d is **clear**, each member's speed target is the **formation speed** (+0x20c);
- when **set**, the speed target is left at the member's own speed.

A double-click attack therefore lets each attacker close at its own speed. [M for the effect; formation
type 2 vs land/space was not pinned down, L]

---

## 4. Execution of 0x80 [H unless noted]

### 4.1 Formation speed baseline

`0x41b2a0` returns a formation's speed:
- the minimum, over its members, of locomotor vf +0x250;
- in space for non-team members, of the effective speed `0x370f00`;
- −1 if the formation is empty.

`0x414720` stores it at formation +0x20c. This closes the [L] in MOVEMENT.md §4.4 "Speed matching":
**formation speed = slowest member**. [M: the per-frame consumer of +0x20c on land was not traced]

### 4.2 Recruitment speed test (land, `0x43c1b0`)

In MOVEMENT.md §4.2 step 4, a candidate is rejected when **all** of these hold:
- the flags have 0x80;
- the candidate's speed s_c = `0x370f00(type, obj)` is non-zero;
- **max(s_c, s_seed) / min(s_c, s_seed) > DoubleClickMoveMaxSpeedRatio** (GC+0x1508 = `0x140b16d08`,
  shipped **1.25**).

The test is **symmetric**: a much slower candidate is rejected too. Rejected units become seeds of later
formations, so the selection splits into speed bands. Each band moves at its own slowest member's speed.
**Without 0x80** there is no speed test: every unit that passes the grouping, zone, distance and path tests
shares one formation and moves at its slowest member's speed.

Example (land multipliers apply equally, so ratios are type ratios):
- Speeders and AT-ATs in one selection: a plain click keeps them in one slow formation (if within
  recruitment distance), while a double-click sends the speeders ahead in their own formation.
- A same-speed selection behaves identically for click and double-click.

### 4.3 LargeHover formation ordering (land, `0x439360`)

`0x43c1b0` calls `0x439360` at its end unless flag 0x20 is set (or `0x438c10` for object targets).
It handles formations whose seed MovementClass is **LargeHover (16)**:
- it lays them out around the group centroid;
- it then sorts them and writes their index (+0x22c):
  - **without 0x80**: by projection on the move direction (`0x43b110`, front-to-back);
  - **with 0x80**: by formation speed, **fastest first** (`0x43b420`).

### 4.4 What 0x80 does not do [M]

- No reader exists in locomotors, the animation system or space formations. The only bit-7 tests of
  the movement flag word are the two above. Searched: `>> 7 & 1`, `& 0x80` and signed-byte tests on
  +0x48 / `[0x12]`.
- **Infantry do not switch to a run animation.** Walk/move animations follow speed only (MOVEMENT.md
  §3.5–3.6). A unit is never faster than its own effective max speed.
- **Space:** formations are built by the space coordinator, which does not test 0x80. A double-click in
  space differs only in its acknowledgement and in not replaying the voice.
- Formation keeping (slots, side-error correction, final facing) is unchanged within each formation.

---

## 5. GameConstants involved

| tag | GC offset | shipped | use |
|---|---|---|---|
| DoubleClickMoveMaxSpeedRatio | +0x1508 | 1.25 | §4.2 |
| GUI_Double_Click_Move_Command_Ack_Effect | +0x1e0 (+0x1e8 type) | GUI_Double_Click_Move_Command_Particle | §2.3 |
| GUI_Movement_Double_Click_Radar_Event_Name | +0x230 | Default_Double_Click | §2.3 |
| GUI_Move_Command_Ack_Effect / _Attack_ / _Guard_ | +0x1d0 / +0x1f0 / +0x200 | GUI_*_Command_Particle | §2.3 |
| GUI_Movement_Click / GUI_Attack_Movement_Click _Radar_Event_Name | +0x210 / +0x250 | Default_Click / Default_Attack_Click | §2.3 |
| GUI_Move_Acknowledge_Scale_Land / _Space | +0x270 / +0x274 | 1.0 / 5.0 | §2.3 |
| MinimumDragDistance | +0x11f4 | 4.0 | facing-drag threshold, §2.2 |
| MinimumDragSelectDistance | +0x11f8 | 100.0 | box-select threshold on the select button; on the order button it also cancels a move (`0x30fc0` action 13). Not double-click related [M] |

There is **no double-click time or distance constant**. Both come from Windows.

---

## 6. Port notes

> **Status 2026-10-30:** All of these notes are implemented. Input: system double-click time and rectangle via `GetDoubleClickTime` / `SM_CXDOUBLECLK` in `engine/ui/src/input.cpp:91-96`, and the release after a double click is swallowed (`engine/ui/src/orders.cpp:148`). Orders: the right double-click sends a second move with `kPlayerOrder | kDoubleClick` (0x84) and no second voice (`orders.cpp:196-207, 262-267`); every UI move sets 0x04 and the Ctrl, Ctrl+Alt and Alt-run bits (`orders.cpp:161-176`, constants in `engine/ui/include/alamo/ui/orders.hpp:72`); a land drag now sends MoveToPositionFacing and a space drag MoveToRayFacing (`orders.cpp:223-261`), so "The port currently sends 0" and "sends MoveToRay for a land drag" below are out of date. Sim: `DoubleClickMoveMaxSpeedRatio` is read at `engine/tactical/movement/src/common.cpp:71` and used with `large_hover_formation_order` in `engine/tactical/movement/src/movement_system.cpp:315, 389`.

- **Input** (`engine/ui` input.cpp): the existing `double_click` (0.5 s, 4 px) matches the Windows defaults
  closely enough.
  - Windows uses a 4×4 rectangle centred on the first click (±2 px) and the user's system time. Read
    `GetDoubleClickTime()` and `SM_CXDOUBLECLK`/`SM_CYDOUBLECLK` for exact parity on Windows.
  - Suppress the release that follows a double-click.
- **OrderTranslator** (orders.cpp):
  - on a right-button double-click over ground in a tactical mode, send a **second** move with flags
    **0x84** (plus modifiers) at the second click position;
  - keep the first click's 0x04 move;
  - do not play the voice again;
  - swap the ack particle and radar ping.
- **Flags for every UI move:**
  - set 0x04 always;
  - add 0x01 for Ctrl, 0x02 for Ctrl+Alt, and 0x08 for Alt from the second Alt order on.
  - The port currently sends 0.
- **Land drag:** the original sends MoveToPositionFacing for a land drag (> 4 px) and MoveToRay /
  MoveToRayFacing in space. The port sends MoveToRay for a land drag; flag that separately.
- **Sim:** implement 0x80 only as the recruitment speed-ratio test and the LargeHover ordering. Nothing in
  locomotion changes.

---

## 7. Open items and probes

1. **In-game probe (Gabriel)**, best done as a replay or video comparison:
   - in land, select a speeder group plus AT-ATs (or infantry plus speeders);
   - right-click far away, then repeat with a right double-click;
   - expected: one formation at AT-AT speed vs. speeders arriving first, as a separate formation;
   - also check that a double-click in space looks identical to a click apart from the ack.
2. Whether the radar minimap passes double-clicks with double = 1 (`0x3b090` caller table at `0x75249`). [L]
3. Exact meaning of flag bits 0x04, 0x10 and 0x40 (§3.3). [L]
4. Movement-parameter byte +0x5d (double-click attack): the formation type 2 meaning and any other
   readers. [L]
5. Consumer of formation +0x20c on land (it is used for the space-type branch in `0x417520`). [M]
6. Action 13 in the default scheme, "+0x111 and drag > MinimumDragSelectDistance turns a move into no
   action": the role of +0x111 (likely "facing drag in progress", owned by the drag code). [L]

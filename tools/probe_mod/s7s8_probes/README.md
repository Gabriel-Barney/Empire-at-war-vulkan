# S7/S8 probe (lob solver, deploy windows, ability blocks, squadrons, escorts)

In-game probe for the open **probe** items left by roadmap S7 t23–t27 and S8 t10–t13. It needs one game session with
two skirmish battles: a land battle on Geonosis and a space battle on Kuat. Questions, numbered as in the request:

| # | Question | docs/re source |
|---|---|---|
| 1 | SPMAT grenade arc at ~275 (about 64°, lands in about 0.9 s). Range edge: a target 544–550 away is scanned but not fired at | LOB_SOLVER §9 (last bullet), §2.3, §4, §8 item 22 |
| 2 | Remote_Bomb at bare ground, interrupted before BOMBTOSS frame 12: does a `w_smallbomb` marker stay, or is ground targeting not offered at all? | LOB_SOLVER §3.5 ("MARKER removal"), §9 |
| 3 | Deploy window length for SPMAT (rate 2.0) and AT-AT, and after an undeploy reversal | TACTICAL_COMBAT "Gate details (S8 t11 / t12)", MOVEMENT §3.2 |
| 4 | Muzzle bones: do Moldy_Crow, Underworld_Infantry_Droid_Shuttle and the other S8 t12 types fire their WeaponBehavior in retail? | LOB_SOLVER §5.1, TACTICAL_COMBAT "Gate details" |
| 5 | `Activate_Ability` blocks: DEPLOY on an MDU, and an object-target ability whose target dies (return value and finish frame) | AI_TACTICAL §7.3.3, §12 |
| 6 | Squadron team target: do the members share it; does the member that scans with an empty target retarget them all? | FIGHTER_FLYING §4.10 B |
| 7 | Escort squadrons after their carrier dies: do they guard its last position? | FIGHTER_FLYING §4.10 E / F |
| 8 | (optional) First shot after a late spawn (no random initial delay); Dark_Trooper_PhaseIII deploy, jet pack, missiles silent until redeploy | TACTICAL_COMBAT "Gate details", ABILITIES_HARDPOINTS (jet-pack swap) |

The probe does not play the game for you. The shipped `BusyTacticalFreeStore.lua` gets `s7s8_hook.lua` appended.
In it, only the **Rebel AI** player's free store spawns the units and runs the fixed scenarios, one after the other.
On land it also gives **you** an Underworld Saboteur for item 2, which needs the GUI.

A read-only memory reader (`s7s8_reader.py`, built on `../a1_tactical/a1mem.py`) records every new object near the
test sites. For each object it records owner, flags, health, position and primary animation slot. Projectiles are
included, because they are in the object ID map (OBJECT_RUNTIME §1.1). For units it also records four inferred
fields:

- the locomotor state (obj +0xa8 → +0x48);
- the current target and targeting state (obj +0xf0 → +0x08 / +0x18);
- the container (obj +0x2b0);
- the squadron's team target (container +0x110 → +0x30).

The Lua side writes `S78P1|...|END` records: spawns, orders, what each `Activate_Ability` returned, and when each
block finished. The reader finds them with a heap scan. `s7s8_parse.py` turns both into `report.txt`, which ends with
an **ANSWERS** list.

## What Gabriel does (about 15 minutes in all)

1. Start Steam. Double-click `run_s7s8.cmd` in this folder. It builds `Mods\S7S8Probe` from your install, starts the
   reader, and launches FoC **windowed** with `MODPATH=Mods\S7S8Probe`. Keep the game windowed.
2. **Battle 1, LAND (about 6.5 minutes).** Go to **Single Player > Skirmish > LAND**, map **GEONOSIS**, and set the
   slots:
   - Slot 1 (you): **EMPIRE**, team 1.
   - Slot 2: **AI** (any difficulty), **REBEL**, team 2.
   - Every other slot: **closed**.
   - Credits, tech and settings: defaults.

   Start the battle.
3. Give your own army **no orders**. About 15 s in, the message *"S7S8 PROBE: your Saboteur is here"* appears and the
   camera jumps to an **Underworld Saboteur** next to your units. The probe's own units appear elsewhere on the map
   (around x 3480, y 4640, 1900 from the AI start). Leave them alone; you can watch them if you like.
4. Select **only the Saboteur** and do these three steps. His Remote Bomb throw reaches 5–130 units, about 3 tank
   lengths. The throw releases at clip frame 12, about 0.4 s after he starts the throw animation.
   - **H1 (far target, interrupted while walking):** click the Remote Bomb button, then click **bare ground** about a
     third of the screen away (well beyond 130, so he walks first). Right after that, right-click the ground on the
     **other** side so he walks away. Watch the target spot for 15 s. Note whether a small bomb marker stays there.
   - **H2 (near target, interrupted in the throw):** click Remote Bomb, then click bare ground just in front of him
     (2–3 body lengths). Right-click away **at once**: the throw must not finish. Watch the spot for 15 s and note
     what you see.
   - **H3 (near target, let him throw):** click Remote Bomb, then bare ground just in front of him, and let him throw.
     Wait 20 s. The ability recharges 18 s only after a bomb actually spawned, so H3 may need a short wait after H2.
   - If the button never accepts a click on the ground (only on units), note **"ground not offered"** and skip the
     rest.
5. Wait until about **6 minutes** after the battle started (message *"land scenarios done"*). Then press
   **Esc > Quit** to go back to the **main menu**. Do not close the game.
6. **Battle 2, SPACE (about 4.5 minutes).** Go to **Skirmish > SPACE**, map **KUAT**. Use the same slots: you
   EMPIRE team 1, AI REBEL team 2, others closed. Start. Give **no orders at all** and leave your fleet where it
   starts. Wait about **4.5 minutes** (message *"space scenarios done"*).
7. Press **Esc > Quit**, then quit the game. The `run_s7s8.cmd` window asks for your H1–H3 notes (one line), writes
   the report and removes the mod folder by itself.
8. Send back `out\<newest run>\`, `report.txt` first. The folder name is also written in `out\latest.txt`.

If the text file could not be built, no "S7S8 PROBE" messages appear: go by the clock (the scenarios start 15 s into
each battle). If anything went wrong, `cleanup_s7s8.cmd` removes the mod folder. That folder is the only thing the
probe installs; no retail file is touched.

`run_s7s8.cmd --no-launch` installs the mod and starts the reader without launching the game. Start
`StarWarsG.exe WINDOWED MODPATH=Mods\S7S8Probe` yourself; Claude can do this with computer use, see the memory note
"Driving the game with computer use". `--non-interactive` never waits for a key.

## Run plan (frames are mode frames, 30 per second; each scenario despawns its units at its end)

**Land, Geonosis, test site (3480, 4640), facing −x.** All scenarios run at the same flat strip:

| Scenario | Frames | What happens | Items |
|---|---|---|---|
| SPMAT | 450–2850 | SPMAT at the site, facing down the strip. Held tank at 275. DEPLOY at +45. At +585 the tank is replaced by one at 547 (edge). At +1185 a tank at 530 is added (control). Undeploy at +1635. Deploy +1875, undeploy +1887, deploy +1899 (two reversals 12 frames apart) | 1, 3, 5 |
| ATAT | 2853–4353 | AT-AT attacks a held tank at 250. DEPLOY true / false / true / false / true at +330 / +780 / +1080 / +1092 / +1104. A nil DEPLOY(true) falls back to DEPLOY_TROOPERS | 3 |
| MDU | 4356–5556 | Rebel MDU: DEPLOY at +30. DEPLOY again at +300 (deployed). Undeploy at +330. Move 450 at +600. DEPLOY while moving at +630. New move at +930 | 5 |
| STICKY | 5559–7059 | Two Infiltrators (autofire off). STICKY_BOMB at a live tank 120 away (+30, control). STICKY_BOMB at a tank 450 away (+480), killed 60 frames later. Move order at +1200 | 5 |
| DT3 | 7062–9162 | Dark_Trooper_PhaseIII attacks a held tank at 200. DEPLOY at +300. JET_PACK to (−120, 60) at +750. Attack again at +1200. DEPLOY at +1650 | 8 |
| REINF | 9165–9945 | Four MZ8 pulse tanks spawned 150 frames apart (+30, +180, +330, +480), each ordered at once to attack a held tank at 200 | 8 |
| MUZZLE_LAND | 9948–10608 | The 8 S8 t12 types in two groups of four, 220 apart, each with a held tank 150 to the side, each ordered to attack it | 4 |
| HUMAN | from 450 to the end | Your Saboteur (H1–H3). The reader watches every `DUMMY_BARRAGE_TARGET`, `PROJ_REMOTE_BOMB` and the Saboteur's BOMBTOSS clip anywhere on the map | 2 |

**Space, Kuat.** Three points: `sq` (−1106, 1351), `esc` (1449, −1186) and `muz` (172, 82).

| Scenario | Frames | What happens | Items |
|---|---|---|---|
| MUZZLE_SPACE | 450–1110 | The same 8 types, 600 apart, each with a held Tartan 350 to the side | 4 |
| SQUAD | 1113–3213 | Rebel X-Wing squadron. Short move. Three held Tartans at +300. At +1200 and +1800 the Tartan the squadron is on is despawned | 6 |
| ESCORT | 3216–about 7700 | Star_Destroyer. When its squadrons are out (or after 900 frames) it moves 900, and 600 frames later it is killed. 1200 frames after that a held Tartan appears 700 from the escorts' centroid; it is removed 900 frames later | 7 |

Victims are held in place, never fire and are invulnerable, except the STICKY ones and the squadron's targets, which
are despawned. They are owned by the map's Hostile player when there is one.

## Expected answers (what `report.txt` should say if docs/re is right)

The **ANSWERS** list at the end of the report uses these keys.

- **`1_arc` = `MATCHES_LOB_SLOWER`.** Expected about 64° (63.9° with the +6 target z adjust, 64.3° with none), about
  26 frames (0.87 s) of flight, apex about 142 and launch speed 24 (30 × 0.8, one `Can_Lob_Slower` step).
  `MATCHES_V30_NO_SLOWER` (about 75°, 35 frames) would mean the slow-down loop is not used. The parser fits each
  grenade's measured path, z quadratic and horizontal linear. It compares that with the solver run on the SPMAT and
  tank positions in memory, with a tolerance of 4° and 3 frames.
- **`1_edge` = `SCANNED_NOT_FIRED`.** The tank at 547 is beyond the 544 reach (slot 44, v = 30, target z + 6) but
  inside the 550 scan cube. So the SPMAT's current target becomes that tank, the targeting state never reaches 4 and
  no grenade is launched. The control at 530 must fire, otherwise the edge result says little. `NOT_SCANNED` means
  the scan cube is smaller than thought. `FIRED` means the reach is larger.
- **`2_marker` = `MARKER_USED_AND_PERSISTS_AFTER_INTERRUPT`** if the GUI issues command 57. That puts a
  `Dummy_Barrage_Target` marker (`w_smallbomb.alo`) at the spot. The marker has no lifetime: only the toss removes it,
  so after H1 / H2 it stays, and after H3 it goes when the bomb is thrown (`REMOVED_AT_THE_THROW`). `NO_MARKER` with
  your note "ground not offered" means ground targeting is not offered, the other branch of the LOB_SOLVER §9 item.
- **`3_spmat_window`.** The measured number of frames from the DEPLOY order to locomotor state 0x19. The report also
  shows the order → 0x18 delay (turret recentre wait) and the 0x18 length, to set against
  ceil(max(count − 2, 0) / (clip rate × 2.0)). No grenade is expected while the state is 0x18 or 0x1a.
- **`3_spmat_reversal` = `MIRRORS`.** Deploy(false) from 0x18 starts the undeploy clip at len − cur − 1, and the
  reverse. The parser checks the measured lengths against undeploy ≈ e + 1, or deploy3 ≈ L_undeploy − e + e2
  (e, e2 = frames spent in the interrupted states).
- **`3_atat` = `NO_DEPLOY_STATE (DEPLOY ret=nil)`.** AT_AT_Walker has `Deploys = Yes` but only a DEPLOY_TROOPERS
  ability, so Lua cannot start a deploy. The laser counts per phase show whether the AT-AT ever stops firing.
- **`4_muzzle_land`: every type `SILENT` or `NOT_SPAWNED`.** All 8 types have WEAPON only in SpaceBehavior; the land
  half is the control. **`4_muzzle_space`: `FIRES`** for every type that spawns in space (their space models have
  MuzzleA_00 / 01). `SILENT` there for a spawned type with a target would be the surprise to report.
- **`5_deploy_block` = `FINISHES_AT_DEPLOYED`.** The block finishes on signal 0x29, at the 0x19 frame (±4).
  **`5_deploy_again` = `NIL`**: a second DEPLOY true while deployed returns nil.
  **`5_deploy_moving`** is open ([M], the "moving" gate vf +0x2b0). Possible values:
  - `NO_BLOCK`: refused while moving;
  - `FINISHED_AT_DEPLOYED`: deploys anyway;
  - `FINISHED_BY_MOVE_ORDER`: pending until the move order (signal 0xb).
- **`5_target_dies`** is open ([M], is the pending entry cleared on target death?). Possible values:
  - `FINISHES_ON_TARGET_DEATH`: cleared;
  - `FINISHES_ON_NEW_ORDER`: no timeout, ends at the +1200 move order (signal 0xb);
  - `NEVER_FINISHED`;
  - `NO_BLOCK`: the pending ability was not stored.

  The `sb_live` control should finish shortly after its bomb.
- **`6_squad` = `ALL_AT_ONCE`.** While a target lives, every member is on it, and the team target (container +0x110 →
  +0x30) equals it. After a drop, the member that scans with an empty target retargets the whole squadron: every member
  gets the same new target in the same frame. `INDIVIDUAL` / `SAME_TARGET_STAGGERED` would contradict 0x406370.
- **`7_escort` = `GUARDS_LAST_POSITION`.** The escort block becomes a position block (the last waypoint) with the guard
  flags kept, so the escorts stay near the death point (centroid within 600). They engage the intruder 700 away
  (inside `Guard_Chase_Range` 1000) and come back. `_IGNORES_INTRUDER` / `_BUT_DID_NOT_RETURN` refine it.
  `RETURNED_TO_SPAWN_POINT` / `LEFT_THE_POSITION` contradict it.
- **`8_first_shot` = `NO_RANDOM_DELAY`.** A late spawn's first service clears the `Get_Int(0, FPS × recharge)`
  initial delay, so the four tanks fire after the same short aim time. A spread above 45 frames (up to 159 for
  recharge 5.3 s) means the delay survives.
- **`8_dt3` = `MISSILES_SILENT_UNTIL_REDEPLOY`.** Phases: lasers only while undeployed, missiles only while deployed,
  no missiles after the jet pack (the swap undeploys), missiles again after the second DEPLOY.

## Feeding results back

Edit only `docs/re` (RE/doc task). Quote the run folder and the report line. Then:

- items 1 / 2: LOB_SOLVER §9. Remove or answer the two **probe** bullets and adjust §3.5 / §8 item 22 if the result
  differs.
- items 3 / 4 / 8: TACTICAL_COMBAT "Gate details (S8 t11 / t12)" (deploy window, muzzle types, first-shot delay) and
  MOVEMENT §3.2 (deploy states, reversal).
- DT3: ABILITIES_HARDPOINTS (jet-pack swap undeploys).
- item 5: AI_TACTICAL §7.3.3 (the two [M] tags) and §12.
- items 6 / 7: FIGHTER_FLYING §4.10 B and E (and F, which is derived from them).

Then tick the matching roadmap tasks (S7 / S8 follow-ups) with `roadmap_store.py write`.

## Files

| File | What |
|---|---|
| `run_s7s8.cmd` / `s7s8_run.py` | One session: build + install, reader, launch, wait, report, cleanup (always). Exit 0 report, 10 no armed run, 20 preflight, 30 interrupted, 31 error |
| `cleanup_s7s8.cmd` / `s7s8_cleanup.py` | Deletes `Mods\S7S8Probe` only if it holds `S7S8_PROBE_MARKER.txt` (`--dry-run`, `--no-pause`) |
| `build_s7s8_mod.py` | Builds the mod from the install: hooked free-store script + `S78_CFG` (sites from the .ted maps), the text file with the `TEXT_S78_*` messages, the marker. `--map-info` prints the sites; `--rank-land-maps` scores every MP land map |
| `s7s8_hook.lua` | The scenarios (Lua 5.0, everything pcall'd) |
| `s7s8_reader.py` | Read-only memory recorder (`--selftest`) |
| `s7s8_parse.py` | Report and answers (`parse <log>`, `--selftest` on a synthetic log built from the expected model) |
| `selftest/s7s8_selftest.py` | Runs the hooked script under stock Lua 5.0.2 against a stub engine (land, space, other faction), parses those records, runs both `--selftest`s. `selftest/build_lua50.cmd` builds `out\tools\lua50.exe` |

Run the selftest: `toolchain\venv\Scripts\python.exe -I tools\probe_mod\s7s8_probes\selftest\s7s8_selftest.py`.
It ends with `S7S8 SELFTEST OK`.

## Limits

- **Item 2 is manual.** The Remote_Bomb ground order (command 57) is a GUI path; Lua's `Activate_Ability` cannot issue
  it. The answer depends on you doing H1–H3. Without markers the parser cannot tell "not offered" from "not done", so
  your note matters.
- **AT-AT has no DEPLOY ability.** Its deploy window cannot be driven from Lua; the ATAT scenario records the nil
  return and the firing per phase only.
- **The extra memory fields are inferred** from docs/re offsets: locomotor state, current target / targeting state,
  container and team target. Any wrong one shows as `-` and makes the matching verdict `UNREADABLE` / `NOT_MEASURED`.
  The Lua answers (block returns and finish frames) and the projectile counts do not depend on them. The `TEAMDATA`
  line says whether the team data resolved as a pointer or inline.
- **Kuat has no parsed playable box.** The space points come from the start positions only, 1800 to each side of the
  start line.
- **The land muzzle half is a control:** those types have no WEAPON in land.
- **Lua frames are quantized to 3 frames** (free-store service). ACT / BLK / MARK frames are ±2 frames; memory frames
  are exact. The parser applies a Lua → memory frame offset only when the spawn ids show 3 or more frames.
- **Projectile attribution** is by type and by the first-seen position next to the shooter (120 land, 450 space).
- **The squadron's scanning member is not visible.** The report shows the first member to switch, and whether all
  members switch in one frame and the team target changes with them.
- **"Reinforcements" are `Spawn_Unit` spawns,** not a reinforcement drop. The doc rule (first service frame beyond
  the drawn delay) covers both.
- **The escort answer needs the Star_Destroyer to launch its starting squadrons.** If none launch within 900 frames,
  ESCORT reports `NOT_MEASURED`.

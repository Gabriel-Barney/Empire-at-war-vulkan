# R1 hardpoint probe (roadmap r1 t01, "Provisional hardpoint-destroyed rule")

## r1b t07: the SHORT rerun (default profile `short`; the original 7-stage run is `--profile full`)

What the first run (2026-11-08, `out/20261108-235419`) left open: side views of the FL and ML/MR kills, a stern view of
the engine fire, and what retail does with a name that does not exist. The `short` profile (about 6 minutes of battle):

| Time | Stage | Call on ship A | Camera (by hand) |
|---|---|---|---|
| 0:10 | spawn A and B | | |
| 1:30 | FL | `Take_Damage(5000, "HP_Star_Destroyer_Weapon_FL")` | SIDE view of the left hull edge, bow end |
| 2:30 | ML | `..., "HP_Star_Destroyer_Weapon_ML"` | SIDE view of the left edge, mid-ship |
| 3:30 | MR | `..., "HP_Star_Destroyer_Weapon_MR"` | SIDE view of the right edge, mid-ship |
| 4:30 | ENG | `..., "HP_Star_Destroyer_Engines"` | STERN view (engine glow and fire) |
| 5:30 | BOGUS | `Take_Damage(1000 + 1.1 x shield, "HP_NO_SUCH")` | any; LAST step |

- Each kill is announced 15 s ahead. `R1 PROBE:` messages are the clock.
- The BOGUS step is last because retail may stop the Lua script inside that call. The hook writes its `DONE` record
  first, and a `CALL` record before every `Take_Damage`: a `CALL` with no `KILL` after it means the script stopped there
  (`lua_bogus_name = SCRIPT_STOPPED_AT_CALL`). If the script survives, `TEXT_R1P_BOGUS_DONE` shows.
- ML and MR have no breakoff prop: only `Get_Hull` (and the screenshots) show them.
- `report.json` `answers` now holds `kill_ml`, `kill_mr`, `kill_eng`, `kill_shld`, `kill_trac` and `lua_bogus_name` next to
  the old keys (mapped values such as `KILLS_THE_HARDPOINT`). A step that never ran is `NOT_RUN`, never a "no match" value.
- Camera: `--lua-camera` makes the hook call `Point_Camera_At(A position)` at each pre-message (record `CAM`). It is a
  trial: the s78 run saw no effect from a free-store script. Rotation is by hand: Ctrl + middle-drag turns the view around
  the screen centre (P1 of `p1_camera_input`, land; unchecked in space), middle-click resets the angle.
- BOGUS verdicts: `SCRIPT_STOPPED_AT_CALL`, `NO_MATCH_HIT_SHIP` (hull or shield fell as for a normal hit),
  `NO_MATCH_UNSEEN_BY_LUA` (shield already 0 and the hull masked by the hardpoint fraction), `NO_MATCH_NO_EFFECT`.

## The question

The port draws a destroyed hardpoint with the B.10 rule (`docs/re/SPACE_AND_OPEN.md` §B.10,
`engine/render/include/alamo/render/hardpoints.hpp`, `engine/render/src/hardpoints.cpp`):

- the `Damage_Decal` mesh is shown;
- the `Damage_Particles` bone subtree is shown;
- the `Engine_Particles` subtree is hidden;
- `Model_To_Attach` is never hidden;
- nothing is ever repaired.

Which hardpoint is destroyed reaches the renderer in one of two ways:

- **The PROVISIONAL rule** (`engine/render/src/sim_snapshot.cpp:473`, header `sim_snapshot.hpp:17`): an attached child
  object whose type name is one of the parent's `<HardPoints>` names, while it is dying or at health <= 0.
- **The combat source** (`apply_hardpoint_source`, `sim_snapshot.cpp:513`; events `:553`, wired at `:708`): the
  CombatSystem's flat hardpoint records. This source is authoritative.

The attach-child rule assumes that hardpoints are child objects. `ABILITIES_HARDPOINTS.md` §0 says retail hardpoints
are **not** game objects. In the port, Lua `Take_Damage(n, "<hardpoint>")` still uses that child path
(`engine/lua/src/bind_objects.cpp:1794`), so it does nothing in a battle.

The probe settles the visible rule and the Lua-visible facts in one retail space skirmish.

| # | Question | How it is answered |
|---|---|---|
| V1 | After a turret dies, does its attached turret model (`Model_To_Attach`) stay on the hull? The port says yes. The breakoff prop of the same model flies away. | screenshot |
| V2 | Do the scorch decal (`HP_*_Blast`) and the fire at `*_EmitDamage` appear at once and stay? | screenshot |
| V3 | Does the ISD's engine glow go out when the engine hardpoint dies? The port says no: see "Offline answer" | screenshot |
| V4 | Owner death: does every live hardpoint die with the ship? Expected: one breakoff prop for each live hardpoint that has one, and the death clone. | Lua counts + screenshots |
| L1 | Does `Take_Damage(n, "HP_Star_Destroyer_Weapon_FL")`, the vanilla story-script form, kill that hardpoint? | Lua: breakoff count, `Get_Hull` |
| L2 | Is the name lookup case-insensitive? Does a `Collision_Mesh` name (`HP_F-R_COLL`) also match? | Lua (ship B) |

## Offline answer (no game): `r1_model_sheet.py`

`r1_model_sheet.py` reads the shipped XML and ALO files. For `Star_Destroyer` (`EV_StarDestroyer.ALO`) it finds:

- **The decals exist.** All `Damage_Decal` meshes (`HP_F-L_Blast` and the rest) are in the model and are *not*
  hidden in the ALO, so type setup is what hides them.
- **The fire.** Each `*_EmitDamage` bone carries 1–3 `p_hp_imperial_damage` particle proxies.
- **`HP_E_MainEngines` has no children.** The engine glow proxy `pe_stardestroyerengines` hangs under `engines_big`
  (root child). So B.10's `Engine_Particles` hide does nothing on this model. If retail's glow goes out when the
  engines die, another path does it (for example, the locomotor engine-off of `TACTICAL_COMBAT` §5.1), and the port
  misses that path.
- **The attached models.** They (`EV_StarDestroyer_HP00_*.alo`) have no particle proxies. The breakoff props
  `Hardpoint_Breakoff_Star_Dest_Weapon_FL` and the rest use the **same** model as the turret they replace. This is why
  V1 is the key screenshot.
- **No damage-state art.** There are no `_ALT` meshes and no `Damaged_Smoke_Asset_Name`. The port shows no hull damage
  state on this ship.

Run it with:

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\r1_hardpoint\r1_model_sheet.py [--type <Type>] [--json out.json]
```

The data cannot answer V1–V4 or L1–L2. Those need the in-game run.

## What the probe does

`BusyTacticalFreeStore.lua` is shipped with `r1_hook.lua` appended. Only the **Rebel AI** free store runs it, in a
**Space** battle, and only while `Find_Player("Empire").Is_Human()` is true, so the main-menu battle records `SKIP`.
While the probe runs, the AI's own free-store logic is skipped.

At 0:10 two `Star_Destroyer`s owned by **you** appear at the probe site, the empty diagonal of Kuat, chosen from the
map file:

- **A** at the site;
- **B** 2500 south of A.

Each kill is one Lua `Take_Damage` call, announced 10 s ahead by an `R1 PROBE:` message:

| Time | Stage | Call | Expected (retail per docs/re) |
|---|---|---|---|
| 2:00 | 1 FL | A `Take_Damage(5000, "HP_Star_Destroyer_Weapon_FL")` | front-left turret dies |
| 3:00 | 2 ENG | A `..., "HP_Star_Destroyer_Engines"` | engines die (no decal, fire at the stern) |
| 4:00 | 3 SHLD | A `..., "HP_Star_Destroyer_Shield_Generator"` | domes on the bridge tower die, shield 0 |
| 5:00 | 4 TRAC | A `..., "HP_Star_Destroyer_Tractor_Beam"` | tractor beam dies (decal, 1 fire emitter) |
| 6:00 | 5 DEATH | A `Take_Damage(1000000)` | A dies; FR, BL and BR break off; death clone `EV_StarDestroyer_D` |
| 7:00 | 6 LOWER | B `Take_Damage(1000 + shield, "hp_star_destroyer_weapon_br")` | unknown: case rule |
| 8:00 | 7 COLL | B `Take_Damage(1000 + shield, "HP_F-R_COLL")` | unknown: Collision_Mesh match |

The amounts are chosen so that a miss does not kill the ship:

- **A stages (5000):** a full 2000-point shield still lets more than 1000 through. A miss costs about 40 % of the hull.
- **B tests (1000 + 1.1 × the shield left):** a miss costs about 1000 hull.

If stage 1 does **not** kill FL (no breakoff, and `Get_Hull` falls by a hull hit), the hook switches the other A stages
to the `Collision_Mesh` names (record `MODE`). The visual stages then still kill hardpoints.

The hook records polls. Each poll holds `Get_Hull`, `Get_Shield` and the object counts of the 5 breakoff props, the
death clone and the explosion. It polls every 3 frames for 90 frames after a kill and every 30 frames otherwise. The
records are `R1P|...|END` strings that `r1_run.py` finds with a read-only heap scan (`../a1_tactical/a1mem.py`).

## Run (Claude drives with computer use; Gabriel's go-ahead, CLAUDE.md rule 7)

1. Steam running, the game **not** running. If it is, the kit exits with code 20. Start the kit detached, from the repo
   root:
   ```
   tools\probe_mod\r1_hardpoint\run_r1.cmd --no-launch --non-interactive
   ```
   It removes a stale mod folder and builds `Mods\R1HardpointProbe`. It prints the steps and the command line, then
   waits up to 15 min for `StarWarsG.exe`. It scans until the game exits, then writes the report, removes the mod
   folder and prints `R1_RUN_DONE exit=<n>`. Use `--windowed` without `--no-launch` to let the kit launch the game.
2. Launch from `F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption`:
   ```
   StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 MODPATH=Mods\R1HardpointProbe
   ```

## Driver steps (clicks in the 1456x819 frame)

1. **Single Player > Skirmish**. Pick game type **Space** (Land/Space rows), map **Kuat**.
   - Slot 1 (you): **Empire**, team 1.
   - Slot 2: **AI, Easy, Rebel**, team 2.
   - Every other slot: closed.
   - Credits and settings: defaults.

   Start. The setup is remembered between launches: check every value.
2. Give **no orders** at any time. Never right-click on the map, never pause, never change the game speed, never save.
   Moving the camera and left-clicking your own ship to select it are fine.
3. **About 0:00:** *"R1 PROBE: armed ..."* appears. If it does not appear by 0:20, the probe did not arm (factions or
   mode wrong). Quit and check the slots.
4. **0:10:** *"R1 PROBE: Star Destroyers A and B are here ..."*.
   - On the minimap, find your two new blips in the corner that holds **neither** base. That corner is on the
     diagonal without a start position. A is at about 27 % from the left and 75 % from the bottom; B is 2500 south of
     A, about 54 % from the bottom. The printed minimap line assumes north-up.
   - Click A's blip, then zoom in with the mouse wheel until A fills about 2/3 of the frame. Keep the default pitch:
     the top of the ship must be visible.
   - Left-click A and press **Ctrl+1** (later, **1** reselects it).
5. **BEFORE shots, by 1:50:** one full screenshot, then `zoom` crops. In the model, the bow is the pointed end:
   - **bow:** the FL / FR turrets, about 10 % of the length behind the tip, one on each side;
   - **mid-ship:** the BL / BR turrets on the hull edges at half length, and the tractor beam on the top centre line at
     about 40 % from the bow;
   - **bridge tower:** the shield-generator domes on top of it, the highest point;
   - **stern:** the engine glow.
6. **Every stage:**
   - At the *"in 10 s"* message, frame the named spot.
   - At the kill: one screenshot at +1–2 s (explosion, loose turret flying off).
   - At about +10 s: zooms of the spot. Is the turret model still on the hull? Is there a scorch decal? Fire or sparks?
     For ENG: is the engine glow still on?
   - At about +40 s: one more zoom. Everything must stay; nothing is repaired.
7. **6:00 DEATH:** a screenshot every 2 s for 20 s. Count the loose turrets. Look at the death clone. Then frame **B**:
   its blip is 2500 south of A's position.
8. **7:00 / 8:00:** zoom on B's **rear-right** turret (BR, mid-ship right edge) for stage 6, then on its **front-right**
   turret (FR) for stage 7. Did each explode, scorch and burn, or did nothing visible happen?
9. **About 9:05:** *"R1 PROBE: all done"*. Press **Esc > Quit** to the main menu, then quit the game. The kit window
   finishes by itself.

Expected duration: about 9 minutes of battle, 13–15 minutes in all with the menus. If the AI attacks the ships early,
keep going: stray damage shows in the `Get_Hull` polls. If a message is missing, go by the clock. If the text file
failed to build, no messages show at all; the schedule still runs.

## Reading the result

`out\<run>\report.txt` (`out\latest.txt` names the run) has one block per stage:

- `Get_Hull` before, at +90 frames and at the stage end;
- `Get_Shield`;
- the objects that appeared;
- a `VERDICT`;
- the port's prediction.

The **ANSWERS** list at the end of the report uses these values:

| Key | Expected if docs/re is right | Other values and what they mean |
|---|---|---|
| `lua_type_name` | `KILLS_THE_HARDPOINT` (FL breakoff +1, `Get_Hull` -0.107) | `HITS_THE_HULL`: Lua passes the name to the Collision_Mesh lookup; the vanilla scripts would then kill whole ships |
| `lua_lower_case` | probably `CASE_INSENSITIVE` | `NO_MATCH_HULL_HIT` |
| `lua_collision_mesh` | open | `COLLISION_MESH_MATCHES` / `NO_MATCH_HULL_HIT` |
| `owner_death` | `ALL_LIVE_BREAK_OFF` (FR, BL, BR +1 each; FL and Shield_Gen +0; death clone +1) | `NONE`: owner death does not run hardpoint destruction (ABILITIES_HARDPOINTS §1.3 wrong); `+DEAD_ONES_AGAIN`: dead hardpoints break off twice |
| `kill_eng` / `kill_trac` | `HP_KILLED` (no breakoff prop, so `Get_Hull` must fall by 0.099 / 0.069) | `UNSEEN_BY_LUA`: `Get_Hull` cannot show it, so the screenshots decide |

**What the port does with each result:**

- `lua_type_name` = kills: the port's Lua `Take_Damage(n, name)` must route to `CombatSystem` hardpoint damage by name.
  Today it is a no-op in battles. This is a fix item for the Lua or combat lane, not for render.
- `owner_death` = `ALL_LIVE_BREAK_OFF`: matches `combat_system.cpp:3038`.
- **V1**, turret gone in retail: `Model_To_Attach` must be hidden on destruction (B.10 answer 4 is wrong). Turret kept:
  the port matches.
- **V3**, glow off in retail: an engine-off visual is missing from the port. Glow kept: B.10 on this model matches.
- **V2:** if the decals and fire appear at the kill and stay, the destroyed bit (combat source, sticky) matches. The
  provisional attach-child rule can then be deleted, because the combat source covers battles. It is still the
  fallback for objects the CombatSystem does not track.

## Files

| File | What it does |
|---|---|
| `run_r1.cmd` / `r1_run.py` | One session: build, install, (launch), heap-scan until the game exits, report, clean up. Exit codes: 0 report with records, 10 no records, 20 preflight (no game, or the game is already running), 30 interrupted, 31 error. Options: `--no-launch`, `--non-interactive`, `--windowed`, `--dry-run`, `--report-only RECORDS.log`, `--game DIR` |
| `build_r1_mod.py` | Builds `Mods\R1HardpointProbe`: the shipped free store, the generated `R1_CFG` and `r1_hook.lua`; the master text file with the `TEXT_R1P_*` messages. The probe site comes from `_MP_SPACE_KUAT.TED` |
| `r1_hook.lua` | The probe (Lua 5.0, `pcall` everywhere; methods are only called on valid objects) |
| `r1_parse.py` | Records to `report.txt` / `report.json` |
| `r1_model_sheet.py` | The offline B.10 prediction per hardpoint from the XML and ALO |
| `r1_cleanup.py` / `cleanup_r1.cmd` | Deletes `Mods\R1HardpointProbe`, but only when `R1_PROBE_MARKER.txt` is in it. That folder is the only thing the probe installs |
| `selftest_r1.py` | Offline check: build and Lua 5.0 compile; the hook under a stubbed hardpoint engine in two lookup variants plus a no-human run; the parser, also on shuffled records; the model sheet |

Offline check (both profiles, a stop-at-bogus-name variant, and the 2026-11-08 run's records when present): `toolchain\venv\Scripts\python.exe -I tools\probe_mod\r1_hardpoint\selftest_r1.py` (ends with
`R1 SELFTEST OK`).

## Known limits

- **Time-based stages.** The driver cannot signal the script, so the stages run on a fixed clock. If framing is
  late, the BEFORE shots of the next stage double as the AFTER shots of the previous one.
- **Breakoff props are objects, explosions may not be.** `Find_All_Objects_Of_Type` counts `Large_Explosion_Space_Empire`
  only if retail keeps it as an object; a `+0` there means nothing.
- **Tight timing.** Lua runs every 3 frames, so kill frames are known to ±3 frames. Screenshots cannot tell whether the
  visual changes on the kill frame or a few frames later.
- **Hull values.** `Get_Hull` and `Get_Shield` are assumed to be fractions (as in the cu8g records). A
  `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` coupling that is off shows as no `Get_Hull` change at the FL kill;
  the parser then reports `UNSEEN_BY_LUA` for ENG and TRAC.
- **Which side is "left".** The bone names give it; the model's +x is the `-L` side. The explosion at each kill shows
  the spot anyway.

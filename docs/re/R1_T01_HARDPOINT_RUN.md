# r1 t01: retail hardpoint-destroyed run 20261108-235419 (2026-11-09)

Retail run `tools/probe_mod/r1_hardpoint/out/20261108-235419/` (`run_r1.cmd --no-launch`, kit README): Kuat space
skirmish, human Empire vs Easy AI Rebel, defaults, no orders. Gabriel quit at 10:09 battle time. The kit spawned two
`Star_Destroyer`s for the human at f=300: A at (-2750, 3000) and B at (-2750, 500). It then ran stages 1-7 on the
clock, kills at f=3600 + 1800·(i-1). There are 740 records, 734 of them from the armed script (sid 172190). There is
no `DONE` record (see "Stage 7" below). Cleanup is OK and the exit code is 0.

Evidence:
- `report.txt` / `report.json`: the Lua answers.
- `records.log`: the raw `R1P|...|END` records.
- `driver_notes.md`: Gabriel's visual reading, with screenshot ids.
- `shots/mcp-computer-use-blob-<id>.jpg`: the screenshots and zooms. The ids below are the `<id>` part.

The key shots were checked again for this write-up: the ENG BEFORE/AFTER zooms, the SHLD shots, the FL plume, the
death series, the TRAC scorch and the COLL AFTER zoom.

Frames are `GetCurrentTime()·30`, read at the free-store service (every 3 frames), so ±3. In retail, `Take_Damage`
acts at once. In every successful stage, the `KILL` record written right after the call already shows the new
`Get_Hull`, `Get_Shield` and prop counts.

Tags:
- [H]: read from the run.
- [M]: one inferred link, or a single visual reading.
- [L]: the driver's impression only.

## Answers

| # | Question | Retail (this run) | Port today | Verdict |
|---|---|---|---|---|
| V1 | `Model_To_Attach` after the hardpoint dies | **Removed.** SHLD kill: the left dome on the bridge tower (`EV_StarDestroyer_HP00_M-C`) is gone at +15 s (`1794200453400-ceruqn`, `-o9f3z1`, vs BEFORE `1794200384693-sk5wsh`). Black scorch shows where it stood; the right dome stays. At +1-2 s a large piece flies off the tower top (`1794200447557-mgr8qj`). Lua counts `Hardpoint_Breakoff_Star_Dest_Shield_Gen` +1 in the same frame. Still gone at +40 s (`1794200496602-vk8dab`). [M: one hardpoint, top view]. FL turret: not visible from this angle (an edge turret), so not decided | kept: `hardpoints.hpp:17`; the attachment slot is never in an effect's show/hide list (`hardpoints.cpp:139`, `:158-164`, `attachment_visible` `:238`) | **DIFFERS** (B.10 answer 4 is wrong for this hardpoint) |
| V2 | decal and fire appear at the kill and stay | **Yes.** FL: a smoke/particle plume at the left hull edge about 1/3 from the bow. It is there at +2..+10 s (`1794200313835-*`, `1794200322518-6sgwtw`), unchanged at +40 s (`1794200362569-3zk8i8`) and still burning at about +3 min (`1794200496602-uxu5t6`, `-cg9t50`). TRAC: a dark scorch decal on the centre line in front of the tower base, with blue sparks, at +5..+15 s (`1794200506894-xgq2y6`, `-bg1yo9`) and at +40 s (`1794200540651-p3g0vb`). SHLD: scorch and smoke at the dome site at +15 s and +40 s. Nothing was repaired in 8 min [H]. ENG: no fire visible at the stern from the top view [not decided] | combat source, sticky (`sim_snapshot.cpp:513-523`, events `:553`, wired `:708`); decal and `Damage_Particles` shown (`hardpoints.cpp:160-161`) | MATCH (within screenshot timing, ≥ +1 s) |
| V3 | engine glow when the engine hardpoint dies | **Goes out.** BEFORE (`1794200384693-sk5wsh`): a large blue glow halo at the stern, left of the tower. AFTER at +3 s and +8 s (`1794200393247-jckr39`, `-ph7ocj`): the halo is gone and only a small blue speck is left. Still out at +40 s [H visual] | stays: `HP_E_MainEngines` has no children in `EV_StarDestroyer.ALO`, so the `Engine_Particles` hide (`hardpoints.cpp:162`) changes nothing; `pe_stardestroyerengines` is under `engines_big` (`r1_model_sheet.py`) | **DIFFERS**: another path turns the glow off |
| V4 | owner death | Lua, same frame as the kill (f=10800, `KILL` n=534): `Weapon_FR`, `Weapon_BL`, `Weapon_BR` +1 each; `Weapon_FL`, `Shield_Gen` +0 (already dead, their props had despawned); `Star_Destroyer_Death_Clone` +1; `Large_Explosion_Space_Empire` +10 → `ALL_LIVE_BREAK_OFF` [H]. Visual (`1794200565259-nekx2f` … `1794200578498-be8hk3`): the clone splits into two burning halves, and 3 separate flying chunks with fire trails at +6..+10 s [M: matches the 3 props]. The earlier scorch spots look like they are on the halves [L] | every live destroyable hardpoint dies at owner death, one prop each (`combat_system.cpp:3038-3045`) | MATCH (props and clone; the clone's split animation was not compared) |
| L1 | `Take_Damage(n, "HP_Star_Destroyer_Weapon_FL")` | **Kills that hardpoint** in the call's frame: FL prop 0→1, `Get_Hull` 1.0000→0.8931 (−0.1069 = FL's share of the hardpoint health), `Get_Shield` 1.0→0.0 (`KILL` n=133, `MODE` n=155: mode=type). ENG / SHLD / TRAC by type name: `Get_Hull` −0.0992 / −0.1145 / −0.0687 (their shares) and the SHLD prop +1 [H] | `bind_objects.cpp:1794-1817` looks for an attached child of that type. Battle hardpoints are flat CombatSystem records, so it is a **no-op** | **DIFFERS** |
| L2a | lower-case name `hp_star_destroyer_weapon_br` (ship B) | **Matches** (case-insensitive): BR prop +1, `Get_Hull` 1.0000→0.8931, shield 1.0→0.0 in the same frame (`KILL` n=634). Visual: an explosion and a loose piece at B's mid-ship right edge, smoke at +20 s and +40 s (`1794200626252-8faguu`, `1794200631066-5e8wkn`, `1794200676427-3hzxto`) [H] | no-op (same path) | **DIFFERS** (the rule itself is now known) |
| L2b | `Collision_Mesh` name `HP_F-R_COLL` (ship B) | **No match.** No damage at all, and **the calling script stops** at the end of that service. See "Stage 7". [H that nothing changed in the call's frame and that the script never ran again; M for the mechanism] | no match → `return 0` (`bind_objects.cpp:1815`): nothing happens and the script continues | **DIFFERS** (script stop) |

Side findings [H]:
- **Shield generator dead → shield 0 and no regeneration.** From the SHLD kill (f=7200) to A's death (f=10800), A's
  `Get_Shield` stays 0.0000. Ship B, whose shield generator is alive, regenerates from 0 to 0.44 in 1800 frames. This
  agrees with `TACTICAL_COMBAT.md` §5.1 SHIELD_GENERATOR (tagged [M] there).
- **Named damage drains the shield first.** Every successful named kill shows `Get_Shield` 0.0000 in the call's own
  record. The kit's amounts (5000; 1000 + 1.1 × shield) were large enough that the hardpoint still died.
- **Breakoff props live about 22-23 s.** B's BR prop count is 1 from f=12600 through f=13260 and 0 from f=13290.

## Stage 7 (COLL): why the record is missing and what it means

What the records show (`records.log:722-738`):
- `STAGE` n=721 at f=14103 (`arg=HP_F-R_COLL`, `mode=type`).
- `P why=before` n=733 at f=14400: B `1:0.8931:0.4396`, all prop counts 0, `EX=0`.
- `P why=fast` **n=736** at f=14400: **identical** values (B hull 0.8931, shield 0.4396, `Weapon_FR=0`, `EX=0`).
- Nothing with n > 736. n=734 and n=735 are the only numbers missing in the whole run (734 of 1..736 found).

Reading it against `r1_hook.lua`:
1. **The call ran and returned.** On the kill frame, `R1_Tick` writes three records in a row:
   - `R1_Damage` (`r1_hook.lua:253-273`) → `KILL` (n=734);
   - `R1_Say("..._DONE")` → `SAY` (n=735); Gabriel saw that message ("test 7 done") on screen;
   - `fast_until` is set, so the fast poll (`:375-381`) writes `P` (n=736).

   If `Take_Damage` had raised a Lua error, `R1_ApiFail` would have added an `APIERR` record first and the poll would
   be n=737. So the pcall returned normally.
2. **The call did nothing in its frame.** In every other stage, the effect is already in the `KILL` record and in the
   same-frame poll (for example LOWER: shield 1.0→0.0 and the BR prop +1 at f=12600). Here n=736 equals the
   before-poll exactly, so there was no hardpoint kill, no shield drain and no hull hit. Visually nothing happened at
   B's front-right at +5..+20 s (`1794200676427-bdw0wt`, `-s3msom`, `1794200688297-734sd4`, `-r6bv4y`; BEFORE
   `1794200676427-ljjjyq`).
3. **The script never ran again.** The hook would have written about 30 more fast polls, a poll every 30 frames, a `HB`
   every 300 frames (next due f=14463) and the `DONE` record at the tail. None of these appear in the roughly 2 min
   before the quit. The hook has no exit of its own that leaves no trace:
   - `R1_Finish` writes `DONE`;
   - tick errors write `ERR` (`:451-454`);
   - the record cap (20000) was not reached.

   So `FreeStoreService` / `On_Unit_Service` were no longer called for this script.
4. **The scanner was still running.** `r1_run.py:143-148` rescans every 4 s until the game exits (64 scans). Over the
   run it found every record but these two. Two records of another script (sid B76090, `LOAD`/`SKIP` at f=63,
   `records.log:739-740`) turned up only at the very end, most likely from the main-menu battle after the quit [M].

**Mechanism [M].** The retail `Take_Damage` binding (0x581710) has the error string
`GameObjectWrapper::Take_Damage -- unknown hard point %s.` (`docs/re/ghidra/strings_xrefs.csv:10762`, next to its
other argument errors). `OBJECT_SCRIPTS.md` §2.2 records what such a binding error does in retail. With no logger, it
only sets the script's failed flag +0x121. The pump then ends the script at the end of the current service and
releases it (0x2488e0: threads killed, state closed, `GAME_SCORING.md` "Start_Game" row). That matches all four
points. The Lua state is freed with it. The read-only heap scan then sees only what survives in freed memory: n=722-736
were found late (at the end of `records.log`) and n=734/735 not at all. That is why the `KILL` record is missing. The
parser's "NOT RUN" (`r1_parse.py:196-198`) means "no `KILL` record", not "not called".

**Can be concluded:**
- A `Collision_Mesh` name does not select a hardpoint [H].
- An unknown name applies no damage at all. It does not fall back to a hull hit, unlike the README's
  `NO_MATCH_HULL_HIT` guess [H for the call's frame].
- The calling script is stopped [H that it stopped; M that the "unknown hard point" path is the cause].
- In retail, therefore, a vanilla story or object script that passes a wrong hardpoint name dies silently at that
  call.

**Cannot be concluded:**
- The `res=` field of the lost `KILL` record. It is not needed: point 1 shows the pcall returned.
- Any effect after f=14400 as seen by Lua. Only the screenshots cover the later seconds.
- Whether other non-type names behave the same. Only one Collision_Mesh name was tried, and no plainly bogus name.
- Whether the lookup also accepts something else, such as a bone name. In vanilla, `HP_Star_Destroyer_Tractor_Beam`'s
  `Collision_Mesh` is its bone `HP_trac_bone`. This was not tried.

`lua_collision_mesh` should read **`NO_MATCH_SCRIPT_STOPPED`** (a value the parser does not have yet).

## Verdict on the provisional attach-child rule

**Delete it** (`sim_snapshot.cpp:473-487`, header `sim_snapshot.hpp:17-21`). Reasons:
- Retail hardpoints are records, not objects (`ABILITIES_HARDPOINTS.md` §0). This run confirms it: Lua named damage
  kills hardpoints that no child object stands for, and the only objects that appear are the breakoff props and
  explosions.
- In a battle, the combat source (`apply_hardpoint_source`, `:513`) already overrides the provisional bits for every
  object it tracks. Its destroyed states are sticky, as V2 requires.
- Objects the CombatSystem does not track have no live hardpoint state in retail either, so the fallback has nothing
  true to show.
- The only port code that can make a "hardpoint child" is the attach-child lookup of Lua `Take_Damage`
  (`bind_objects.cpp:1807-1817`), which this run shows is wrong too.

Deleting the rule does not settle the visuals. The B.10 rule in `hardpoints.cpp` is wrong in two places (V1 and V3, see
the follow-ups).

## Follow-ups

- [port] Lua `Take_Damage(n, name)` (`engine/lua/src/bind_objects.cpp:1807-1817`):
  - Match `name` case-insensitively against the owner type's `<HardPoints>` type names, not against `Collision_Mesh`.
  - On a match, apply the damage to that CombatSystem hardpoint. The shield takes it first, as in this run.
  - With no match: `script_error_stop("GameObjectWrapper::Take_Damage -- unknown hard point %s.", name)`, with no
    damage. Today it returns 0 and the script continues (`:1815`).
- [port] Delete the provisional rule:
  - `sim_snapshot.cpp:473-487` (pass 3) and the header text `sim_snapshot.hpp:17-21`;
  - `SnapshotObject::attach_parent` (`sim_snapshot.hpp:213`) if nothing else reads it;
  - the comment at `sim_snapshot.cpp:513-515` and the test wording at `engine/render/tests/test_render_r5.cpp:251`.
- [port] V1: hide the `Model_To_Attach` slot on destruction. Today it is kept by design (`hardpoints.hpp:17`,
  `hardpoints.cpp:158-164`). Whether this applies to every destroyable hardpoint or only to those with a
  `Death_Breakoff_Prop` waits for the RE / probe items below. Until then, hiding it when a breakoff prop exists
  matches the one observation.
- [port] V3: an engine-off visual is missing. When the last engine hardpoint dies, the glow proxy
  `pe_stardestroyerengines` goes out. The sim already has the state (`engines_offline`, `tactical_battle.cpp:3074`);
  the snapshot needs a flag and the renderer a hide rule, once the RE item below names the rule.
- [RE] V1: which path removes or detaches `Model_To_Attach` on destruction? B.10 answer 4 says "[M] that no other path
  hides it", and this run contradicts that. Candidates:
  - the breakoff spawn (B.10 effect (e)): is the prop the attached model itself, moved off?
  - owner bookkeeping 0x3a5700;
  - something on the attachment list (0x12d770).

  Write the answer into `SPACE_AND_OPEN.md` §B.10.
- [RE] V3: what turns the engine glow off? The candidate is the ENGINE offline path → `Locomotor_Set_Engines`
  0x5545a0 (`TACTICAL_COMBAT.md:2359`; `HARDPOINT_DISABLED.md` §2). Find the reader of the locomotor engines-on word
  (+0xd0) on the draw side, and how it picks the proxies to hide (by proxy name, by bone `engines_big`, or by
  attachment). Also check whether a timed ion disable (same 0x5545a0) turns the glow off too.
- [probe] One more short space run (kit `tools/probe_mod/r1_hardpoint/`):
  - side views of FL (a turret with a breakoff prop) and of ML/MR (ion cannons with `Model_To_Attach` and **no**
    breakoff prop) dying, to settle V1 for turrets;
  - a stern view for the ENG fire;
  - a deliberately bogus name (`"HP_NO_SUCH"`) run **last**, to confirm the script stop without the Collision_Mesh
    question mixed in.
- [probe] `r1_parse.py:196-198`: when the `before` poll exists but no `KILL` record does, and the poll after it in the
  same frame shows no change and no later record follows, report `NO_MATCH_SCRIPT_STOPPED` instead of `NOT_RUN`
  (`:285` answers map). Add the kill_eng / kill_shld / kill_trac answers to `report.json` (today they are only in
  `report.txt`).

## r1b t07 run 20261109-050430 (short profile: FL, ML, MR, ENG, then the bogus name)

Kit `tools/probe_mod/r1_hardpoint/`, Kuat space skirmish, Empire (human) vs Easy Rebel AI, driven by Claude. Records:
`out/20261109-050430/records.log` (497 hook records, contiguous, plus 3 from the menu-battle scripts).

**Lua results (solid; the same kind of evidence as the 2026-11-08 run)**

| Step | Call on ship A | Result |
|---|---|---|
| FL | `Take_Damage(5000, "HP_Star_Destroyer_Weapon_FL")` | `Hardpoint_Breakoff_..._FL` +1, explosion +1, `Get_Hull` 1.000 -> 0.893 (-0.107 = its share): `KILLS_THE_HARDPOINT` |
| ML | `..., "HP_Star_Destroyer_Weapon_ML"` | no breakoff prop, explosion +1, `Get_Hull` 0.893 -> 0.786 (-0.107 = its share): `HP_KILLED` |
| MR | `..., "HP_Star_Destroyer_Weapon_MR"` | no breakoff prop, explosion +1, `Get_Hull` 0.786 -> 0.679 (-0.107): `HP_KILLED` |
| ENG | `..., "HP_Star_Destroyer_Engines"` | explosion +1, `Get_Hull` 0.679 -> 0.580 (-0.099 = its share): `HP_KILLED` |
| BOGUS | `"HP_NO_SUCH"` | **not run**: no CALL record, no DONE record |

So the type-name Lua form kills ion-cannon hardpoints (ML, MR) too, and each kill removes exactly its health share from
`Get_Hull` (the hull/hardpoint coupling holds on all four). ML and MR have no breakoff prop; only `Get_Hull` shows them.

**The script went silent at frame 9693.** The last records are slow polls (n=496 and 497, A hull 0.5802, shield 0.40, B
untouched); the BOGUS pre-message and STAGE record were written at 9453, the call was due at 9900. The game kept running
(the coordinator saw battle time about 8:14, frame about 14800). No `ERR` or `APIERR` record. The cause is **not found**:
nothing in the hook's code runs at 9693 that did not run in every poll before, and the cut is clean (n=1..497 with no
gaps), so it is the script that stopped, not the reader. It is **not** the bogus name (the call was never reached). The
2026-11-08 run also lost its script once: its last record is the `before` poll of stage 7 (frame 14400, name
`HP_F-R_COLL`), so that stop may have been the name. Open question for the next run: does the free-store script of the
AI get stopped by the engine for a reason that is independent of the kit (battle time about 5:23 here)? The `tail` profile
puts the bogus call at 1:45, well before that time.

**No CAM record**: `config.json` of this run says `lua_camera: false`, so the flag did not reach the build (the hook only
calls `Point_Camera_At` when `R1_CFG.lua_camera` is true). Whether the camera call works is therefore still untested.

**Visual notes (by eye, low confidence; the camera could not be rotated: Ctrl+Left and Delete do nothing, so all views are
oblique from the south at two zoom levels, no true side or stern view).**
- After FL: smoke and sparks at the bow's right edge in screen space.
- After ML and MR: dark scorch decals on the upper hull near mid-ship and next to the bridge tower; the turret models
  could not be told apart at that zoom (V1 for turrets is still open).
- ENG: the stern face was never in view, so the engine-glow question (V3) is still open from this run. The 2026-11-08 run
  saw the glow go out from above.
- The hull model did not visibly change.

**Kit changes after this run**: `report.json` `answers` carry `kill_ml/mr/eng/shld/trac` and `lua_bogus_name`; a step that
never ran is `NOT_RUN` (with the last record frame as the reason); a stage whose `before` poll is the last record of an
old-format run (no `CALL` records) is `NO_MATCH_SCRIPT_STOPPED` (the 2026-11-08 COLL stage now reads that). New profile
`--profile tail` (ENG then BOGUS at 1:00 and 1:45; the hook orders ship A to move north 3000 units at 0:11 so a camera in
the south sees its stern; record `TURN`).

## r1b t07 run 20261109-052935 (started as `--profile tail`, but ran the short schedule)

**Profile bug (kit, fixed).** `r1_run.py` built the mod in its real run path without `--profile` and `--lua-camera`
(only the `--dry-run` path passed them), so the console printed the TAIL steps while the game ran the default `short`
schedule (PLAN record: `profile=short|lua_camera=false|turn=false`). This is also why run 20261109-050430 had no CAM
record. Now both paths call one `build_mod()`; `selftest_r1.py` builds with the command-line options and checks the built
free store for `profile = "tail"`, `lua_camera = true`, `turn_dy`, `first_kill = 1800`. Because the short schedule ran,
the hook's `Move_To` (TURN) was not exercised; the driver ordered ship A north by hand (right-click on the minimap, about
0:40) and followed it from the south.

**Lua results** (same short schedule as the first run): FL, ML, MR, ENG all `HP_KILLED` again, each with its exact
`Get_Hull` share (0.107, 0.107, 0.107, 0.099); no ERR/APIERR records.

**Engine glow (V3), high confidence by eye:** from the south behind the moving ship, before the ENG kill (frame about 5600,
ship stationary) all three main engine glows are bright cyan; after the kill at frame 8100 all engine glows are OFF (nozzles
dark grey/orange), no visible fire or sparks at the stern, unchanged 30 s later. Retail turns the engine glow off when the
engine hardpoint dies (the port keeps it: B.10's `Engine_Particles` subtree of this model is empty). The FL / ML / MR scorch
decals were visible on the upper hull.

**Bogus name `HP_NO_SUCH` (frame 9900): the script survived the call.**
- Records in the heap scan: n=504 `P why=before` (f=9900), then n=509 `P why=after_final` and n=510 `P why=fast`, both f=9900.
  The hook writes n=505..508 between them (DONE, CALL, KILL, SAY: DONE was written first in that hook version), and they
  were lost by the heap scan. Records 509 and 510 are written after `Take_Damage` returned, so the call returned and the
  script ran on.
- No later record exists because that hook version had already written DONE (`R1.done`), which stops all polling: the
  silence after n=510 is by design, not a script stop.
- A's hull and shield before and after are identical (0.5802 / 0.4751), but both reads are in the same frame, so the effect
  of the call (hull or shield hit, or nothing) was **not measured**.
- The old parser said `NO_MATCH_SCRIPT_STOPPED` here: wrong (it compared frames, not the hook's record counter). It now
  looks at counter n: later records after the `before` poll mean `CALL_NOT_RECORDED_SCRIPT_SURVIVED`. The same rule
  re-reads the 2026-11-08 COLL stage (before n=733, fast poll n=736 in the same frame): that script did not stop at the
  `HP_F-R_COLL` call either: its n=734, 735 were lost and no later poll is in the records, so the 2026-11-08 silence stays
  unexplained (that hook had no DONE-first).
- Run 1 of today (20261109-050430) is contiguous (n=1..497) and silent from frame 9693 on, long before any bogus call. Run 2
  did not repeat it, so it was not a fixed engine time limit. Not explained; the heap scan is the suspect (a block it could
  not read, or records missed), not the Lua script. The kit now keeps a second copy of every record (`dup=1`, merged by
  sid + n), lists missing counters in the report ("records lost by the heap scan") and logs `unreadable_chunks` in `run.json`.
- Hook change: the DONE record is no longer written before the bogus call; the hook polls for 15 s after the call and then
  finishes. A `CALL` without a `KILL` still marks a script that stopped inside the call.

**Open after this run:** what `Take_Damage(n, "HP_NO_SUCH")` does to the ship (hull / shield / nothing). One more `tail`
run (about 3 minutes of battle) with the fixed hook measures it from the polls in the 15 s after the call.

## r1b t07 run 20261109-055324 (`--profile tail`, fixed kit)

PLAN: `profile=tail|lua_camera=false|turn=true`; `TURN res=true` at frame 330 (`Move_To` 3000 units north worked: the
ship turned and the driver got a true stern view). Schedule: ENG kill at frame 1800, bogus name at 3150.

**ENG (stern view, by eye, high confidence):** before the kill all three main engine glows are bright cyan; at +12 s the
glows are out and grey smoke trails come from the nozzles; at +22 s and +60 s the glows stay out, nozzles dark, no fire.
Lua: `Take_Damage(5000, "HP_Star_Destroyer_Engines")` ok, explosion +1, `Get_Hull` 1.000 -> 0.901 (-0.099 = its share).

**Bogus name `HP_NO_SUCH` (frame 3150): the call returns, then retail ends the script.**
- Hook order at the kill: `before` poll (n=153), `CALL` (154), `Take_Damage` inside a `pcall`, `KILL` (155), `SAY` (156), then
  the `fast` poll (157) in the same tick. Record 157 is in the records (both copies), so the call **returned** without a Lua
  error and the tick ran to its end. The next services (every 3rd frame) wrote nothing for the following ~50 s: no 15 s of
  post-call polls, no DONE. So the script was ended at the end of that update. This fits the known retail behaviour that
  a failed engine read ends a Lua script silently (the engine flags the script and drops it when control returns to it).
- Records 154..156 (and in run 20261109-052935 the records n=505..508) are missing in both copies. An error unwinding before the
  commit cannot be the reason: 157 was written after them in the same tick. The likely reason is that the script state is
  freed when the script ends (`lua_close`), so the heap scan finds only what the allocator has not overwritten yet; the
  last records before the death are the ones lost. Not proven.
- Effect of the call on the ship: not measurable (`Get_Hull` 0.9008 / `Get_Shield` 0.3251 before and in record 157, same
  frame; no later reads exist). Any visible effect (explosion, damage) has to come from the screenshots.
- Same pattern in the 2026-11-08 run (`HP_F-R_COLL`: `before` n=733, `fast` n=736 in the same frame, nothing later) and
  in 20261109-052935 (there the hook had written DONE first, which alone ends the polling, so that run alone does not
  show it). So a Lua `Take_Damage(n, "<name that is no hardpoint>")` ends the calling script, for a made-up name and for a
  Collision_Mesh name (`HP_F-R_COLL`) alike; the type-name and lower-case forms of real hardpoints do not.
- Parser: verdict `SCRIPT_STOPPED_AFTER_CALL` (later records only in the call's own frame, none after) vs
  `CALL_NOT_RECORDED_SCRIPT_SURVIVED` (records in later frames). `lua_bogus_name` and `lua_collision_mesh` use it.
- Run 20261109-050430 (silent from frame 9693, before any bogus call, records contiguous) is not explained by this; it is the
  one unexplained silence.

## r1b t07 conclusion

- **Lua `Take_Damage(n, "<HardPoint type name>")` kills that hardpoint**, also for the ion cannons ML and MR (no breakoff
  prop) and for ENG: each kill takes exactly its health share from `Get_Hull` (0.107, 0.107, 0.107, 0.099), lower-case works
  (2026-11-08).
- **Engine glow (V3):** off after the engine hardpoint dies, in two runs (top view 2026-11-08, stern view 20261109-052935 and
  -055324); grey smoke trails from the nozzles at +12 s in the second stern run, no fire. The port keeps the glow
  (B.10's `Engine_Particles` subtree of this model is empty): a missing engine-off visual.
- **FL / ML / MR visuals:** scorch decals on the upper hull; the turret models could not be told apart at the available zoom
  and angle (V1 for turrets stays "by eye, not settled"; the 2026-11-08 shield-generator dome did disappear).
- **Unknown name:** `Take_Damage(n, "HP_NO_SUCH")` (and a Collision_Mesh name) returns and then ends the calling Lua script
  silently; the ship's hull and shield are unchanged in that frame (a later effect cannot be read, the script is gone).
  A port script engine need not copy the termination; the effect of the call itself is unmeasured.
- **Status: DONE.** Open leftovers (not blocking): the turret-model question (V1) by eye, the one unexplained script
  silence of run 20261109-050430, and `--lua-camera` untested.

# a2f t07 budget probe (AI budget reset flag +0x74, pre-Init Infrastructure pool, template state)

Roadmap group a2f, task t07: "Gabriel probe: budget reset flag +0x74 setter and the Empire's pre-Init Infrastructure pool /
template state at HotM start (in-game Lua Budget queries)". One run of Heart of the Maw (HotM) as REBEL, hands off for about
5 minutes. Run in game on 2026-11-05 (two runs, results in `docs/re/A2F_T07_BUDGET_PROBE.md`).
A2g t01 (2026-11-05): a 20-minute run found the +0x74 setter (a template switch; `docs/re/A2G_T01_RESET_FLAG_PROBE.md`). Since
then the story script keeps a full sample every 30 s after DONE (so a long run stays covered), and the watcher reads +0x74 as
a byte (the three bytes after it are uninitialised padding). For a long run keep the game in front and auto-resolve the
battles the AI starts: their prompt pauses galactic time.

**a2h t02 (2026-11-06): why retail never flushes MajorItem.** The port evaluates `Needs_More_Space_Force_Globally` = 1 from
frame 1, so its Empire runs `Flush_MajorItem_Budget`; retail's MajorItem pool never dipped in the a2f / a2g runs
(`docs/re/A2G_T02_T04_PORT_VS_RETAIL.md` section 4). Added for that, everything else unchanged:

* five more hooked plans: BuildBaseComponentHeavyVehicleFactory, BuildBaseComponentAdvancedVehicleFactory (BaseForce_Thread),
  LiftBlockadePlan, FlushMajorItemBudget, ConquerPiratePlan (MainForce_Thread); FlushMajorItemBudget takes a full sample at
  its start and its end (the MajorItem pool before / after `Budget.Flush_Category`);
* end records per plan run: `PRET` (thread returned), `PEXIT` (the plan called ScriptExit), `PFAIL` (the shipped
  `<force>_Production_Failed` ran); PSTART gets `n=` (start number in that script instance) and `prev=` (how the previous run
  of the instance ended; `none` = no end record = the goal system released it). The 40-start cap per script is gone;
* an `MJ` record with every Empire sample (all channels): the shipped `Needs_More_Space_Force_Globally`,
  `Need_To_Flush_MajorItem_Budget`, `Can_Reclaim_Excess_MajorItem_Budget`, `Has_Enough_Space_Force_Globally`,
  `Friendly_` / `Enemy_Global_Space_Unit_Raw_Total`, and 9 mod equations (8 since a2i, which dropped ActiveGoals without a category, see below): `Variable_Human` / `Variable_Self.SpaceTotalUnnormalized`,
  `Variable_Self.ActiveGoals` without and with `Parameter_Category = AIGoalCategoryType[MajorItem]`, `HasUnit(Death_Star)` (the
  other term of the flush trigger), `Maintenance` (+ Unnormalized), `PlanetsControlled` (+ Unnormalized);
* the 30 s full sample after DONE (a2g) stays; DONE at 300 s now says "5-minute dense phase done, keep running", and a new
  message at 1500 s (25 min) says the run can end. **Run at least 20, better 25 minutes of galactic time**: in the a2g run the
  Empire's `Tech_Upgrade` template (which turns `Flush_MajorItem_Budget` off) came on only at frame 17569 (9.8 min), and the
  question is whether the flush goal ever runs before and after that;
* record caps raised so the long run fits: story 20000 (was 8000), plan scripts 3000 (was 300), free store 500 (was 400:
  the same 100 samples now that the Empire's carry an MJ record).

**a2i t02 (2026-11-06): story channel fix.** In run `20261106-040725` the story channel kept only `LOAD` and `DEF`, the
Empire's free-store instance stopped after its first sample, and every Empire plan instance recorded one sample and then
only bare `PSTART` lines. Cause: the MJ mod equation `A2FB_ActiveGoals` (`Variable_Self.ActiveGoals` with no
`Parameter_Category`, `AG=nil` in that run). Retail cannot evaluate it (`docs/re/A2H_GALACTIC_AI_RE.md` section 1), and a
failed evaluation calls the script error helper. That helper raises no Lua error, so pcall misses it, but it sets the
script's failed flag (`docs/re/LUA_SCRIPT_OUTPUT.md` section 1), and the script then stopped. The Underworld, which has no MJ
record, was not affected. Changes:

* `A2FB_ActiveGoals` (key `AG`) is removed from the equation file and the MJ list (46 probe equations). The answer it gave
  is already measured: no category means the token fails. Every equation the kit still evaluates answered in that run.
* every record is built inside pcall (`A2FB_Rec` in `a2fb_core.lua`), and in the story script each player's sample and each
  marker record (`LOAD`, `DEF`, `ENTER`, `FIRST`, `DONE`, `LONGDONE`) has its own pcall. A Lua error now costs one record and
  leaves one `ERR` record. It no longer ends the round or the channel.
* the selftest emulates retail. The model engine fails (failed flag, no value) on an unknown equation and on a probe
  equation that breaks a retail rule (`retail_eval_fails`: ActiveGoals without exactly one `Parameter_Category`). The drivers
  then stop servicing that script. `math` is removed from every probe environment, and `debug` from the story and free-store
  ones. A fault-injection run checks the ERR path. With the a2h code the selftest fails 29 of 101 checks; with this code it
  passes all of them.

**a2j t02 / t04 (2026-11-06): D4 perception, D3 production events, 30-minute run.** One run now serves a2j t02 and
t04 (`docs/re/A2J_T02_PROBE_PLAN.md`; how to run it: section 9). Added, everything else unchanged:

* channel **D** (D4): one plot script per (AI faction, target planet) pair, set as the `Lua_Script` of that faction's
  own AI story plot (mod copies of `Story_Plots_Underworld_AI_Heroes.xml` and `Story_Plots_Empire_AI_Heroes_EXP.xml`,
  whose shipped `Lua_Script` is empty): `A2fb_PerceptU` samples Underworld -> Kashyyyk, `A2fb_PerceptE` Empire ->
  Felucia, every 30 frames in frames 0..600, then every 300 frames to the end of the run. Records `DT` (11 token mod
  equations `A2FB_D4_*`: Markup, ConnectsIsolatedPlanetsByForce, TradeRoutes, Hints, HasGround / SpaceForce,
  IsHumanControlled, IsConnectedToCorruption, IsCorrupted, AnyCurrentThreats) and `DS` (9 shipped equations with the
  planet as target: Should_Conquer_Pirate_Planet, Is_Good_Invasion_Target, Opens_New_Front, Is_AI_And_Should_Be_Ignored,
  GenericPlanetValue, Is_Neglected_By_My_Opponent_Space, System_Recon_Relevance, Is_Connected_To_Me / _Enemy). One script
  per pair so that, if one of these fails in retail, only that pair's channel stops (not the budget channels).
* channel **Q** (D3): the shipped `GameScoring.lua` with a hook on `Galactic_Production_Begin_Event` /
  `_Canceled_Event` / `_End_Event` (records `QB` / `QC` / `QE`: frame, planet owner, planet, type, cost, and the owner's
  `Get_Credits()` when the event ran, `cr=`, for lane A's D3 / D5 question), for every player. **The Lua API has
  no read of a planet's build queue** (LUA_API.md has no queue query), so a 30-frame poll of the queue heads is not
  possible from Lua; the report rebuilds the heads from these events (section 7 of the report).
* 30-minute run: the story script adds record `MIN30` and the message "30 minutes recorded" at 1800 s (LONGDONE at
  1500 s stays, its text now says keep going); free-store cap 300 samples / 1600 records (40 minutes at 8 s).
* report sections 6 (D4 table, retail | port per frame, against `port_reference_a2j.tsv`), 7 (every AI player's events
  in 0..1500 with credits, the first Begin per queue, the Empire's queue heads every 30 frames in 0..1400) and 8 (per
  fiscal cycle P / I, retail | port, with `--port-trace`); new files `perception.tsv`, `production.tsv`. `a2j_port.py`
  makes the port side (export + traces + run), `port_a2j_patch.py` is its trace patch. `--no-percept` builds without
  channel D (section 9, "Probe effect"). Selftest: 140 checks.

**a2k t05 (2026-11-07): the Underworld saboteur short run.** For `docs/re/A2K_T08_RE_ANSWERS.md` section 5 (Q5: H1 vs H2 of
Q4.2, why retail's Naboo got the starting saboteur). How to run it: section 10. Added:

* a 12th hooked plan, `AI_Plan_Underworld_DeploySaboteur` (in every build), with an extension (`a2fb_saboteur_plan.lua`):
  PSTART and the end records carry `goal=` (`MainForce.Get_Goal_Type_Name()`), `tgt=` (target planet) and `tgtc=`
  (`Target.Is_Corrupted()`); new records `PSTAGE` (the stage AssembleForce got from `MainForce.Get_Stage()`), `PPROD`
  (result of `BlockOnCommand(Produce_Force(stage))`), `PMOVS` / `PMOVE` (start and result of `BlockOnCommand(Move_To(Target))`).
  The generic PFAIL wrapper already records `MainForce_Production_Failed`.
* `--saboteur` builds (`run_a2f_sab.cmd`): channel **U** (`a2fb_saboteur.lua`, the Underworld AI plot's `Lua_Script`, the
  slot channel D uses otherwise; channel D is left out) samples every Underworld_Saboteur_Team every 30 frames in 0..1400
  and the two desire equations per HotM planet every 30 frames in 0..600; the Underworld free store records `FSAB`
  (`Is_Object_On_Free_Store`, `Is_Unit_In_Transit`, every 8 s), `FADD` (admissions) and `FSVC` (`On_Unit_Service`); the
  story script shows "saboteur run recorded" at frame 1410.
* report section 9 and `saboteur.tsv`. Selftest: 197 checks.

**a2k t05 fix (2026-11-08): plan records survive a pool release.** AI plan scripts are pooled; releasing an instance
(every plan end) runs PGBase `Flush_G`, which deletes every global table (`docs/re/LUA_SCRIPT_OUTPUT.md` section 4).
The kit kept `A2FB_Records` / `A2FB_Errors` and read `A2FB_SPEC` / `A2FB_COMMON` as globals, so at the next start of a
released instance `A2FB_Say` raised, and the wrapper's unprotected `A2FB_Error` then raised again (`A2FB_Errors` was
nil): **that killed the plan's task-force thread before the shipped code ran.** So, contrary to the earlier note, the
hooked plans WERE disturbed in every run before this fix (20261105-*, 20261106-040725, 20261106-074145): the second and
later runs of each pooled instance of the 12 hooked plans did nothing (the "bare PSTART lines with no sample" of run
040725 are the orphaned PSTART strings of those runs, caught by the scan before the GC freed them). Treat those runs'
AI production / credits after each hooked plan's first end as disturbed. Now the record and error tables and the
references to the spec tables live in a closure (`a2fb_core.lua`, read through `A2FB_Get` / `A2FB_Spec` /
`A2FB_Common`), every error path in the plan wrappers is in `pcall`, and `ScriptExit` / `BlockOnCommand` are wrapped once
per instance. The selftest runs the real DeploySaboteur thread, the real PGBase `Flush_G` + `Base_Definitions` (a
release) and the thread again (`sab plan flush`, 5 checks; with the old Lua it fails with `attempt to index global
'A2FB_Errors'`). Selftest: 202 checks.

## 1. What it records

Retail 1.121 writes no Lua output files, so the probe Lua keeps its records as strings
`A2FB1|<channel>|<faction>|<seq>|<KIND>|<text>|END` in a table, and `a2f_run.py` finds them with a read-only heap scan of the
running game (the pattern of the a1h / q2 kits). Three Lua channels plus one Python watcher:

| Channel | Where it hooks | What it samples |
|---|---|---|
| S (story) | a plot script added to the three HotM story plots (`Lua_Script A2fb_Story`), ServiceRate 0.01 | every frame for the first 4 s of galactic time (before and just after the AI's budget Init), every frame in a window around each of the first four fiscal payments (44.5 s .. 47.5 s, 89.5 s .. 92.5 s, ...), a full sample every 1 s until 120 s and every 5 s until 300 s, then it goes quiet |
| F (free store) | wraps `FreeStoreService` of GalacticFreeStore.lua at its shipped 8 s schedule (no rate change) | one full sample per call, at most 200 |
| P (plans) | wraps the task-force thread of 12 plans: BuildBaseComponentBarracks, BuildBaseComponentLightVehicleFactory, BuildBaseComponentResearchFacility, UpgradeStarbasePlan, BuildEconomicStructurePlan, EmpireAdvanceTechPlan, and since a2h BuildBaseComponentHeavyVehicleFactory, BuildBaseComponentAdvancedVehicleFactory, LiftBlockadePlan, FlushMajorItemBudget, ConquerPiratePlan, since a2k t05 AI_Plan_Underworld_DeploySaboteur; also ScriptExit and `<force>_Production_Failed` (when the script has it) | `Budget.Get_Spendable_Resources` and `Get_Unallocated_Resources` at plan start (PSTART), then a small sample (full for FlushMajorItemBudget); the same fields at the end (PRET / PEXIT / PFAIL); DeploySaboteur also goal / target / stage / block results (section 10) |
| U (saboteur, `--saboteur` only) | the Underworld AI plot's `Lua_Script` (`A2fb_SabU`), ServiceRate set per sample frame | Underworld_Saboteur_Team objects every 30 frames in 0..1400, desire equations per planet every 30 frames in 0..600 (section 10) |
| watcher | `a2f_watch.py`, a thread of `a2f_run.py`, ReadProcessMemory only (never writes, injects or suspends) | the budget block of each AI player: init byte, income/s, last tick period T, reset flag +0x74, refund accumulator, polled about every 3 ms, with the mode frame of every change |

Per sample (Lua, per AI player; the human is skipped): credits, `CreditsUnnormalized` (CU), net income, and
`BudgetFractionToBuild` readings for ALL / Infrastructure / Offensive (small sample, SM) plus Defensive / Information /
Major (BF) and the shipped weight / trigger equations (SH) in a full sample. Empire samples (small and full, every channel)
also carry the MJ record (a2h t02, keys in section 5).

## 2. What is reachable from Lua, and what is not (read this first)

* Not reachable: the reset flag +0x74, the per-category pool fields, the budget template pointer and the "initialised" byte.
  The Lua Budget API has no query for them (`docs/re/LUA_API.md`; the Budget calls are plan / goal level:
  `Budget.Get_Spendable_Resources`, `Get_Unallocated_Resources`, `Allocate_Resources`, ...). So "the flag" cannot be read in Lua.
* Pools are therefore decoded, not read. The probe adds 38 equations (47 since a2h t02: the 9 MJ ones) to a mod copy of the galactic equation file
  (`A2FB_CreditsUnnorm`, `A2FB_NetIncome`, and `A2FB_<EMP|REB|UWD>_BFB_<k>_<CAT>`: `BudgetFractionToBuild(type, category)` for a
  probe type with two different build times `b1`, `b2`; 46 since a2i). With D = R / v (v = the returned number, R = the type's cost), the
  rate is (D1 - D2) / (b1 - b2) and the available pool is D1 - b1 * rate. v >= 1e17 means D <= 0: the category has no pool
  ("empty"). The ALL row is D = (b x income/s) + player credits, so it stays finite before Init (available = credits, rate 0).
  Before the AI's budget Init every category row is 1e18 and CU is 0; after Init the pool is credits x normalised weight, and
  zero-weight categories stay "empty".
* Template state is inferred: the measured normalised pool split is compared with the shipped weight equations (the base
  template weights, wOFF/wINFO/wINFRA/wMAJOR for the Empire, and the Tech_Upgrade ones, tOFF.. ) in the SH fields. The L1
  distance per candidate is in the report. The template pointer itself is not read.
* The reset flag +0x74: only the Python watcher can see it. It follows
  `player +0x360 -> AIPlayer +0x38 vector -> context with mode 0 -> +0x40 goal system -> +0x190 budget` and decodes
  +0x68 income, +0x70 T, +0x74 flag, +0x80 refund. Offsets are [H]/[M] from `docs/re/AI_GALACTIC_2.md` section 3 and
  A2GAL_READINGS; the chain may fail on this build (it then logs `chain` events saying where, and the report says so). The
  flag is only observable as a transition ("flag" events with the mode frame), correlated with the Lua frames; the probe
  cannot force a template switch, so a flag that never moves in 5 minutes is itself a result ("not set in the first four
  fiscal cycles with the base template").
* The raw 0x90 budget block is stored (hex) in every state line, so the per-category fields can be decoded offline later
  without a re-run.

## 3. Run it (Gabriel)

1. Steam running, FoC not running, nothing else using the keyboard.
2. Double-click `tools\probe_mod\a2f_budget\run_a2f.cmd` (or run it from a shell). It backs up your profile (registry
   `HKCU\Software\Petroglyph\StarWars FOC` plus Saved Games Save/Recordings), builds and installs `corruption\Mods\A2fBudgetProbe`,
   and launches the game with `MODPATH=Mods\A2fBudgetProbe`. It prints the steps again:
3. Main menu > Single Player > Galactic Conquest > "Heart of the Maw". **Faction REBEL**, difficulty Medium, Start.
   (Empire as the human has no AI budget to look at.)
4. Close the "Active Story Objectives" box. "A2FB PROBE: story script running" shows (blank if the text patch could not be
   applied: the run status still tells you the canary state). Give no orders, do not pause, keep the window in front (FoC
   pauses on focus loss). Normal game speed (the earlier runs used it; galactic time is what counts, not wall time).
5. At 5 minutes of game time "A2FB PROBE: 5-minute dense phase done" shows: keep going. When the AI attacks one of your
   planets and the battle prompt appears, pick auto-resolve (the prompt pauses galactic time; a tactical battle leaves
   galactic mode). At 25 minutes "A2FB PROBE: 25 minutes recorded" shows (record LONGDONE; the a2h t02 MajorItem question
   needs at least 20 minutes). Then Esc > Quit to the main menu > Quit. Do not save. (The old 5-minute run is still valid
   for the a2f t07 questions: quit after the first message.)
6. The run finishes by itself: report, cleanup, profile restore, `A2F_RUN_DONE exit=<n>`. Send back `tools\probe_mod\a2f_budget\out\<run>\`
   (or at least `report.txt`, `records.txt`, `budget_watch.jsonl`).

Options (pass to `run_a2f.cmd`): `--windowed` (computer-use driven run), `--no-launch` (install and scan only; start the game
yourself with `StarWarsG.exe MODPATH=Mods\A2fBudgetProbe`), `--no-mod-equations` (see 6), `--no-profile`, `--non-interactive`,
`--game DIR`, `--scan-sec S`, `--report-only <run dir>` (rebuild the report from a saved run, no game).
Exit codes: 0 records found and report written; 10 no records; 20 preflight (game missing or already running); 30 interrupted; 31 error.

## 4. Cleanup and profile restore

`a2f_run.py` always cleans up at the end (also after an error or Ctrl+C). By hand: `cleanup_a2f.cmd`. It deletes
`Mods\A2fBudgetProbe` only if it contains `A2FB_PROBE_MARKER.txt` (nothing else is touched; no retail file is modified),
restores the profile snapshot (registry options and the Saved Games files), verifies it, and moves files the run created into
`out\<run>\profile_new\` (nothing is deleted). It does nothing while StarWarsG.exe is running, and a snapshot that was already
restored (`profile_restore_done.txt`) is never applied again, so an old backup cannot overwrite newer settings.
`--dry-run` shows what it would do.

## 5. Reading the result

`out\<run>\report.txt` (also `report.json`, `samples.tsv`):

1. Scripts that ran (the canary: `_CustomScriptMessage` deletes the kit's canary file on its first call, so a missing file
   proves the script loaded), per channel.
2. Per faction: the first / last sample frame before Init (OFF and INFRA both "empty"), the first frame with real pools, the
   decoded pools (available credits and rate per category), and the shipped / trigger equations over time.
3. Template: at the first full post-Init sample the L1 distance of the measured split to each candidate (0 = that template's
   weights). Expected from the shipped data: Empire base template gives OFF 8 / INFO 2 / INFRA 6 / MAJOR 4 of 20 (pools 5200 / 1300 /
   3900 / 2600 on 13000 credits), Defensive empty.
4. Pre-Init: the question "what is the Empire's Infrastructure pool before Init" is answered by the samples of frames
   0 .. first_postinit-1: expected INFRA "empty" (1e18) with CU 0 and the ALL pool equal to the credits.
5. Watcher: `found` (pointers, init byte), the `state` timeline, and every `flag` event (from, to, mode frame). The
   fiscal-window samples show what the pools did around each payment.

a2h t02 additions (`report.txt` section 2 end and section 5; `plans.tsv`, `major.tsv` next to it):

* Section 2, "per plan": per plan and faction the starts, how many ended with PEXIT / PFAIL / PRET, and `no_end` (no end
  record: the goal system released the plan and killed its thread, or the record was lost from the scan; `confirmed` = the
  next start of the same script instance says `prev=none`, so it was a release). For a single-structure plan (LVF / HVF /
  barracks): PEXIT without PFAIL = the structure was built (thread: AssembleForce, Set_Plan_Result(true), ScriptExit);
  PFAIL then PEXIT = production failed; no end = released (the time-drop loop of A2G_T02_T04 section 2). PEXIT's `by=` /
  `line=` (debug library permitting) say which ScriptExit fired, `age=` how long the run lasted. ConquerPiratePlan and
  LiftBlockadePlan runs are listed one by one with their ends. `plans.tsv` has one row per start.
* Section 5, MajorItem: how many Empire samples had each trigger > 0 (NMS, NTF, CRM, HES, AGM, HDS) with the first / last
  frame, every change of those flags with all MJ values, and the FlushMajorItemBudget runs with the MajorItem pool before
  (PS) and after (PE) the flush. Reading it: `NTF` > 0 means the Flush_MajorItem_Budget goal function is on; whether the goal
  then ran is the FlushMajorItemBudget PSTART list (that plan serves both Flush_MajorItem_Budget and
  Reclaim_Excess_MajorItem_Budget: the CRM / AGM values in its PS sample tell which). `NMS` = `HST > SST`, so HST / SST show
  why it is 0 or 1; `skip` = no human player (Variable_Human), `nil` / `ERR` = the engine gave no value. `AG` vs `AGM` was
  the port's open ActiveGoals question (A2g F1: the port counted all goals). Run 20261106-040725 answered it with `AG=nil`,
  so a2i dropped `AG`; runs from a2i on have `AGM` only. `major.tsv` has every MJ sample.
* `tools/analysis/a2g_port_vs_retail.py major --retail-records out\<run>\records.txt [--port-log LOG]` prints the same
  trigger summary and compares the flush / reclaim plan starts with a port `--ai-log`; `plans` also counts the retail ends.

**ERR records (a2i).** `A2FB1|<ch>|<sid>|<seq>|ERR|<where> <message>|END`, one per distinct message per script instance;
`report.txt` lists the first 8 under "Lua errors:" (the channel first, then the text). `<where>` is the record kind whose text
failed (`PL`, `SM`, `BF`, `SH`, `MJ`, `LOAD`, `DEF`, `ENTER`, `FIRST`, `DONE`, `LONGDONE`): that record is missing and the
rest of the sample and of the round is still written. `Sample <Faction>` means a story sample failed outside a record,
`Update` is the story's whole service, and `FSSample`, `PlanStart`, `PlanRet`, `PlanExit`, `PlanFail` are the free-store
and plan wrappers. The message is Lua's own, with the chunk position (`A2fb_Story.lua:<line>`, a line of the generated
script). An ERR record means a Lua error in the kit (a binding that raised, or a nil in the text), not an equation without
an answer: that shows as `nil` in its key. Retail gives no ERR for an equation that fails inside the engine, because the
script just stops (see section 8). The signature of that is a channel whose sequence numbers stop, or a plan instance
whose later `PSTART` lines have no sample after them.

If nothing came back: check `run_status.txt` (canary files gone = scripts loaded). No canary at all means the mod folder was
not picked up (check `MODPATH=` and that the game started from `corruption`). Records but all pools "unknown"/empty and
`modeq_working` false in the report means the game ignored the mod's equation file: re-run with `--no-mod-equations`, which
keeps the Lua samples of credits, `Budget.Get_*` at plan start and the shipped equations (the equations that already exist) and
leaves the watcher (the flag and the raw budget block) unchanged.

## 6. Files

| File | Role |
|---|---|
| `run_a2f.cmd`, `a2f_run.py` | one run: snapshot, build, launch, scan, watcher, report, cleanup, restore |
| `cleanup_a2f.cmd`, `a2f_cleanup.py` | remove the mod, restore the profile |
| `build_a2f_mod.py`, `a2fb_spec.py` | build `Mods\A2fBudgetProbe` (patched equation file, 3 story plots, plan and free-store hooks, canaries, marker, optional text patch); the single table of probe types, equations and shipped equation names |
| `a2fb_core.lua`, `a2fb_story.lua`, `a2fb_freestore_hook.lua`, `a2fb_plan_hook.lua` | the probe Lua (Lua 5.0.2: no `#`, no `%`, no math library in the free-store script) |
| `a2fb_percept.lua`, `a2fb_scoring_hook.lua` | a2j: channel D (AI plot scripts, D4 perception) and channel Q (GameScoring production events) |
| `a2fb_saboteur_plan.lua`, `a2fb_saboteur.lua`, `run_a2f_sab.cmd` | a2k t05: the DeploySaboteur plan extension (channel P), channel U (Underworld AI plot), the short-run launcher (section 10); the free-store part is in `a2fb_freestore_hook.lua` |
| `a2j_port.py`, `port_a2j_patch.py` | a2j: the port side (export a commit, trace patch, build, run the scenario, write the D4 table) |
| `port_reference_a2j.tsv` | a2j: the port's D4 values (commit and method in its first lines; section 9) |
| `a2f_watch.py` | read-only memory watcher of the budget block |
| `a2f_report.py` | decoder: `records.txt` + `budget_watch.jsonl` + `A2FB_TYPES.json` -> report |
| `selftest_a2f.py` | offline self-test |
| `out\<run>\` | records.txt, budget_watch.jsonl, report.*, samples.tsv, major.tsv and plans.tsv (a2h), run.json, run_status.txt, cleanup.log, profile_backup\, profile_restore.json |

## 7. Offline self-test

`toolchain\venv\Scripts\python.exe -I selftest_a2f.py [--lua PATH] [--out DIR]`. 202 checks since the a2k t05 pool-release fix (197 at a2k t05, 140 in a2j, 101 in a2i, 83 in a2h, 55 until a2g).
a2k t05 adds: the saboteur spec (the shipped DeploySaboteur thread and handler, AssembleForce's global `stage`, the reads
the shipped free store and AI scripts make themselves, the saboteur type, HotM's planets and starting saboteur, the two
desire functions, Enemy-only goals, closures without Variable_Human or `#`); the `--saboteur` / `--no-sab-desire` builds;
the DeploySaboteur thread driven as a coroutine under the real PG library (AssembleForce, BlockOnCommand, Sleep,
ScriptExit) in three cases (normal, Produce_Force returns nothing, Production_Failed): the records, and the **same
orders at the same frames** as the unhooked shipped script; channel U under the model engine (cadence, owner filter,
transit, a saboteur in a transport, desire only on enemy planets, no `UD` without desire); the free-store wrappers
(shipped handlers still called with the same object, Underworld instance only, nothing in a normal build); SABDONE in
the story script; report section 9.
a2j adds: the D4 spec (every shipped D4 equation is Should_Conquer_Pirate_Planet or one of its sub-functions, every token
equation is verbatim a token of them, the needs-human marks match the Variable_Human closure, the AI plot files have no
Lua_Script, GameScoring has the three events and makes the reads the Q hook makes); the build (AI plot files, queue
types); both D scripts under the model engine (cadence 0..600 / 30 then / 300, every key, nil before the AI Init, `skip`
without a human, and a fault-injection run where a token fails: failed flag, one complete record with `MK=nil`, then
that script stops); the GameScoring hook on the real script (records, the shipped End handler still runs); the report
sections 6 - 8 against a port table and a synthetic port trace. Earlier checks: the spec (shipped equations exist,
types, plan thread functions, the patched XML has 46 equations, none breaks a retail rule); a build from the extracted game-data zip; the Lua syntax of every
generated file and a run of the story script under the project's Lua 5.0.2 with the real PG library and a model engine (state
machine, canary, per-frame start sampling, dense fiscal windows, the human excluded); the free-store hook and the real plan
scripts with their hooks (a2h: all 11 plans with PSTART n= / prev=, PRET, PEXIT with by= / line=, PFAIL where the script
has the handler, the Flush plan's full start / end samples; MJ with every Empire sample; the 30 s samples after DONE up to
the 25-minute LONGDONE); the watcher on a fake memory chain (flag 0 -> 1 at frame 4, 1 -> 0 at frame 6); the cleanup; and the
report on a synthetic run (pre-Init ALL = credits, pools solved to 13000 / 3900 / 5200, template = base). Since a2i the
model engine emulates retail: an unknown equation, or a probe equation that breaks a retail rule, sets a script-failed flag
(`SCRIPT_FAILED <name>` in the test output). The drivers then stop servicing that script, and every Lua test checks that the
flag stays clear. `math` is nil in every test environment and `debug` in the story and free-store ones. A fault-injection
story run (`robust_test`) checks that one failing record gives one ERR record and costs nothing else. The Lua part needs
`lua50.exe`: it is auto-detected at `tools\probe_mod\out\tools\lua50.exe`; build it with
`tools\probe_mod\common\build_lua50.cmd`, or pass `--lua`. Without it the Lua checks are skipped, not passed.

## 8. Known limits

* Unproven in game: that the engine loads a mod copy of the galactic equation file (the mod override of
  `Data\XML\...` equation files); `--no-mod-equations` is the fallback.
* **An equation that fails inside the engine stops the script that evaluated it. pcall does not help** (measured in run
  20261106-040725). EvaluatePerception returns nil and the script error helper sets the script's failed flag, but raises no
  Lua error. Retail then stops the script: the story channel (only `LOAD` / `DEF` were found; the rest of its records were
  lost from memory too), the Empire's free-store instance after its first sample, and each Empire plan instance after its
  first sample (its later `PSTART` lines have no sample after them, with no ERR). Equations seen to fail: `ActiveGoals`
  with no `Parameter_Category`. Expected to fail by the same mechanism: an unknown equation name, and a `Variable_Human`
  equation with no human player (the kit writes `skip` for those). Rule for kit changes: evaluate only equations that have
  answered in retail, and add a rule to `retail_eval_fails` in the selftest for every new failure found. Not every `nil` is
  a failure: "AI not yet initialized" is a warning, also nil, and it does not stop the script (the a2g run's pre-Init samples).
* pcall per record (a2i) covers Lua errors only: a binding that raises, or a nil in a record's text. They give one `ERR`
  record per distinct message, and the channel goes on (section 5, "ERR records").
* Library availability in retail: the free store has no `math` (s78g). For the story and plan scripts it is not measured.
  No shipped script uses `math` (PGBase has its `math.floor` lines commented out), and the kit uses no `math`, `#`, `%` or
  `string.rep` anywhere, and `debug` only in the plan hook's PEXIT `by=`, guarded. The selftest removes them as described in
  section 7.
* The BFB decode assumes no item of the probe type is already in progress and that the build-time / R definitions are as
  recovered ([M]); an "ok" pool is exact only under that assumption (the report prints D1 and D2 so it can be rechecked).
* Watcher offsets are [H]/[M]; the per-category budget fields are not decoded (raw block only).
* The setter of +0x74 is only seen as a transition within about 3 ms and correlated to a mode frame; the probe cannot force
  a template switch (Tech_Upgrade), so the setter's trigger is inferred from what changed at that frame (SH trigger values,
  the template split, a plan starting).
* The free store is sampled only every 8 s; the story script covers the time between.
* Heart of the Maw as Empire (human) leaves the Empire without a budget; the kit samples whichever AI factions exist.
* A real run needs the player at the PC: the game pauses on focus loss.
* Plan-channel records can be missing from `records.txt`: in run 20261105-195851 the plan scripts' sequence numbers have
  gaps (an LVF instance: 20 of 39 records found) while the story and free-store channels have none. Cause unknown (the
  records are kept in the script's table). So a2h's `no_end` count is an upper bound for releases; `confirmed`
  (`prev=none` on the next start) and the orphan count (end records whose start is missing) show how much was lost.
* PEXIT `by=` / `line=` need the Lua debug library in plan scripts; without it the record says `by=nodebug`. Line numbers
  are lines of the shipped plan script (it comes first in the mod copy).
* MJ: `Needs_More_Space_Force_Globally`, `Need_To_Flush_MajorItem_Budget` and `A2FB_HumanSpaceTotal` use Variable_Human and
  are `skip` without a human player. A mod equation whose token fails in retail does not just show nil in its key: it stops
  the script (first bullet of this section).
* a2j D (perception): unproven in game that an AI faction's story plot runs its `Lua_Script` (STORY / A2I_T01_FRAME_ORDER
  T9: every active plot gets its Lua service [M]); the `PerceptU` / `PerceptE` canaries tell. If they stay present, the
  D channel did not run and nothing else is affected. Why each D read is safe: `docs/re/A2J_T02_PROBE_PLAN.md` section 3.
  Should_Conquer_Pirate_Planet, Is_Good_Invasion_Target and Is_AI_And_Should_Be_Ignored reach Variable_Human: `skip`
  without a human player (a Rebel-human run has one). Safe is not the same as without effect: the D evaluations share
  the AI's leaf cache and can shift its refreshes (section 9, "Probe effect"; `--no-percept` leaves channel D out).
* a2j Q (production): Begin may mean "queued" rather than "started"; the report's queue rebuild shows which (a Begin while
  the queue is busy). Retail calls GameScoring's production events for every player [M: the names are exe strings; the
  q2 kit saw GameScoring load in galactic]; the `Scoring` canary only proves the script loaded.
* a2k t05 (saboteur, section 10): no hidden-fleet read, no proposal / accept / effective-desire read, no desire inside
  the plan; `Is_Unit_In_Transit` / `Is_Object_On_Free_Store` only every 8 s and at free-store events (free-store
  methods); `Is_Object_On_Free_Store` is unproven in retail; the `UD` desire reads may shift the AI's cache refreshes
  (`--no-sab-desire`). Canary `Saboteur` still present after the run = the U plot script did not run.

## 9. How to run (a2j)

One in-game run serves a2j t02 (D4 perception, D3 production) and t04 (30 minutes, per-cycle P / I against the port).
The coordinator asks Gabriel first; nothing here launches the game by itself except step 2.

1. Offline check (no game): `toolchain\venv\Scripts\python.exe -I tools\probe_mod\a2f_budget\selftest_a2f.py` must end with
   `A2F SELFTEST OK (140 checks, 0 failed)`. A dry build that does not touch the game folder:
   `toolchain\venv\Scripts\python.exe -I tools\probe_mod\a2f_budget\build_a2f_mod.py --out <scratch>\A2fBudgetProbe`.
2. Install, launch, record, report, clean up (one command; Steam running, FoC closed):
   `tools\probe_mod\a2f_budget\run_a2f.cmd` (add `--windowed` for a computer-use run, `--no-launch` to start the game by
   hand with `StarWarsG.exe MODPATH=Mods\A2fBudgetProbe` in `corruption`). It backs up the profile, builds and installs
   `corruption\Mods\A2fBudgetProbe`, launches the game, scans for records every 5 s and runs the budget watcher.
3. In the game: Single Player > Galactic Conquest > "Heart of the Maw", faction **REBEL**, Medium, Start. Close the
   "Active Story Objectives" box. Then **hands off for 30 minutes of galactic time**: no orders, no pause, normal speed,
   keep the window in front (FoC pauses on focus loss). When a battle prompt appears (the AI attacks a Rebel planet),
   choose **auto-resolve** (the prompt pauses galactic time; a tactical battle would leave galactic mode). Messages:
   "story script running" at the start, "5-minute dense phase done" at 300 s, "25 minutes recorded" at 1500 s (keep
   going), "30 minutes recorded" at 1800 s: then Esc > Quit to the main menu > Quit, do not save.
4. Watching: the console prints `+N records (... total)` while it scans; `tools\probe_mod\a2f_budget\out\latest.txt`
   names the run folder and its `run_status.txt` the state. The run ends by itself after the game exits: report,
   cleanup, profile restore, `A2F_RUN_DONE exit=<n>` (0 = records and report; 10 = no records).
5. Cleanup by hand only if the run was killed: `tools\probe_mod\a2f_budget\cleanup_a2f.cmd` (removes the marked mod folder,
   restores the profile snapshot; `--dry-run` shows what it would do).
6. Port side and the comparison (no game):
   * the port reference of the same scenario (export of a commit, traces, build, 30-minute run, D4 table):
     `toolchain\venv\Scripts\python.exe -I tools\probe_mod\a2f_budget\a2j_port.py all --dir <scratch>\x --out <scratch>\r54000 --ticks 54000 --commit <C>`
     (`--control` on `run` repeats the run without the A2J perception trace; see "Probe effect" below);
   * the report with all sections: `toolchain\venv\Scripts\python.exe -I tools\probe_mod\a2f_budget\a2f_report.py tools\probe_mod\a2f_budget\out\<run> --port-trace <scratch>\r54000\port.err`
     (`--port-perception F` for another port table; the default is `port_reference_a2j.tsv` in this folder). Or
     `run_a2f.cmd --report-only out\<run> --port-trace <...>`.

**What to send back:** the whole `tools\probe_mod\a2f_budget\out\<run>\` folder, at least `records.txt`, `report.txt`,
`perception.tsv`, `production.tsv`, `samples.tsv`, `budget_watch.jsonl`, `run_status.txt`, `run.json`. Also say whether
any battle prompt was not auto-resolved, and the galactic time reached.

**Reading it.** Report section 6: per pair, the D4 terms per sample frame, retail | port. The question (A2I_T03 section
5) is whether retail's markup (`MK`) on Kashyyyk rises like the port's (0.293 at frame 240, 0.618 at 270, 1.012 at 390)
and whether retail's `SCP` turns positive; `CIP`, `ONF` and `ACT` are the other non-zero terms of the port's desire
(A2I_T03 section 5.1). A D channel that
stops early with a `nil` key names a term that fails in retail. Section 7: every AI player's production events in
0..1500 with the owner's credits when the event ran (`cr=`), the first Begin per (player, planet, queue), and the Empire's
queue heads in 0..1400 (D3: what is building at Tatooine, Kuat, Corellia and Kashyyyk, ground and space; D5: which item
each queue starts and when). Section 8: the P / I of OFF / INFRA / MAJOR per fiscal cycle against the port.

**Probe effect (found 2026-11-06).** Channel D's `EvaluatePerception` calls go through the same per-token leaf cache as
the AI's own evaluations (AI_TACTICAL_PERCEPTION section 1.2: one cache per token, keyed by the bindings, entries
stamped with the frame computed; EnemyForce totals live 15 s). So a probe evaluation can change when the AI's cached
value is refreshed. In the port this changes the run. At 5509c2a, a run with the A2J perception trace has the same
ai-log as one without it up to frame 1460. Then two plans started in that frame swap order, and the trajectory drifts:
final CRC 7cf652411ed4a322 against 5d3a792a7601b351 at 54000 ticks. The extra equation file alone changes nothing.
In 2000-tick runs, Is_Neglected_By_My_Opponent_Space alone does it (and so SCP / IGI, which contain it), and so does the
token set alone (the EnemyForce.Has* leaves). Retail is expected to behave the same way [M], so:

* D4 and the 30-minute run: compare the retail run with channel D against the traced port run (`a2j_port.py run`, the
  default). Both carry the same probe effect, with the samples at the same frames. Frames 0..1460 of the port are
  unaffected.
* An unperturbed t04 budget comparison, if wanted: a second 30-minute run with `run_a2f.cmd --no-percept` (channel D left
  out, all else the same), against `a2j_port.py run ... --control`.

**Port reference `port_reference_a2j.tsv`.** Commit 5509c2a (`origin/claude/a2i-followups-4`, A2 i), exported with
`a2j_port.py` and traced with `tools/analysis/a2g_trace_patch.py` + `port_a2j_patch.py` (scratch copy only, the
repository's engine is unchanged); scenario `Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel, `--economy --galactic-ai --story
--seed 1 --workers 0 --ticks 54000`; final CRC 7cf652411ed4a322. The port evaluates each pair right after that AI
player's own galactic AI service of the frame (retail's plot script runs at T9 of the same tick, after every player's
AI service: a difference of at most the later players' service in that frame). The port table has no frame-0 row (the
trace saw no AI service at frame 0); retail's frame-0 sample is expected to be `nil` (AI not initialised yet).

## 10. a2k t05: the Underworld saboteur short run (Q5 of `docs/re/A2K_T08_RE_ANSWERS.md`)

**Question.** In retail the starting saboteur (NalHutta) corrupted Naboo at about frame 600; in the port it is staged at
Felucia for the MonCalimari proposal and dies at Kashyyyk. Q4.2: H1 = retail's desires rank Naboo first (or MonCalimari is
not proposed); H2 = the MonCalimari plan ends early, the saboteur returns to the free store and Naboo's build task claims
it. The run also shows Q1 (fleet members after the stage arrival), Q2 (what Move_To moves) and Q3 (`Get_Stage`).

**Scenario.** `Sandbox_Heart_Of_The_Maw_Rebel` ("Heart of the Maw", faction REBEL, Medium), human Rebel with no orders,
story on (the campaign's own story plots, as always), frames 0..1400 (about 47 s of galactic time).

**Run it (Gabriel, Steam running, FoC closed).**
1. Offline check: `toolchain\venv\Scripts\python.exe -I tools\probe_mod\a2f_budget\selftest_a2f.py` ends with
   `A2F SELFTEST OK (202 checks, 0 failed)` (the Lua checks need `lua50.exe`, section 7).
2. Double-click `tools\probe_mod\a2f_budget\run_a2f_sab.cmd`. It opens its own console window (`start`), so the recorder
   does not depend on the shell that launched it. From a Claude session start it detached, for example
   `Start-Process -FilePath "<repo>\tools\probe_mod\a2f_budget\run_a2f_sab.cmd"` (PowerShell), never in a Claude terminal
   tab (a2j: the recorder died with the session). The window runs `a2f_run.py --saboteur`: profile backup, build and
   install `Mods\A2fBudgetProbe` with `--saboteur`, launch, scan, report, cleanup, profile restore. Options are passed on:
   `--windowed`, `--no-launch`, `--no-sab-desire`, `--game DIR`, `--non-interactive` (no pause at the end).
3. In the game: Single Player > Galactic Conquest > "Heart of the Maw", **REBEL**, Medium, Start. Close the "Active Story
   Objectives" box. Hands off: no orders, no pause, normal speed, window in front.
4. At frame 1410 "A2FB PROBE: saboteur run recorded" shows (record `SABDONE`). Esc > Quit to the main menu > Quit; do not
   save. The window then writes the report, cleans up and restores the profile, prints `A2F_RUN_DONE exit=<n>` and waits
   for a key. `tools\probe_mod\a2f_budget\out\latest.txt` names the run folder, its `run_status.txt` the state (also after
   a Claude session cut).
5. Cleanup by hand only if the window was killed: `tools\probe_mod\a2f_budget\cleanup_a2f.cmd`.
6. Send back `out\<run>\` (at least `records.txt`, `report.txt`, `saboteur.tsv`, `plans.tsv`, `run_status.txt`).

**What it records** (all read-only; Q5 items):

| Q5 item | Record | Content |
|---|---|---|
| 1 PSTART | P `PSTART` of AI_Plan_Underworld_DeploySaboteur | frame, `n=` (run number of the script instance, with the channel's sid = plan id), `goal=`, `tgt=`, `tgtc=` |
| 1 stage | P `PSTAGE` | `stage=` the planet AssembleForce got from `MainForce.Get_Stage()` (the script's own global `stage`; no extra call), `blk=` type of the Produce_Force status (`nil` = no status) |
| 2 progress | P `PPROD`, `PMOVS`, `PMOVE` | `res=` BlockOnCommand's result for Produce_Force(stage) and Move_To(Target) (true / false / nil), `dt=` seconds blocked; PMOVS also `stage2=` (Get_Stage again), `units=` (Get_Force_Count) |
| 2 failed | P `PFAIL` (+ `PEXIT` from its ScriptExit) | `MainForce_Production_Failed` fired, with `tgtc=` |
| 3 end | P `PRET` / `PEXIT` / `PFAIL` | result path (`PRET` = thread returned after Set_Plan_Result or not; `PEXIT` = ScriptExit, `by=` / `line=`), `tgtc=` (`Target.Is_Corrupted()`), `blk=` block calls done; no end record = the goal system released the plan |
| 4 every 30 frames | U `UN`, `US` | per Underworld_Saboteur_Team the Underworld owns: `id=` (Get_Object_ID), `type=`, `pl=`, `par=` / `parid=` / `cnt=` / `parpl=` (parent: the fleet, or a transport, then `gp*` = the fleet); `parpl=nil` = in transit |
| 4 store | F `FSAB` (8 s), `FADD`, `FSVC` | `st=` `Is_Object_On_Free_Store`, `tr=` `Is_Unit_In_Transit` (read only for a store unit), admissions (every type), `On_Unit_Service` calls (unreserved entries; `safe=`, `tp=`) |
| 5 proposals | U `UD` (every 30 frames, 0..600) | per HotM planet not owned by the Underworld: `CP=` Should_Corrupt_Planet, `SP=` Should_Sabotage_Planet (the two goals' desire functions), `own=`, `cor=` |

**Not covered, and why** (do not read these from the run):
* **Hidden fleet**: no Lua binding reads a fleet's hidden / stealth state (LUA_API.md). Indirect evidence: `cnt=` of the
  saboteur's fleet after its arrival (Q1: merged into a visible fleet = cnt > 1).
* **Proposals and "accepted"**: the goal system's proposal list, the effective desire (learning, Per_Failure_Desire_Adjust,
  incumbent bonus) and the accept decision have no Lua API. The `UD` values are the raw desire functions, evaluated
  only for (goal, target) pairs the goal system itself scores; which proposal was accepted shows as the DeploySaboteur
  `PSTART` (goal + target) that follows. The pass timing (about 150 frames per pass, AI_GALACTIC 1.x) is not visible
  either: the 30-frame `UD` samples cover the pass near frame 116 and the next ones.
* **The desire in PSTART**: a plan cannot read its goal's desire; evaluating the equation inside the plan script would
  end the plan if it failed (a binding error stops the calling script), so the report joins PSTART with the last `UD`
  sample of the same target instead (`desire=` in section 9).
* **In transit every 30 frames**: `Is_Unit_In_Transit` / `Is_Object_On_Free_Store` are free-store methods. Raising the free
  store's 8 s ServiceRate would change the AI, so they come every 8 s (`FSAB`) and at the events (`FADD`, `FSVC`);
  channel U's `parpl=nil` is the 30-frame transit signal.
* `Is_Object_On_Free_Store` is registered but no shipped script calls it: unproven in retail. It gets only a valid game
  object; if it failed in the engine it would end the Underworld's free-store script (no ERR, the `FSAB` records stop).

**Probe effect.** The `UD` evaluations share the AI's leaf cache (section 9, "Probe effect"): they may move the AI's own
cache refreshes. The port showed no change before frame 1460 from channel D's reads; for a run without them use
`run_a2f_sab.cmd --no-sab-desire` (channel U keeps the saboteur samples). Channel D is not built in a `--saboteur` run.

**Reading it.** Report section 9: the DeploySaboteur runs (frame, goal, target, desire, stage, produce / move results with
their frames, end, `tgtc` start -> end), the timeline per object id (channel U changes only, every F record), owned-saboteur
count changes, the Underworld's free-store admissions, and the desire table (`CP/SP`, `c` = corrupted). H2 shows as: a
MonCalimari run that ends (PFAIL / PEXIT, or no end = released) without corrupting, then `FADD` / `FSAB st=true` for the
starting saboteur's id, then a Naboo run whose `PSTAGE` / `PPROD` follows and Naboo's `cor=true` at about 600. H1 shows as:
Naboo's `CP` above MonCalimari's at the pass near 116, or no MonCalimari `PSTART`. Compare with the port's trace of the same
scenario (`docs/re/A2K_T08_RE_QUESTIONS.md` section 2: plan 10 MonCalimari 25.5, plan 11 Naboo 24.0 at 116, staged at
Felucia). `saboteur.tsv` has one row per timeline record.

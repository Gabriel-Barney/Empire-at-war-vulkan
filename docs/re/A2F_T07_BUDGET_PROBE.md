# A2f t07: retail galactic AI budget at HotM start (in-game probe, 2026-11-05)

Kit: `tools/probe_mod/a2f_budget/` (README has the method). Two runs on retail FoC 1.121, driven by Claude with computer use:
Heart of the Maw, human = Rebel, Medium, no orders, about 5.5 minutes of galactic time (story channel DONE at t = 303 s, frame 9099).

| Run | Lua records | Watcher | Result |
|---|---|---|---|
| `20261105-191238` | 4499 | failed: `a1mem.Game.players()` returns a dict, the watcher iterated its keys | Lua part valid; the scan loop then died on a non-ASCII heap fragment (exit 31, profile restored by hand with `a2f_cleanup.py`) |
| `20261105-192142` | 4578 | 3 AI budget blocks found, 238 state lines | complete, exit 0, profile restored and verified |

Both kit bugs are fixed in this change (`a2f_watch.py` reads `by_id`, `a2f_run.py` skips undecodable fragments; selftest fake
updated, 28 offline checks pass). The full report of run 2 is `probe_results/a2f_budget_20261105.txt`. The two runs agree to the
last digit on every pool value below.

## 1. Answers

1. **There is no pre-Init window visible to game scripts.** The budget's "initialised" byte is already 1 at mode frame 1 for every AI
   player, and the story script's first sample (frame 0) already sees real pools: Lua never reads the 1e18 "no pool" answer for any
   category. So the original runs budget Init before (or in) the first galactic frame, before any plan or story script can ask. [H]
2. **Empire pools at frame 0** (credits 8000, the HotM AI start, not the 13000 of the menu): Infrastructure 4000.3, Offensive 3105.3,
   MajorItem 894.3 (normalised 0.500 / 0.388 / 0.112), Defensive and Information empty (weight 0). L1 distance to the shipped base
   template weights 0.000, to the Tech_Upgrade template 1.535: the Empire runs the base template from the start. [H]
   Underworld: Offensive 2703.0, Infrastructure 5297.0 (0.338 / 0.662), base template. [H]
3. **The Empire's Infrastructure pool never goes negative** in 5.5 minutes: minimum -0.0008 (spent to zero at frame 2634), then
   334 / 986 / 928 / 488 at the next fiscal cycles. The port's pre-t01 drift to about -4.2k is therefore not retail behaviour; t01's fix
   (the hook answers from the constructor zeros before Init) is consistent with this, and the port's normalised weights in the t05 trace
   (0.408 / 0.485 / 0.107 at fiscal ticks) are close to retail's frame-0 split.
4. **Reset flag +0x74 never moved**: 0 for the Empire and the Underworld over 7 fiscal cycles (mode frames 1 .. 10140), no template
   switch, so its setter was not hit with the base template in the first 5.5 minutes. The trigger equations moved (Empire `UNTRIG`
   0 -> 1, `tMAJOR` 0.8 -> 1.148 at the end) without setting it. A forced per-tick reset in the port (tried in t05) has no retail
   support. [H for "not set in this window"; the setter itself stays unread]
5. Income per second (+0x68) and period T (+0x70 = 45 s) are written at the first fiscal payment (mode frame 1351) and at every
   payment whose income changed (Underworld: every cycle, 47.8 -> 56.6/s; Empire: only the first, 14.83/s, its planets did not
   change). Refund accumulator +0x80 stayed 0. [H]
6. The Pirates player has a context with mode 0, but its "+0x74" reads a constant 0x75F80000 and income 0: not a budget block (the
   Pirates have no galactic budget, or the chain lands on another object for them). [M]

## 2. Plan hook

`BuildBaseComponentLightVehicleFactory` (Empire) started 9 times from frame 1435 on, each with `Get_Spendable_Resources` = 0 and
`Get_Unallocated_Resources` = 500 at plan start; `UpgradeStarbasePlan` (Underworld) once at frame 2794 (0 / 1600). The other four
hooked plans did not start in the window.

## 3. Open

- The setter of +0x74 (needs a run long enough for a template switch, or a Tech_Upgrade).
- Port check: compare the port's Empire pool timeline in HotM-human with point 2 / 3 (the port's human is Rebel there too).

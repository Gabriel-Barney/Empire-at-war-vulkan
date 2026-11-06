# A2g t01: setter of the AI budget reset flag +0x74 (in-game probe, 2026-11-05)

Kit: `tools/probe_mod/a2f_budget/` (README has the method), run `20261105-195851`, driven by Claude with computer use.
Retail FoC 1.121, Heart of the Maw, human = Rebel, Medium. Report: `probe_results/a2g_reset_flag_20261105.txt`.

Changes to the kit for this run: the story script keeps a full sample every 30 s after its 300 s DONE (it used to go quiet),
and the watcher now reads +0x74 as a byte (see 1.1). Selftest: 55 checks pass.

Run: about 20 minutes of galactic time (mode frames 1 .. 36093, days 1 .. 27). Not fully hands-off: the AI attacked the
human's Yavin 4 twice (space, then land) at day 9; both battles were auto-resolved (the prompt pauses galactic time, a
tactical battle would leave galactic mode). Empire took Yavin 4 (income 14.83 -> 18.0 -> 21.3 /s). Game closed with
taskkill from the pause menu (no save); kit cleanup and profile restore verified, exit 0.

## 1. Answers

1. **+0x74 is a byte.** The dword at +0x74 reads 0x16923E00 (Empire) and 0x00000200 (Underworld) in this run, 0x75F80000
   (Pirates) in the t07 run: the upper three bytes are uninitialised padding that differs per run; only byte +0x74 changes.
   The t07 report's "flag stayed 1979188992" for Pirates is this padding, not a set flag. [H]
2. **Setter: a budget-template switch, in either direction.** The Empire's flag byte went 0 -> 1 twice:
   - mode frame **17569**: `Empire_Tech_Trigger` (TRIG) was 0 in the free-store sample at frame 17482 and 1 at 17725, so the
     `Tech_Upgrade` template turned on between them;
   - mode frame **21042**: `Empire_Tech_Untrigger` (UNTRIG) was 0 at 20884 and 1 at 21122, so `Tech_Upgrade` turned off
     (back to the base template) between them.
   TRIG was 1 again from 18940 to 21127 while `Tech_Upgrade` was already active, and the flag was **not** set then; UNTRIG was 1
   from 22342 on while the base template was already the only one, also without a set. So the setter is the change of the
   active template set, not the trigger value. [H for the correlation (two of two switches, no set without a switch); the
   setter function itself is still unread]
3. **Clear: the next fiscal tick**, exactly: 18901 = 1351 + 13 x 1350 and 22951 = 1351 + 16 x 1350 (the first payment is at
   mode frame 1351, T = 45 s). This matches AI_GALACTIC_2 §3.1 ("store T; clear the reset flag"). [H]
4. **Effect at that tick (the reset second pass, P = Σ x w):** the decoded pools jump to the new template's split in one tick:
   - tick 18901 (switch to Tech_Upgrade): MajorItem 2586 -> 4972, Infrastructure 1476 -> 249, Offensive 687 -> 459
     (credits 5633: MajorItem about 0.88);
   - tick 21601 (back to base): MajorItem 5441 -> 533, Offensive 115 -> 2034, Infrastructure 287 -> 2568 (credits 5134).
   Without the reset the base pass only moves the rates, so the pools would have drifted, not jumped. [H]
5. Underworld and Pirates: flag never set in 20 minutes (no template switch). Underworld income changed at every tick
   (47.8 -> 59.4 /s), refund accumulator +0x80 stayed 0 for every player. [H]

## 2. Port status

The port's `AiBudget` (`engine/ai/src/ai_budget.cpp`) only ever clears `reset_` (Init and the tick); nothing sets it, so a
template switch in the port never redistributes Σ. Fix (a2g, engine/ai galactic): set the flag when the galactic AI's active
template set changes (`PlayerAi::templates_changed`), and let the next tick's second pass apply P = Σ x w as the doc already says.

## 3. Plan hook counts (Empire, mode frames 0 .. 36093, about 20 min)

From `records.txt` (PSTART): BuildEconomicStructurePlan 19, BuildBaseComponentBarracks 18, BuildBaseComponentLightVehicleFactory
17, UpgradeStarbasePlan 2; Underworld UpgradeStarbasePlan 6 (the Lua cap is 40 starts per script). Each Empire base-component /
economic start reads Spendable 0 / Unallocated 500. Used by the t04 comparison (`A2G_T02_T04_PORT_VS_RETAIL.md`).

## 4. Open

- The setter function (which code writes +0x74 on a template change) is not read; the behaviour above is enough to port it.
- Whether a switch of a template without budget equations also sets it (HotM Empire has only `Tech_Upgrade`).

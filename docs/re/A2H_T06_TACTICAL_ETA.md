# A2h t06: the production plan ETA and AllowedTime in tactical mode (2026-11-06)

Question: tactical goals in the port carry ETA 0 (a [port] guess, a2g t06), so AllowedTime never limits them. Does retail compute a
nonzero PotentialPlan ETA (PP +0x48 -> goal +0x7C) in land / space battles, what build time does the AllowedTime base 0x6c0450 read
there, and where do the time tests run in tactical mode?

Tags: [H] read from the code, [M] inferred, [L] guess. Addresses are RVAs in StarWarsG.exe (x64). PP = PotentialPlan,
G = instantiated goal. Mode = PP +0x198 (copied at PP Init 0x6c0d00 from the AI player's per-mode manager +0x34: 0 galactic,
1 land, 2 space; AI_GALACTIC_3 §1, AI_GALACTIC §1 [H]). Builds on A2F_RE_READS §1 and A2F_T05_BUDGET_TRACE §7.

## 1. Answer in short

- **In tactical mode the PP ETA is always 0** [H]. Every solve clears it, and only the galactic branch of the ETA step writes it.
  No build queue position, build time, funds time, credits or travel time feeds it in a battle.
- **0x6c0450 reads the galactic `Build_Time_Seconds` (+0x890)** in every mode, never `Tactical_Build_Time_Seconds` (+0xA00) [H].
  Scaling: `Production_Speed_Factor` (truncated, as galactic T0) and the plan player's per-mode difficulty multiplier
  (`Land_Build_Time_Multiplier` +0x30 in land, `Space_Build_Time_Multiplier` +0x2C in space) [H]. Nothing else.
- **The time tests never fail in tactical mode** [H]: accept and Resolve compare AllowedTime with the ETA (0), and the maintenance
  time-drop is skipped outright outside galactic mode. AllowedTime (and Time_Limit for goals without a PP) has no effect on
  tactical goals at all.
- **The port already matches.** Nothing to change beyond comment tags.

## 2. The PP ETA (PP +0x48) in tactical mode

### 2.1 Where it is written [H]
- Solve driver 0x6c3da0 starts with the reset 0x6c3080, which stores a 64-bit zero at PP +0x44, clearing **+0x44 (goal cost
  source) and +0x48 (ETA)** in every mode. The galactic-only part of the reset (`if mode == 0`) also seeds the money fields
  +0x30 / +0x34 / +0x38 from the budget; tactical leaves them alone.
- After each greedy pick, 0x6c3da0 calls the ETA step 0x6bfcf0:
  - plan flag (PP +0x50 -> +0xA8) set [L: a plan-definition flag]: ETA := 0;
  - flag clear and **mode == 0**: ETA := 0x6639f0(queue pool of the target / the player's capital fleet location, the selection with
    its free-store entries nulled, ...), the queue-pool ETA of AI_GALACTIC_2 §3.4;
  - flag clear and **mode != 0**: **no write**. The ETA keeps the 0 from the reset.
- A scan of the decompiled output for writes to +0x48 in functions that also read PP +0x198 finds only 0x6bfcf0; the other PP
  routines that write +0x48 belong to other classes (index cursors, list ends) [H for the scan, M that it is complete: a callback
  through a marshaller vtable could in principle write it, none of those found does].
- The ETA reaches the goal in the three solve entry points, always right after 0x6c3da0: 0x6c0d00 (Init, first solve),
  0x6c47c0 (re-validate) and 0x6c4a50 (re-solve): `G +0x7C := PP +0x48`, `G +0x78 := PP +0x44` [H].

So in a land or space battle **G +0x7C is 0 for every goal with a PP, at accept and after every re-solve** [H].

### 2.2 What does not feed it
- Build queue position, build time, funds time (PAYG `max(btime, fundsTime)` or paid-up sum): these are inside 0x6639f0 / 0x53bb20,
  galactic only [H].
- Travel time: not part of the PP ETA in any mode (galactic 0x6639f0 is queue time only, AI_GALACTIC_2 §3.4) [H].
- The tactical build queue (0x496f00, `Tactical_Build_Time_Seconds` x `Tactical_Build_Time_Multiplier` x difficulty x modifiers,
  SKIRMISH_SETUP) and the build pads (0x5d70f0, L1_S4_REMAINING) are used for actual production only; nothing in the goal / PP
  timing path reads them [H for the callers of 0x6c0450 / 0x6bfcf0 / 0x525200].
- Side effect: the money update in 0x6c3da0 (`if ETA > 0: PP +0x38 := min(ETA, Time_Limit) x rate + available`) never runs in
  tactical [H]. Funding in tactical does not go through it either: the budget checks 0x6129c0 / 0x6139f0 return "fundable" at once
  when the mode is not galactic [H] (port: tactical `uses_budget() == false`, AI_TACTICAL §4.1).

## 3. AllowedTime base 0x6c0450 in tactical mode

0x692860(G, extra) [H]: no PP (G +0x68 == 0): `extra + Time_Limit` (goal type +0x30); else
`(0x6c0450(PP) + extra) x Build_Time_Delay_Tolerance` (goal type +0x34). Every caller passes extra = 0.

0x6c0450(PP) [H]:
- `S` = sum, in selection order, over every entry of the selection PP +0x58 .. +0x60 (free-store and existing units included; no null
  check, the selection holds no nulls) of `btime(T, no planet)`.
- `btime(T, none)` = 0x371230 with a null location: `float(uint32(float(T +0x890) x 0x140b15920))`, i.e. `Build_Time_Seconds` x
  GameConstants `Production_Speed_Factor`, truncated toward zero (GALACTIC_ECONOMY §2.3 T0, AI_GALACTIC_2 "btime"). The location branch
  (planet-specific time) is never taken here because 0x6c0450 passes no location. **Not** read: `Tactical_Build_Time_Seconds`
  (+0xA00), GC `Tactical_Build_Time_Multiplier` (0x140b26f34), build-time modifiers, the corruption factor, the cheat 5.0.
- If PP +0x28 (the plan player) is set: `S x` the player's difficulty adjustment 0x4b0500(player): mode 0 `+0x28`
  (`Galactic_Build_Time_Multiplier`), mode 1 `+0x30` (`Land_Build_Time_Multiplier`), mode 2 `+0x2C` (`Space_Build_Time_Multiplier`).
  0x4b0500 returns the all-1.0 `Internal_Dummy_Adjustment` when the player has no AI record (player +0x30 == 0), else the record for
  the player's difficulty (+0x58) [H structure; M that the dropped argument is the plan player, as A2F_RE_READS §1.4].
  Vanilla land / space: Easy 1.5, Normal 1.0, Hard 0.9 (AI_TACTICAL §4.5).
- Result in seconds, float.

## 4. Where the time tests run, and what a failure does

| Test | Address | Runs in tactical? | Tactical outcome |
|---|---|---|---|
| Accept a new goal | 0x6b8660: after the eligibility check 0x6bae50, accept when `AllowedTime <= 0` or `G +0x7C <= AllowedTime`, then 0x6139f0 (fundable) | yes (no mode test) [H] | ETA 0 <= AllowedTime whenever AllowedTime > 0, and AllowedTime <= 0 passes too: **always passes** [H] |
| Resolve, new proposal against active goals (two sites) | 0x6c8970: after the re-solve 0x6c4a50 succeeds and 0x6129c0 passes, `0 < AllowedTime < G +0x7C` -> release (0x693750 when activated) and reject the proposal (0x58a5d0(..., 0)) | yes (no mode test around it) [H] | `AllowedTime < 0` is false: **never fires** [H] |
| Maintenance time-drop | 0x722a30: `p = 0x525200(exec, G)`, `if p > 0: p += (NOW - G +0x70) / FPS`; drop when `AllowedTime < p` and replaceable | **no**: the whole block sits behind `manager +0x34 == 0` (galactic) [H] | skipped; the rest of maintenance (state, desire <= 0, PP re-validate 0x6c47c0 unless the flag 0x5791e0(G) +0x60 -> +0xA8 is set) runs as in galactic [H] |

0x525200 (goal production time) has that maintenance site as its only caller [H], so no tactical code computes a live production
time either.

Failure handling, for completeness (only reachable in galactic): accept failure rejects the proposal and frees its PP; the Resolve
site releases the competing goal (deactivating it first if it was activated) and rejects the proposal; the maintenance drop
releases the goal through 0x7225d0 / 0x614880. No re-plan is triggered by a time failure; a later goal cycle simply proposes again [H].

So for land and space AIs, **neither Build_Time_Delay_Tolerance nor Time_Limit ever rejects, releases or drops a goal** [H]. The
AllowedTime value is computed (accept, Resolve) but cannot change the outcome. Time_Limit has no other direct reader in tactical:
its other readers (0x6129c0, 0x6139f0, the money horizon in 0x6c3da0) are galactic-only [H for the field reads found; M that no
copy of it is read elsewhere].

## 5. Port comparison

Port code read (A2g t06):
- `engine/ai/tactical/src/tactical_ai.cpp` `accept_result`: `g.eta = 0`, `g.cost = 0`, `g.nominal_time = pp_nominal_time(...)`;
  `plan_activatable` refreshes `nominal_time` after the re-solve; `elapsed_eta` returns 0; `uses_budget()` false;
  `gcfg.nominal_allowed_time = true`.
- `tactical_nominal_time`: sum over the picks of `build_time_t0(Build_Time_Seconds, Production_Speed_Factor)` (truncating, as
  0x371230) x `tactical_build_time_multiplier` (Land / Space of the AI player type's difficulty name, 1.0 when not found).
- `engine/ai/src/ai_goals.cpp`: `accept_tail` (time test only when `uses_budget`), `time_ok` (`!(0 < at < eta)`), `maintenance`
  (time-drop only when `uses_budget`, against `elapsed_eta`).
- `engine/apps/sim/src/battle_ai.cpp`: per type `build_time_seconds = Build_Time_Seconds` (int field +0x890).

All of this matches retail in outcome: ETA 0 and cost 0 [H, was [port]], the 0x6c0450 base and scaling [H], no maintenance time-drop
in tactical [H] (the port gets there through `uses_budget() == false` and `elapsed_eta == 0`; retail through the mode gate).
The only formal difference is that the port's tactical accept skips the time term (`uses_budget` false) where retail evaluates it;
with ETA 0 it always passes, so the result is identical.

## 6. Port change

**Port already matches; no behaviour change.** Optional, comment-only:
1. Retag the `[port]` notes "tactical goals carry ETA 0" (`tactical_ai.cpp` near `accept_result` `g.eta = 0.0f` and the
   `gcfg.nominal_allowed_time` comment) as `[H]`, citing this page: in land / space mode retail never writes a nonzero PP ETA (the
   solve reset clears it and the ETA step is galactic-only), and the goal cost source PP +0x44 is cleared the same way.
2. Note at `elapsed_eta` (tactical) that retail skips the maintenance time-drop entirely outside galactic mode, so returning 0 is
   exact, not a stand-in.
3. Keep the tactical `nominal_time` as it is (galactic `Build_Time_Seconds`, `Production_Speed_Factor` truncation, Land / Space
   multiplier). It does not change any decision; it only feeds the goal hash and save. Do not switch it to
   `Tactical_Build_Time_Seconds`: retail reads the galactic field.

If an engine agent ever wants a nonzero tactical ETA (for example to model build-queue delay), that would be a deliberate deviation
from retail; retail has none.

## 7. Data the battle host must supply

- Per type: `Build_Time_Seconds` (int, +0x890) -> `PerceptionType::build_time_seconds` (already done).
- GameConstants `Production_Speed_Factor` (+0x120) -> `TacticalConstants::production_speed_factor` (already done).
- Per difficulty adjustment: `Land_Build_Time_Multiplier` (+0x30) and `Space_Build_Time_Multiplier` (+0x2C) (already loaded).
- Not needed for the goal timing: `Tactical_Build_Time_Seconds`, `Tactical_Build_Time_Multiplier`, build-time modifiers.

## 8. Open

- [M] that no marshaller callback (the per-mode lists at 0x140b3a570, slots +0x08 .. +0x30) writes PP +0x48 in tactical; the field
  scan found none.
- [L] meaning of the flag PP +0x50 -> +0xA8 (forces ETA 0 in every mode) and whether it is the same object as maintenance's
  0x5791e0(G) +0x60 -> +0xA8 (skips the PP re-validate); the port does not model the latter (ai_goals.cpp "template flag +0x60->+0xa8 not modelled"). It cannot matter for tactical timing,
  where the ETA is 0 anyway.
- 0x6bae50 (the first check in accept 0x6b8660) was not read in full; it is not a time test.

# A2f t06: RE reads of 0x525200, 0x373e30, 0x5f2430, 0x725c50 / 0x726010 (2026-11-05)

Source: Ghidra output of StarWarsG.exe (main checkout `re_reference/`, read by the RE agent only). Behaviour only; no code.
Tags: [H] read from the code, [M] inferred, [port] a port rule. Frames are the global logic frame `NOW` = 0x140b0a320, FPS = int
0x140b0a340 (30). Times are seconds unless stated.

## Summary

| Address | What | Verdict on the port | Change |
|---|---|---|---|
| 0x525200 | goal production time | **The port's `goal_production_time` already matches** (the "stand-in [M]" labels are stale; q2f t01 read it). **Two use-site differences found**, both in the time-drop test: the original adds `elapsed` only when the production time is > 0, and its AllowedTime base is not the PP ETA | `galactic_goal_time.hpp`, `ai_goals.cpp:351`, `galactic_ai.cpp:561` (see 1.4) |
| 0x373e30 | per-tech member list | **Field confirmed**: `Starting_Spawned_Units_Tech_N` (type +0xAD0 + 0x18 x N). The port's list and the "fall back to a lower level" rule are exact for the planet-level force leaves. Player-level tokens must NOT fall back; tech >= 6 gives nothing (port clamps to 5) | `galactic_ai_tokens.cpp:44-56`, `:1194` (see 2.4) |
| 0x5f2430 | enemy strength helper, "not held by AI or ally" | The port's formula matches. **The port's AI snapshot treats a Neutral-held planet as held by an ally** (relation table built as 0 for everything that is not an enemy); in the original the Neutral relation is 2 (not 0), so the planet is "not held" and the add applies | `galactic_ai_tokens.cpp:330-333` (see 3.3) |
| 0x725c50 / 0x726010 | Production Setup C / Score, t = 1e18 | **Vetoed** (non-MagicPlan), and left out of the mean / min / max, never given -0.5. The R1 veto in `ProductionM::score` is exact: retag [port] -> [H] | comment / tag only (see 4.3) |

## 1. 0x525200 `GoalProductionTime(executionSystem, G)`

### 1.1 Inputs
- `param_1` = execution system (the caller passes `*(*(goalSystem+0x80)+0x28)+0x50`). Its +0x40 is the list of the player's `AIBuildTaskClass` objects (list order = creation order).
- `param_2` = the instantiated goal G (+0x74 state, +0x7c ETA, +0x70 creation frame).

### 1.2 Formula [H]
- G == null: 1e18.
- G not activated (`0x692d10`: state +0x74 > 0 is false): returns G +0x7c (the PP ETA).
- Otherwise `A = B = 0` and, over the build tasks X in list order, for X with a bid record (X +0xC8 != 0) and (X has no task force (+0xA0 == 0) or X's task force's goal (+0xA0 -> +0xF0 -> +0x88) == G):
  - bid planet (record +0x28) null: `A = max(A, btime(type(X), none))`, btime = 0x371230 (type +0x890 x global factor, seconds);
  - bid planet P: `Q` = P's queue for X (0x559b40(P +0xD0, 1 unless the type has +0x86C >= 1 and (+0xF0C != 0 or behaviour 0x5F), then 0)); `B = max(B, 0x53bb20(Q, 0, X) + btime(type(X), P))`.
- Returns `A + B` (seconds; 0 when no task matches).
- Callee 0x53bb20 and the bid record lifetime were re-read and agree with AI_GALACTIC_2 §3.4 word for word (bid record built in 0x648980 / 0x6492f0 case 0x17, galactic mode only; wait = current item remaining time + the queue's tasks with slack <= slack(X), slack = goal ETA - (NOW - created)/FPS - btime; paid up front: sum, pay-as-you-go: max(btime, fundsTime)).
- Port: `GalacticAiWorld::Impl::goal_production_time` (galactic_build.cpp:499-560) implements exactly this. The "goal is always activated" shortcut is fine (a goal reaching maintenance has state 1 or 3).

### 1.3 The only caller: maintenance 0x722a30 (galactic, mode +0x34 == 0)
```
p = 0x525200(execSys, G)
if (0 < p)  p = p + (float)(NOW - G+0x70) / (float)FPS      // int subtraction, float division
allowed = AllowedTime(G, 0)                                  // 0x692860
if (allowed < p && replaceable(G)) drop G
```
No `allowed > 0` guard in the original.

### 1.4 Differences between the port and the original [H]
1. **`elapsed` is added only when `p > 0`.** The port's `galactic_maintenance_eta` (galactic_goal_time.hpp) always adds it. Once every build task of the plan is producing (a producing task has released its bid record and adds nothing) or the plan has no produced units, the original ETA is 0 and the goal is never time-dropped. The port's ETA is then `elapsed` and the plan is dropped as soon as `elapsed > AllowedTime` while production runs normally. This matches the stall diag's observation that "eta + elapsed" dropped every on-schedule build.
   - Exact form: `eta = production_time > 0 ? production_time + (float)(now - created) / (float)kAiFps : production_time` (float division, not double then cast).
2. **AllowedTime is not `PP ETA x tolerance`.** 0x692860(G, 0): no PP (+0x68 == 0): `0 + Time_Limit` (type +0x30); else `(0x6c0450(PP) + 0) x Build_Time_Delay_Tolerance` (type +0x34). 0x6c0450 [H structure, M for the argument of the multiplier lookup]:
   - `S = sum over the PP's selection (PP +0x58 .. +0x60, every selected type, free-store entries included) of btime(T, none)`;
   - if PP +0x28 != 0: `S x` the plan player's difficulty adjustment (0x4b0500; `Galactic_Build_Time_Multiplier` +0x28 when PP +0x198 == 0 (galactic), `Land_` +0x30 for 1, `Space_` +0x2C for 2; vanilla Easy 1.5, Normal 0.8, Hard 0.6; an all-1.0 object when the player has none).
   - So AllowedTime is about the sequential nominal build time of the whole selection, not the queue-pool ETA (PP +0x48 / goal +0x7C, 0x6bfcf0 -> 0x6639f0), which is the value it is compared with.
   - The same AllowedTime is used by accept (0x6b8660: accept when `allowed <= 0 || G+0x7c <= allowed`) and by 0x6c8970 (release when `0 < allowed < G+0x7c`). The port's `GoalSystem::allowed_time` (ai_goals.cpp:351) uses `g.eta x tolerance`, which is smaller (max over queues versus a sum), so the port drops and rejects earlier.
   - Change: keep a per-goal `nominal_time` next to `eta` (set where `g.eta = pp.eta`, galactic_ai.cpp:561, from `GalPp::sel_rec` types with `btime_none` x `rules.difficulties[ps.difficulty].galactic_build_time_multiplier`, as economy.hpp `ai_multiplier`); return `nominal_time x tolerance` from `GoalSystem::allowed_time` for galactic goals with a PP; save / hash it (ai_save.cpp:36/58, ai_goals.cpp:613). Tactical (Land / Space multipliers) is a separate change.
   - The `at > 0` guard in `GoalSystem::maintenance` (ai_goals.cpp:405) is a [port] addition; with the sum base it only matters for a selection whose build times are all 0.
3. Both change goldens (galactic AI synthetic / underworld / HotM, N1 save synthetic). Difference 1 alone is the smaller step and removes the "dropped while producing" behaviour; difference 2 changes which plans are accepted.

Unresolved: the exact argument Ghidra dropped at the 0x4b0500 call inside 0x6c0450 (read as the plan player, [M]); the behaviour when PP +0x28 is null (no multiplier) is not reachable in galactic play.

## 2. 0x373e30 `PerTechMemberType(type, tech, index)` and family

### 2.1 Layout [H]
- Six `std::vector`-like lists of 0x18-byte entries at type +0xAD0 + 0x18 x tech (tech 0..5) = `Starting_Spawned_Units_Tech_0..5` (OBJECT_TYPES.md: code 0x46 (type, count) list, plain append, so several XML lines add up). Entry: +0x08 member type (pointer), +0x10 int count; +0x00 and +0x14 not used by these readers [unresolved what they hold].
- `Reserve_Spawned_Units_Tech_N` (+0xB60) is a different field and is not read here.

### 2.2 Accessors [H]
| RVA | Name | Rule |
|---|---|---|
| 0x373f40 | count (with fall back) | tech >= 6: 0. Else start at list `tech` and step down to 0 until a list is non-empty; return its size |
| 0x373e30 | type at index (with fall back) | same walk; index < 0 or >= size: null; else entry +0x08 |
| 0x373d30 | count of a type (with fall back) | same walk; the first entry in that list whose +0x08 equals the type: its int +0x10; none: 0 |
| 0x373fd0 | count (exact) | tech >= 6: 0; else size of list `tech` (no fall back) |
| 0x373ee0 | type at index (exact) | tech >= 6 or index out of range: null; else entry +0x08 |

Tech = the owning player's tech level (player +0x84; the owner of the fleet unit, the structure or the PP's player).

### 2.3 Users
- Fall-back family (0x373f40 / 0x373e30 / 0x373d30): planet-level force leaves 0x6d95d0 (ground, structures D+0x158) and 0x6dbd50 (space: fleet units' types, starbase type D+0x140 with the owner at D+0x74), Plan Setup C 0x729cc0 (rec.contents, repeated by count), hangar bay list 0x594b30, auto-resolve 0x60b0d0 / 0x60a140 / 0x60d2a0. Each member is added as `AI_Combat_Power(member, 0x370e60) x count` when its category mask (+0x1648) matches.
- Exact family (0x373fd0 / 0x373ee0, count by 0x373d30): player-level tokens 0x66b1c0 (SpaceTotalUnnormalized) and 0x6682c0 (GroundTotalUnnormalized), and 0x608080 / 0x493d50.

### 2.4 Port comparison
- Field and list contents: **exact** (`make_galactic_ai_type_info` reads `Starting_Spawned_Units_Tech_N`, galactic_ai.cpp:2355-2375). Retag [port] -> [H] for the field; the fall-back rule for the planet-level leaves [M] -> [H].
- `galactic_ai_member_force` (galactic_ai_tokens.cpp:40-58) always falls back. That is right for the planet-level leaves and wrong for the player-level tokens (`player_total`, galactic_ai_tokens.cpp:1194): use the exact list there (no fall back; a starbase type with only a Tech_0 list adds nothing for an owner at tech 3).
- Tech clamp: the original returns nothing for tech >= 6; the port clamps to slot 5 (`std::min(tech, kSpawnTechSlots - 1)`). Unreachable in vanilla data (tech <= 5), so low value.
- The starbase branch of 0x6dbd50 uses the tech of D+0x74 (the starbase owner), the port uses `ps.owner` [M: equal in vanilla play].
- Duplicate type in one list: 0x373d30 returns the first entry's count for every duplicate; the port multiplies each entry by its own count. Not in vanilla data.
- Not modelled in the port (existing gap, PHASE4 "contained units"): Plan Setup C `rec.contents` from this list (the units a candidate type brings, and the mask / force bookkeeping in Plan Commit 0x729830).

## 3. 0x5f2430 enemy strength helper `(ctx, node, mask, groundFlag)`

### 3.1 Formula [H]
- ctx null: 0.0. `v` = the perception evaluator's value for the path `Target.<EnemyForce>.<Space|Ground>TotalUnnormalized` (double; 0 when the evaluation fails).
- Let R = the planet's political control record (planet object vf +0x98 = 0x3956c0: `{float control, int owner}`) and Self = ctx +0x28. If **0x4ad9f0(R, Self) is false** ("not held by Self or an ally"):
  - mask = param_3 (0 means all bits);
  - space (flag 0): the starbase type D+0x140 (0x3f8b40); if non-null and its category mask (+0x1648) & mask != 0: `v += AI_Combat_Power(type)` (0x370e60, scaled by squad fill only where the mode virtual +0xE0 is true and the type is a squadron);
  - ground (flag 1): `v += sum over D+0x158 ground structures (mask & structure type +0x1648) of AI_Combat_Power(type)` plus 0x4b8000(D, mask) (sum over the planet's garrison list D+0x10, count D+0x18, stride 0x18, of AI_Combat_Power of the looked-up type with a matching mask).
- Returns the double.

### 3.2 0x4ad9f0(R, player) and 0x2823c0 [H]
`held = (R.owner != -1) && (player.relation[R.owner] == 0) && (R.control > 0.0)`. Relation table (player +0x370, TACTICAL_COMBAT §2.10): 0 friendly (self, ally), 1 enemy, **2 neutral**; the player constructor fills 2, campaign setup writes 2 for every pair that involves an `Is_Neutral` faction. An owner of -1 gives false.

### 3.3 Neutral-held planets
- A planet owned by the Neutral player: relation[Self][Neutral] = 2, so `held` is false regardless of `control`, and the starbase / structure / garrison strength **is added**. Same for owner -1.
- Port: `GalacticSnapshot::held_by_ally` (galactic_perception.cpp:58) matches 0x4ad9f0 when given the right table, but `galactic_ai_tokens.cpp:330-333` builds the snapshot relation as `0` for every pair that is not `GalacticSim::is_hostile` (Enemy). A Neutral pair has `Relation::Neutral` (default_relation, campaign.cpp:45), so it becomes 0 = friendly. Effects: (a) the 0x5f2430 add (galactic_perception.cpp:296-303) is skipped for Neutral-held planets; (b) `view.held_by_ally` / `dangerous` (fill_view) treat them as held; (c) the "last seen hostile units" tests (galactic_perception.cpp:223, 345) treat Neutral units as friendly (original: relation != 0 is hostile for that test).
- The galactic sim's own `PlayerView` (campaign.cpp:188, `allied()` = Self or Ally) is already right.
- Change: build `s.relation` from `sim.relation(a, b)`: Self / Ally -> 0, Enemy -> 1, Neutral -> 2 (the port enum is Self 0, Ally 1, Neutral 2, Enemy 3, so map explicitly, do not cast). `GalacticSnapshot::friendly` keeps testing `== 0`. Moves goldens only where a Neutral-held planet has a starbase / structures or Neutral units are in orbit or on the surface.

## 4. 0x725c50 (ProductionM Setup C) and 0x726010 (ProductionM Score)

### 4.1 Setup C (re-read, agrees with AI_GALACTIC_2 §1.4) [H]
Per candidate type T (list PP +0xD0; record array stride 0xB8, `prodScore` = rec +0x18 is used first as `t`, then overwritten by the score):
- generic (0x5B / 0x5C), or (a target exists and T is 0x5F / 0x60): `t = 0x2804d0(player, T, target planet) ? 0 : 1e18`, then (when buildable) `t -= btime(T, none)`;
- else `t = 0x664210(reachability, T, level)` (mean over source planets of btime + queue wait; level 5 with no target, else the goal's Reachability +0x2C); **exactly 1e18 when no planet can build it**; otherwise `t -= btime(T, none)`.
- Stats over the candidates with `t != 1e18` only: n, sum, min m (init 1e18), max M (**init 0.0**, so M >= 0 always).
- If `0 < M` and `n != 0`: spread = M - m, set to 0 when `|spread| < 1e-4`, else avgNorm = (sum/n - m)/spread. Otherwise spread = 0, avgNorm = 0. k = min(spread/120, 2).
- Second pass: a candidate whose rec +0x18 is exactly 1e18 is **left at 1e18**; others get 0 when spread == 0, else `clamp(-((t - m)/spread - avgNorm) x k / 2, -0.5, 0.5)`.

### 4.2 Score 0x726010 [H]
Order: free-store entry (rec +0x04 != -1) -> 0.5; galactic ground / orbital structure vetoes (1e18); then, **if the plan is not a MagicPlan**: pop cap veto (sets rec +0x18 = 1e18), then **`return rec +0x18`** (the Setup C value); MagicPlan: its own veto list then one synced float draw (rec +0x18 is not read).

### 4.3 Answer and port comparison
- A candidate with t = 1e18 in a non-Magic plan: dropped from n / sum / min / max, never given -0.5 (or 0), and **Score returns 1e18, which the solver treats as a veto** (AI_GALACTIC_2 §1.2 step 1; Production's IsPermanentVeto is true, so the candidate is erased). In a MagicPlan it is not vetoed by t.
- The stall diag's guess ("the original's 1e18 probably reaches the score through a non-zero spread") is wrong: it is a veto, with or without spread.
- Port: `ProductionM::setup_c` stats (galactic_pp.cpp:569-585) and the `r.prod_t >= kInf` veto in `score` (galactic_pp.cpp:621-626) are exact. Differences that do not matter: the port stores `prod_t` and `prod_score` separately and gives a kInf candidate -0.5 / 0 in `prod_score` (never read for it after the veto; read only by a MagicPlan, which does not use it); `m` / `M` start from the first value instead of 1e18 / 0 (same results: `0 < M` is tested). Change: tag [port] -> [H] in the comment (galactic_pp.cpp:621-626), PHASE4 line "ProductionM vetoes produced types no planet can build [port]", and correct AI_GALACTIC_2 §1.4 Score ("return rec.prodScore" is 1e18 for an unbuildable type).
- Not covered by this read: whether 0x664210 itself returns 1e18 for a zero-cost hero (Veers) is decided by its `0x2804d0` call (cost / lock / structure checks, 0x2804d0 was not re-read here).

## 5. Proposed port changes, in order
1. `galactic_goal_time.hpp` `galactic_maintenance_eta`: add elapsed only when `production_time > 0`, in float arithmetic (1.4 item 1). Regenerate the galactic AI goldens.
2. AllowedTime base (1.4 item 2): per-goal nominal time x `Galactic_Build_Time_Multiplier`, used by accept, maintenance and `time_ok`. Separate step, own golden regeneration; verify with a probe if wanted (log AllowedTime vs ETA of the first accepted Conquer / Build plans).
3. `galactic_ai_tokens.cpp:330-333`: relation from `sim.relation()` (3.3).
4. `galactic_ai_tokens.cpp:1194`: exact-tech member list for player-level tokens (2.4).
5. Retag comments [M]/[port] -> [H]: galactic_ai_impl.hpp:395 / 590, galactic_goal_time.hpp, galactic_pp.cpp:621-626, PHASE4 "A2 f" stand-in list.

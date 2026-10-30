# Galactic AI, part 2: closing the A2 parity gaps

Clean-room behavioural spec from the Ghidra 12.2 decompilation of the 64-bit Steam `StarWarsG.exe` (FoC v1.121, image base 0x140000000; all addresses are RVAs), vtables and constants read from the same exe, and the shipped FoC scripts. No decompiled code is reproduced.

Tags: **[H]** read directly from code or data; **[M]** one inferred link (a field name, a callee's purpose); **[L]** plausible, not verified.

This document continues AI_GALACTIC.md and closes its open items 3, 4, 6, 7 and 8, plus the A2 open items in PHASE4.md. It does not repeat AI_GALACTIC.md, AI_TACTICAL_PERCEPTION.md §1 (evaluator machinery), GALACTIC_ECONOMY.md §6 or SCRIPT_GAPS.md (`Apply_Markup`).

Notation:
- **PP** = PotentialPlanClass (0x210 B).
- **rec[i]** = the solver's per-candidate scratch record (0xB8 B, global array 0x140b3a558).
- **slot** = a task-force team slot (0x50 B, PP+0xE8 vector).
- **FPS** = 30.
- **cost(T)** = 0x3711c0(T, planet). With a planet it is the planet-specific price (rounded, ≥ 1 when positive); without one it is type +0x86C (build cost).
- **btime(T)** = 0x371230. With a planet it is the planet-specific build time; without one it is type +0x890 × global 0x140b15920.
- **power(T)** = `AI_Combat_Power` (0x370e60).

---

## 1. PotentialPlan instantiation marshallers (AI_GALACTIC open item 3)

### 1.1 Marshaller lists per mode [H]

The lists are built once by 0x6c0e60 into three vectors at 0x140b3a570 (galactic), 0x140b3a588 (land) and 0x140b3a5a0 (space).
- Each element is a static marshaller object; its vtable was read from the binary.
- Contrast is the dynamic object 0x140b3a548, built by 0x72aa50.

| Order | Galactic (mode 0) | Land (mode 1) | Space (mode 2) |
|---|---|---|---|
| 0 | FreeStore | FreeStore | FreeStore |
| 1 | Budget | Plan | TechTree |
| 2 | TechTree | Reinforcement | Plan |
| 3 | Plan | TechTree | Reinforcement |
| 4 | Production | Hero | Hero |
| 5 | Hero | Contrast | Contrast |
| 6 | TerrainType | TacticalBuild | TacticalBuild |
| 7 | Contrast | – | – |
| 8 | Survival | – | – |

Every marshaller call runs over the list in this order. This includes commit, so the FreeStore commit runs before the Budget commit.

**Vtable slots** [H]

| Slot | Purpose | Default |
|---|---|---|
| +0x08 | Score(candidate, PP) → float | – |
| +0x10 | Commit(candidate, PP) | – |
| +0x18 | Setup B | no-op 0x1fa90 |
| +0x20 | Setup C | no-op 0x1fa90 |
| +0x28 | Setup A | base 0x6bdbe0 stores the marshaller's bit (PP+0x1AC = 1 << index) |
| +0x30 | Finalize | no-op 0x1fa90 |
| +0x38 | IsPermanentVeto | false 0x13b7d0; Plan and Production return true 0x17dc70 |

| Marshaller | vt | Score | Commit | Setup B | Setup C | Setup A | Finalize |
|---|---|---|---|---|---|---|---|
| FreeStore | 0x8bf368 | 0x728560 | 0x728fd0 | – | – | 0x7287a0 | – |
| Budget | 0x8bf320 | 0x728330 | 0x7284f0 | – | – | 0x7284d0 | 0x728310 |
| TechTree | 0x8bf3b0 | 0x729190 | – | – | – | base | – |
| Plan | 0x8bf3f8 | 0x72a170 | 0x72a720 | 0x729550 | 0x729cc0 | 0x72a3b0 | 0x72a060 |
| Production | 0x8bf120 | 0x726010 | 0x726390 | – | 0x725c50 | base | – |
| Hero | 0x8bf168 | 0x726f40 | 0x7271f0 | 0x726410 | 0x726710 | – | – |
| TerrainType | 0x8bf200 | 0x727c40 | – | – | – | base | – |
| Contrast | 0x8ce6b0 | 0x72b940 | 0x72bf80 → 0x6c4b20 | – | 0x72aad0 | 0x72bee0 | 0x72af80 |
| Survival | 0x8bf248 | 0x727cd0 | – | – | – | base | – |

### 1.2 Solver 0x6c3da0, exact [H]

**Reset** (0x6c3080):
- Clear the PP selection and scratch.
- Galactic: PP+0x30 = the goal category's available credits (budget cat+0x44 via 0x612e30); PP+0x34 = its available rate (cat+0x48 via 0x612d80); PP+0x38 = PP+0x30.
- If the population cap is enabled (byte 0x140b276bc), the remaining pop capacity goes to 0x140b3a530.

**Setup.** For each marshaller in order, run Setup A; then Setup B for each; then Setup C for each.

**Iteration loop:**
1. For each candidate i in candidate-list order, sum the scores of all marshallers in order:
   - a score of exactly **1e18** is a veto: rec[i].score = −1e18 and the remaining marshallers are skipped;
   - if that marshaller's IsPermanentVeto returns true, the candidate is erased from the list (and its record), and the list shifts.
2. **50 % rule.** For each candidate with score > −1e18 that is not the only viable one:
   - count how many selected entries (PP+0x58) equal this candidate's type;
   - if count / |selection| ≥ 0.5, set score = −1e18.
   - This is skipped while the selection is empty.
3. **Pick** the first index with the strictly greatest score, starting from −1e18. Ties go to the lowest index. If nothing scores above −1e18, run Finalize on every marshaller and stop.
4. **Commit:**
   - append the type to PP+0x58, the candidate index to 0x140b3a5e8, rec.contrastEntry (+0x14) to PP+0x78 and rec.teamSlot (+0x10) to PP+0xC0;
   - merge the record's attachment ranges (+0x60, +0x78) into PP+0x88 / PP+0xA0;
   - galactic: push −1 to PP+0x120;
   - call Commit on every marshaller in order.
5. **Recompute the ETA** (0x6bfcf0, §2.1). If ETA (PP+0x48) > 0:
   - horizon = ETA, but use Time_Limit instead when 0 ≤ Time_Limit ≤ ETA;
   - PP+0x38 = horizon × PP+0x34 + PP+0x30 (money available by the horizon).

**Success** is PP+0x4C; it starts at 1 and any Finalize may clear it. On success Init 0x6c0d00 copies ETA → goal +0x7C and **net cost PP+0x44** → goal +0x78, and sums the selection's population into PP+0x1A8.

**Candidates are object types, not objects.** A type can be selected many times; each selection is one unit. The candidate list (PP+0xD0) is filled by 0x6c1fb0, which deduplicates through a set:
- a generic type with behaviour flag 0x5C or 0x5B is first replaced by the planet-specific variant (0x3f8fd0 / 0x3f8c40 on the target planet's surface);
- candidate order = order of first insertion.

**Fields used below:**
- PlanDefinition: +0xA8 MagicPlan, +0xA9 MagicPlanStealing, +0xAA IgnoreTarget, +0xAC AdditionalCost, +0xB0 MinimumCost, +0xB4 "some TF is percent-based", +0xB8 RequiredCategories (u64 masks), +0xD0 AllowFreeStoreUnits, +0xD1 AllowEngagedUnits, +0xD4 PerFailureContrastAdjust, +0xA0/+0xA4 Min/MaxContrastScale.
  - Constructor defaults (0x66ec20): MagicPlanStealing = 1, AllowFreeStoreUnits = 1, AllowEngagedUnits = 1, everything else 0 [H].
  - Loader offsets come from 0x4e7170 [H].
- TaskForceDefinition (parser 0x66f5d0 / 0x66fcc0): +0x48 percent-based, +0x49 hero attach allowed (default 1), +0x4A EscortForce, +0x4B TaskForceRequired, +0x90 special-weapon attach, +0x94 unit-filter mask (−1 = none), +0x98 / +0x99 RequireUnique / RequireMatchingStage, +0x9C MinimumTotalSize, +0xA0 MinimumTotalForce, +0xA8 list of excluded types.
- TeamDef (0x40 B): +0x10 **max**, +0x14 **min**, +0x18 percent, +0x20 candidate types, +0x38 category mask.

### 1.3 Team specs [H]

`lhs = rhs` (0x66fcc0):

| rhs | min (+0x14) | max (+0x10) | percent (+0x18) |
|---|---|---|---|
| `a,b` | a | b if b ≠ 0, else a | 0 |
| `N` | N | N | 0 |
| `N%` | 0 | 0 | N; also sets TFDef+0x48 |

- After all TFs are parsed, every percent is renormalised (0x670630): `pct = (int)(pct / Σpct × 100 + 0.5)`, where Σpct is summed over all teams of all TFs of the plan.
- **lhs is a category mask** (parse through 0x140b31020) → the team's types are every GameObjectType, in type-manager order, that passes all of:
  - flags +0x67, +0x29 and +0x21 are 0;
  - no behaviour 0x5C or 0x5B;
  - mask & type+0x1648 ≠ 0;
  - if the TF has a unit filter: (filter & typeFilterBits) == filter.
- **Otherwise lhs is a `|` list of type names.**
  - A type with behaviour 0x5C (resp. 0x5B) expands to every faction × level variant (0x407b60 over 0x140a172e0, levels 0..0x140b158f4 / 0x140b158f8).
  - Other names are kept if flags +0x67/+0x29/+0x21 are clear.
- A team with no types is dropped. `-Name` adds to the TF's excluded types, which are then removed from every team's list.

### 1.4 Per-marshaller rules (galactic order)

**FreeStore** (PlanDef AllowFreeStoreUnits)

*Setup A* 0x7287a0 [H]:
- For each TF: compute the stage location 0x140b3a660[tf] through the target's reachability object, using the goal Reachability and TF +0x94; it uses RequireUnique/MatchingStage (+0x98/+0x99). If there is no target, use the goal's own location.
- Only if AllowFreeStoreUnits && !MagicPlan: walk the free store's unit list in list order. A unit is eligible when all of these hold:
  - it is not reserved by another PP (entry +0x30 == 0);
  - its health fraction 0x396470 ≥ the constant at 0x140b27338;
  - **engaged test:** the unit is reserved by this PP, or AllowEngagedUnits, or it is not in a battle-pending state (+0x348 / +0x1C4 checks); and either it is not in a fleet with pending orders (+0x361 / +0x369), or (+0x158 set, the 0x4914e0 value ≥ 1.0, and the 0x527620 check passes) [M field names];
  - it is not building or moving (+0x210, +0x3A7, +0x3B4, 0x39ad40);
  - its type has no behaviour 0x20 for this mode;
  - it is not attached to an object that refuses detach (0x42e3b0 / 0x42e3d0).
- **Distance key** of each eligible unit (galactic):
  - over the TF stages, Σ (stage-reachability-cost(unit planet)²) + Σ 2-D distance² (unit pos − stage pos);
  - the reachability mode is 4 when the goal Reachability is 4, else 3;
  - divide by the number of TFs when any stage cost was finite;
  - it is kept in 0x140b3a678, and the max over units goes to 0x140b3a520.

*Score* 0x728560 [H]:
- Applies only if candidate type+0x13 (hero flag) or (AllowFreeStoreUnits && !MagicPlan).
- If rec.freeUnit (+4) is already set → **1.0**.
- If the type has behaviour 0x2A → 0.0.
- Otherwise, over the eligible units of this exact type, still unconsumed:
  `s = (maxKey ≠ 0 ? 1 − key/maxKey : 0) + 2 (galactic) + 2 (if unit +0x3A2 bit 2)`.
- Pick the max; ties go to the earlier unit. Store it in rec+4 and return s. No unit → 0.0.

*Commit* 0x728fd0 [H]:
- No free unit (rec+4 = −1) → push null to PP+0x108 ("to be produced").
- Otherwise:
  - push the unit; listen to its signals 0x1A and 0x10; mark the entry consumed;
  - record the selection index in 0x140b3a5d0;
  - galactic: **PP+0x44 −= cost(T, none)**; rec+4 = −1.

**Budget** (galactic only)

*Setup A* 0x7284d0: PP+0x40 (gross cost) = PP+0x44 (net cost) = AdditionalCost [H].

*Score* 0x728330 [H]. Only for a unit to be produced (rec+4 = −1) and !MagicPlan:
- If ETA (PP+0x48) is still 0:
  - with no target, or type behaviour 0x60/0x5F/0x5C/0x5B: t = 0.5 × btime(T, none), c = cost(T, none);
  - otherwise c = cost(T, target planet) and t = btime(T, target planet);
  - clamp t to Time_Limit when 0 ≤ TL ≤ t;
  - available = PP+0x38 + t × PP+0x34.
- Otherwise available = PP+0x38 and c = cost(T, none).
- **Veto (1e18, non-permanent)** if available < c + PP+0x44. Otherwise 0.

*Commit* 0x7284f0: unless MagicPlan, PP+0x40 += cost(T, none) and PP+0x44 += cost(T, none) [H].
- Net effect for a free-store unit: FreeStore subtracts its cost and Budget adds it back, so net cost counts only produced units while gross cost counts every unit.

*Finalize* 0x728310: fail if PP+0x40 < MinimumCost + AdditionalCost [H].

**TechTree** 0x729190 [H]
- Galactic, produced unit (rec+4 = −1), and type tech (+0x89C) > player tech (+0x84) → veto (non-permanent).
- Otherwise score = tech/5 × 2 (0.4 per tech level; free units get it too).

**Plan** (the task-force fitter)

*Setup A* 0x72a3b0 [H]:
- Build one slot per (TF in order × team in order): {TFDef, TeamDef, assigned indices, costSum (+0x28) = 0, category mask (+0x30) = 0, heroAttach (+0x38), specialWeapon (+0x39), stage (+0x40), TF unit count (+0x48), TF force (+0x4C)}.
- Clear every record's team list (+0x20).
- Globals: 0x140b3a526 = any TF allows hero attach; 0x140b3a527 = any TF allows special-weapon attach; 0x140b3a525 = any TF is EscortForce.

*Setup B* 0x729550 [H]. For each slot s, for each type T of its team:
- If T is affiliated with the player's faction (0x3751a0), or T has behaviour 0x5C/0x5B, or the free store holds a unit of T (0x4d3570): add s to rec(T).teams, creating the candidate (§1.2).
- Galactic only: also add s to each upgrade variant of T for this player (0x3712a0(T, player+0x84) list) that has no behaviour 0x5C/0x5B/0x60/0x5F and flag +0x21 clear.

*Setup C* 0x729cc0 [H]:
- For each candidate: `rec.startTeam (+0xC) = SyncedInt(0, nTeams−1)` (0x1ffb40 on the synced LCG 0x140a13e24), or −1 when there are no teams.
- **0x1ffb40 draws nothing when lo == hi**, so candidates with exactly one team draw nothing. With ≥ 2 teams it uses rejection sampling: one or more draws, mask = next power of two − 1, redraw while the value > range.
- Galactic: rec.contents (+0x60) = the type's contained units (0x373e30, repeated by count 0x373d30; tech = player+0x84) plus the type's companion list (+0x2028). This is what a fleet or company type brings.

*Score* 0x72a170 [H, return value from the disassembly]:
1. rec.teamSlot (+0x10) = the first team, round-robin from startTeam, that is percent-based or has |assigned| < max; otherwise rec+0x44.
2. Then, over the candidate's teams in list order:
   - non-percent team with |assigned| < max → return **1 − |assigned| / max**;
   - percent team:
     - !IgnoreTarget → return **1.0**;
     - IgnoreTarget and pct/100 × PP+0x3C > cost(T, none) + slot.costSum → return **1.0**.
3. No team qualifies → hero-attach allowed (0x140b3a526) && rec.isHero (+0x54) → 0.0; otherwise **veto, permanent** (the candidate is erased).

*Commit* 0x72a720 → 0x729830 [H]:
- OR the type's category mask into 0x140b3a538.
- Place the unit: starting from startTeam, round-robin over the candidate's teams, choose the first slot whose team lists this type and that is percent-based or below max.
  - First pass: skip EscortForce slots whose escort contrast is already met (0x140b3a529), and non-escort slots whose main contrast is met (0x140b3a528).
  - Second pass: ignore the met flags.
- Placing adds:
  - the selection index to the slot;
  - cost(T, none) to slot.costSum;
  - the category mask to the slot;
  - for **every slot of that TF**: unit count +1 and force += power(T).
- Not placed, but a hero that may attach → slot rec+0x44.
- Each contained unit (+0x60) is also run through 0x729830 for mask/force bookkeeping.

*Finalize* 0x72a060 [H]. Fail when any of these holds:
- the selection is empty and no TF is TaskForceRequired;
- any non-percent slot has |assigned| < **min**;
- any slot's TF force < MinimumTotalForce;
- any slot's TF count < MinimumTotalSize;
- any RequiredCategories mask has no overlap with the selected mask 0x140b3a538.

**Production**

*Setup C* 0x725c50 [H], per candidate:
- If the type is not a structure or generic (not 0x5B/0x5C, and no target, or not 0x5F/0x60):
  t = travelEstimate 0x664210(target reachability, T, goal Reachability or 5) − btime(T, none). A travel value of 1e18 makes t = 1e18 (unreachable).
- Structures: t = −btime(T, none) if 0x2804d0(player, T, planet) can build there; else 1e18.
- Over the non-1e18 values: count n, sum, min m, max M. If M > 0 and n > 0: spread = M − m (0 when |spread| < 1e-4), avgNorm = (sum/n − m)/spread. Set k = min(spread/120, 2).
- rec.prodScore (+0x18) = clamp(−((t − m)/spread − avgNorm) × k / 2, −0.5, 0.5), or 0 when spread = 0.
- **Faster is better** (bonus up to +0.5).

*Score* 0x726010 [H]:
- Free unit → **0.5**.
- Galactic ground structure (0x5F): veto unless the target has a planet object with a surface, the per-planet cap (type +0x87C, when ≥ 0) is not reached (0x3f94f0), and used ground slots 0x3f8af0 + this PP's queued ground structures (0x140b3a534) < the slot max 0x372740. Orbital structure (0x60): the same with 0x3f8b10 / 0x372760 / 0x140b3a540.
- !MagicPlan:
  - if pop cap is enabled and the type's pop (0x373500) > remaining pop (0x140b3a530) → rec.prodScore = 1e18 (veto);
  - return rec.prodScore. Production's IsPermanentVeto is true, so a pop or slot veto erases the candidate.
- MagicPlan (galactic): veto when the type is not spawnable (+0x2A or +0x29; non-hero with cost < 1; behaviour 5 (TRANSPORT); tech > 5; 0x375270; a hero already present 0x3729f0; player 0x282550 / 0x282510 locks; unique +0xC1 without 0x282580; generic 0x5C/0x5B; +0xF60 / +0xF80 counts; or not a company/fleet unit and not a 0x5F/0x60 structure). Otherwise return **one synced float draw in [0,1]** (0x5324d0).

*Commit* 0x726390: for a produced unit, remaining pop −= pop(T); the structure counters 0x140b3a534 (0x5F) and 0x140b3a540 (0x60) += 1 [H].

**Hero**
- *Setup B* 0x726410: when hero attach or special-weapon attach is allowed, add as candidates the free-store heroes (type +0x13 / +0x1E / behaviour 0x37, filtered by the two allowances), the hero types in 0x140b3a690, and the commander teams `Field_Com_{Rebel,Empire}_Team` (unless space) and `Fleet_Com_{Rebel,Empire}_Team` (unless land) when the faction is affiliated [H].
- *Setup C* 0x726710: for each hero candidate, load the hero's AI script (object +0x2D8 or type +0x1AF8) and read `MinPlanAttachCost` → rec+0x4C and `MaxPlanAttachCost` → rec+0x50.
  - `Evaluate_Attack_Ability(target, goalTypeName)` → rec+0x38;
  - `Get_Escort_Ability_Weights` → rec+0x58;
  - rec.isHero = 1, rec+0x3C = 1e18, rec+0x40 = 0 [H].
- *Commit* 0x7271f0: each hero record with weights folds the newly selected type into its escort accumulators (0x68dbd0) [M].
- *Score* 0x726f40 [H] for a hero (rec+0x54), when it has no team or !MagicPlan:
  - if the allowance flags permit and the hero is not already attached to a plan (+0x2B0 / +0xF8):
    - (auto-resolve busy, or (Min == 0 or Min ≤ gross cost) and (Max == 0 or gross cost ≤ Max)) and rec+0x40 > 0 → s = rec+0x3C / rec+0x40 + rec+0x38; return s if ≥ −100;
    - otherwise → 1e18.
  - In practice a hero attaches only after escorts were selected and the plan's gross cost is in its [Min, Max] window.
  - MagicPlan with a team: veto if another magic plan of the player already uses that hero, or it is already selected.
  - Non-heroes → 0.

**TerrainType** 0x727c40 [H]
- With a target planet: veto if 0x373490(planet terrain +0x298, T) == 1 (type is forbidden on that terrain).
- Otherwise 0, or 0x5f22c0(T) when PP+0x19C (the target planet's terrain id, set in Init through 0x140b310d8) ≠ −1 (terrain preference) [M].

**Survival** 0x727cd0 [H]
- Score = the galactic Learning system's per-type success ratio 0x588a90(type).
- For a type with variants (+0x960), use the first variant (+0x958) unless its +0x67 is set.
- The galactic context is used even in tactical modes.

**Contrast**
- *Setup A* 0x72bee0 [H]: compute the requirement 0x6c5de0.
  - No target or IgnoreTarget → empty lists and tolerance 0.
  - Otherwise tolerance T = 1 − (f·PFCA + MinContrastScale)/(f·PFCA + MaxContrastScale), with f = recent plan failures at this target (Learning 0x588980). Shipped 1.1 / 1.5 gives T = 0.2667 at f = 0.
  - Galactic list (0x5f1990):
    - entry 0 = (null, total enemy **space** strength at the target 0x5f2430(…, 1), flag 1);
    - entry 1 = (null, total enemy **ground** strength, flag 0);
    - then one entry per EnemyContrastTypes type (plan map +0x68, map order) for space, then for ground. Each = that type's enemy strength × (MaxContrastScale + f·PFCA) × difficulty `Galactic_AI_Contrast_Multiplier`.
    - The two totals are **not** scaled.
    - When a total is ≤ 0, its per-type entries are 0.
  - "Remaining" = a copy of the list.
  - If PlanDef+0xB4 (percent TF): PP+0x3C = max(MinimumCost + AdditionalCost, available + max(TL, 0) × rate). This is the money a percent TF may spend.
- *Setup C* 0x72aad0 (not IgnoreTarget):
  - for each candidate, rec+0xB0 = has a non-escort team; rec+0xB1 = has an escort team;
  - build rec+0x98 = (entry index, weight w) for every entry with a non-null type and positive remaining, where w = 0x5f1f90(T, plan) (this type's weight against that enemy type from the Friendly/EnemyContrastTypes maps; a category match counts unless the weight is 1.0) × terrain factor 0x5f22c0 when PP+0x19C ≠ −1;
  - flags PP+0x1A0 / +0x1A1 = which layers (ground / space) have any positive weight;
  - then 0x140b3a528 / 0x140b3a529 = main / escort contrast already satisfied (0x6bff00).
- **Satisfied test** 0x72ac70 [H], per layer flag:
  - total = Σ target strength over typed entries of that layer;
  - residual = Σ max(remaining, 0)/total;
  - satisfied ⇔ residual ≤ T.
- *Score* 0x72b940 [H, return value from the disassembly]:
  - **IgnoreTarget → one synced float draw in [0, power(T)/500]** (0x532480).
  - Hero record → 0.
  - Otherwise v = the best contribution over the candidate's (entry, w) pairs, starting from −1. P = power(T) × health of the chosen free unit (0x396df0) when there is one.
    - With R = remaining: the pair is considered when (tactical, or a free unit exists) or (target·T < R and the pop-cap allowance permits).
    - d = max(R − w·P, 0), m = max(R, w·P), s = (1 − (d/m)²) × w. Take the max s; the entry index goes to rec+0x14.
    - The escort list is scored too when there is an escort TF and (the candidate has an escort team or main contrast is met).
    - Galactic adds the contributions of contained units (+0x60), each clamped ≥ 0.
  - v > 0 → return v (×2 in tactical modes).
  - v ≤ 0:
    - if the relevant contrast layer is **not** yet satisfied → **veto** (non-permanent);
    - if satisfied → return 0 (still allowed) when any of these holds: MinimumCost + AdditionalCost > gross cost; an untyped total entry still has remaining > 0 that the candidate's layer flags (+0x90 / +0x91) cover; a RequiredCategories mask is unmet and this type provides it; any of the candidate's slots is below min, MinimumTotalForce or MinimumTotalSize;
    - otherwise **veto**.
  - **This veto is what normally ends the solve.**
- *Commit* 0x6c4b20 [H]:
  - recompute both "remaining" lists from scratch;
  - subtract (0x6bf840) every selected unit's weighted contribution, slot by slot (a slot's TF chooses the main or escort list), plus the type's attached objects (+0x1B50, +0x1B60, the first +0x938 entry), then unplaced selections, then PP+0x88 extras;
  - update the satisfied flags;
  - store target − remaining at PP+0x1B0 / +0x1C8 (used by `Test_Target_Contrast`).
- *Finalize* 0x72af80 [H] (not IgnoreTarget): fail if, for an active layer, the untyped total entry's remaining/target > T (checked for entries 0 and 1 of each list), or the layer's 0x72ac70 test fails.

### 1.5 Consequences for the port

1. **Unit selection** = greedy by summed score; a candidate is a type; ties go to the earliest candidate.
   - Typical galactic attack scores: free unit ≈ 2–5 (FreeStore) + 0.5 (Production) + tech×0.4 + team fill (1 − n/max) + contrast fit + survival.
   - Produced unit: faster-to-build/arrive ±0.5 + tech + team + contrast + survival.
   - The free store is therefore strongly preferred, nearest first.
2. **The solve ends** when Contrast vetoes everything (contrast met and all mins met) or every team is full.
3. **Affordability** is checked per produced unit against money available by the ETA (budget §3); free units are free.
4. **Correction to AI_GALACTIC.md §1.13:** the PP solve **does consume synced RNG**. It runs at proposal time (Find_Plan_For_Goal), so these draws happen during proposal, in cursor order:
   - Plan Setup C: one or more int draws per candidate with ≥ 2 teams;
   - Contrast score of IgnoreTarget plans (Build_*_Forces, Advance_Tech, Flush…): one float per candidate **per solver iteration**;
   - Production score of MagicPlans: one float per candidate per iteration.

---

## 2. Production in galactic mode (AI_GALACTIC open item 4)

### 2.1 Planning-time source planets: ETA 0x6639f0 [H]

Called from 0x6bfcf0 after each solver commit. Inputs: the selection with free-store entries nulled, the goal Reachability level (or 5 with no target), output PP+0x180.
- A level > 5 → ETA 1e18.
- The candidate source planets are the target reachability object's planet list for that level (0x665160).
- Two pools, each entry = {used flag, queue time}:
  - pool A (queue 0, ground/structures): types with cost > 0 and (+0xF0C ≠ 0 or behaviour 0x5F);
  - pool B (queue 1): the rest.
  - The initial queue time comes from the planet's AIPlanetBuildQueue 0x53bb20(…, 1, 0) (§2.3).
- For each selected unit, in selection order:
  - **Structure or generic type** (0x5C/0x5B/0x5F/0x60): source = target planet; structureSum += btime(T, target). If the first planet of the list is the target, its queue time also grows.
  - **Other types:** over the pool's planets in list order, a planet is eligible if 0x3fa420 (surface usable) and 0x2804d0(player, T, planet) (can build T there). Then:
    - c = queueTime + |pos(target) − pos(planet)| / (speed × factionSpeed (faction +0x3B0) × FPS), where speed = type +0x648 (first variant's for behaviour 0x5D), and the travel term is added only if speed > 0;
    - c ×= 1 + (planetBaseLevel − requiredLevel) × K, using starbase level 0x3f8b30 − type +0xF10 when +0xF0C == 0, else ground base level 0x3f8aa0 − +0xF0C, with K = 0x140b16c8c;
    - choose the strict minimum (first wins ties); mark it used; its queue time += btime(T, planet); record the planet in PP+0x180.
- ETA = max(max queue time over used entries of both pools, structureSum). **Travel time is used only to choose the planet; it is not in the ETA.**

### 2.2 Queue reservation at commit 0x6c0be0 [H]

Once per PP (PP+0x4E guard; only if PP+0x100): for each selection with a reservation index (PP+0x168[i] ≠ −1), mark that slot of the source planet's queue (planet +0xD0 → 0x559b40(queue index)) as committed (0x53bee0). This is AI_GALACTIC §1.6 step 4. Release is 0x6c2320.

### 2.3 Run time: AIBuildTask → AIPlanetBuildTask market

**Lua `Produce_Force`** creates one AIBuildTask (0x524ee0, FSM 0x64aec0) per missing unit.

**BUILD_TASK_LIST** (enter 0x648980) [H]:
- **MagicPlan:** charge the goal budget (Allocate, 0x691950 → 0x6110e0: req −= cost, alloc += cost) once (+0xD0 guard) → **HIJACK_UNIT** (0x648370). This spawns the unit directly at the planet (0x400b40) or takes an existing one; with MagicPlanStealing or for heroes, it first converts a matching neutral-owned object (0x391310). Then it adds the unit to the free store → FINISHED; spawn failure → FAILED.
- **Galactic:** create an AIPlanetBuildTask (0x6954b0) {type, destination = task+0x98, owner task} with queue index = 0 when cost > 0 and (+0xF0C ≠ 0 or behaviour 0x5F), else 1. Push it on the global pending list 0x140b39330. Then:
  - structure/generic → charge, bind the build planet = goal target (0x6956f0) → PRODUCTION_QUEUED;
  - type +0x894 ≠ 0 → charge → PRODUCTION_QUEUED;
  - otherwise, if AllowFreeStoreUnits and the free store can supply the unit (0x4d2bd0) → PRODUCTION_QUEUED without charging; else charge → PRODUCTION_QUEUED.
- **WAIT_FOR_CREDITS** (0x11) is a pass-through: on enter it goes straight to BUILD_TASK_LIST.

**The market** (AIPlanetBuildQueue update 0x53cd60, per planet and per queue index, from the planet update 0x402840) [H]:
- Skipped for planets with no owner or a neutral owner.
- Runs when more than FPS frames have passed since the last run (strict >).

Each run:
1. queueTime = 0x401ba0(planet production, queue) (the current item's remaining time).
2. **Re-check own tasks** (list +0x10):
   - a task whose bid is < 0 (cancelled), or that is still reachable (0x6955a0), not finished (0x695780) and whose fundsTime ≥ queueTime, is kept. queueTime += btime, or for PAYG players min(fundsTime, btime). If it is no longer buildable here (0x2804d0) → fail.
   - Otherwise its bid is reset (0x695a30: planet = null, bid = 1e18) and it returns to the global pending list.
   - fundsTime = 0x6490d0 = the goal budget's time until it can afford cost(T) (0x613720, §3.4).
3. **Claim one** pending task that already chose this planet and matches the queue index: move it to this planet's list, add its time, remove it from the pending list. **At most one claim per run.**
4. **Bid** on every pending task with +0x7C set, the same queue index, and an owner matching this planet's owner (0x695c90). There is no bid if any of these holds:
   - the path planet → destination with mode 2, 3 (+0xDA) or 5 (type +0x2B) fails;
   - !buildable here;
   - a company with no free ground slot (0x3f9530 ≥ `Max_Ground_Forces_On_Planet`);
   - fundsTime < queueTime.
   - Otherwise **bid = (queueTime + btime(T, planet) + travel(planet→destination)) × (1 + (baseLevel − required) × K) × (cost(T, planet) / cost(T, none))**, with travel as in §2.1.
   - The lower bid replaces the task's choice. An equal or higher bid from another planet does not; the same planet always refreshes its bid. **Ties go to the planet that bid first in planet-update order.**
5. **Start production:** if the queue is idle (+0x20 == −1) and it holds tasks, pick the task with the least slack (`fundsTime_t − (NOW − created)/FPS − btime`, strict <, first wins).
   - Give it a queue id from the global counter 0x140a436b4.
   - Clear the matching reservation slot (type and goal).
   - Bind the task (0x64abf0) and execute the Production event directly (0x5244e0 with the type id, planet id and queue id).

**Claiming the produced unit** [H]
- The queue listens to its planet's signals 3 / 4 / 0xE (0x53c4f0).
- On 3 or 4 (production done) with the matching queue id: if the task is gone or MagicPlan, and an object was produced, the new object (or its fleet head) goes to the player's galactic free store (vf+0x98).
- The task's FSM PRODUCTION_END (6) re-binds the object signals, pays (0x5f9f90) unless prepaid, and continues through FREESTORE_ALLOC / PATHING_DEST / WAIT_FOR_ALL / TRANSPORTS_TO_ORBIT / READY_TO_MERGE / JOIN_TASK_FORCE / PRODUCTION_FINISHED.
  - It moves the unit to the destination with a MovementBlockStatus (id 0x808, mode 0, or 3 if +0xDA, or 5 for a fleet object).
  - A landed ground unit at the destination with a usable orbit is launched first (0x647b90 ops, below).
- On 0xE (owner changed/lost): every task bound to this queue either fails (bid < 0 → Production_Failed + finish) or returns to the pending list.

**Failure** [H]
- **PRODUCTION_FAILED** (7): dispatch `Production_Failed(tf, type)` (0x5fdd90), remove the task from the free-store bookkeeping (0x4d4ee0), finish.
- It is entered when:
  - PRODUCTION_QUEUED polls (every 3 frames, galactic only) find the type no longer buildable for the player (0x2ac320), or the target's ground/orbital structure slots are full;
  - GROUND_UNIT_PRODUCTION finds the planet blockaded (0x3fa370);
  - pathing to the destination fails;
  - the queue loses its planet (0xE).
- **There is no time-out** in the build FSM. The only timer is SYNC_MOVE_COUNTDOWN (+0xE0 / +0xDC). A task waits indefinitely on the market until a planet bids, the funds arrive, or the plan ends (Plan end → 0x5f6a50 destroys its blocking statuses).
- The shipped conquest plans `ScriptExit()` on `Production_Failed`.

---

## 3. Budget (AI_GALACTIC open item 7)

The AIBudgetClass lives at goal system +0x190. Galactic only (context mode == 0).

**Per category:** +0x30 weight equation, +0x38 applied weight (one-tick lag), +0x3C normalised weight, +0x44 available A, +0x48 available rate R, +0x4C next pool P, +0x50 income rate I, +0x54 average horizon, +0x68 goal structs.

**Per goal (BudgetedGoalStruct, 0x50 B):** +0x28 req, +0x2C alloc, +0x30 horizon h, +0x34 required rate q, +0x38 accrued a, +0x3C covered c, +0x40 committed spend s, +0x44 needs-re-add, +0x48 goal.

**Budget-wide:** +0x68 income per second, +0x70 last tick period T, +0x74 reset flag, +0x80 refund accumulator.

### 3.1 Per-tick accrual 0x614340(budget, income) [H]

Called from 0x4b2ef0 on each galactic income tick, after `Add_Credits` (multiplier already applied), for each AI player (not human, galactic context initialised). income is the player's gain for that tick.
- T = 0x4b2010(…) (income period in seconds) [M]. Skipped if T ≤ 0.
- rate = income / T, stored at +0x68.
- Σ = budget +0x80.
- **For each category in list order:**
  - P = A;
  - Iprev = I; I = applied weight × rate.
  - For each goal in vector order:
    - share = Iprev > 0 ? q/Iprev × I : 0;
    - r = max(req, 0);
    - if h > 0: x = r/h − share; otherwise x = share − r/T;
    - x' = x > 0 ? max(x, 0.0001/T) : 0;
    - a += x' × T;
    - alloc = max(alloc, s);
    - P += (share − x') × T + c + a (a after the update);
    - h = max(h − T, 0).
  - Then: applied weight = normalised weight; Σ += P.
- **Second pass** over the categories: if the reset flag (+0x74) is set, P = Σ × w first. Then P += (playerCredits − Σ) × w (drift redistribution; credits = player +0x70).
- Store T at +0x70; clear the reset flag.
- This arithmetic is exact. Port it verbatim, including the asymmetric branch on h and the 0.0001/T floor. The intent reading (x' is the extra rate a goal pulls beyond its share) is [M].

### 3.2 Reweight at each commit 0x614590 [H]

- Evaluate each category's equation with the initial value = the previous normalised weight. Negative values become 0.
- Galactic only, per category: A = P; R = I; for each goal: A −= a, c = 0, needs-re-add = 1. Goals are re-added by Resolve (0x610d30).
- Normalise w = w/Σw, or 1/count when Σw = 0.
- Init 0x6137b0: weights as above; applied weight = w; A = P = credits × w; R = I = 0; +0x68 / +0x70 / +0x74 / +0x80 = 0.

### 3.3 Add goal 0x610d30 [H]

- First time: req = goal cost (goal +0x78 = PP net cost), h = min(ETA, Time_Limit unless −1). The category's +0x54 keeps a running mean of h.
- need = max(req, 0) + alloc − a.
  - If need < 0: A −= need (give back), a += need, need = 0.
  - Then A −= need, c = need.
- If A < 0: back = min(need, −A); A += back; c −= back; shortfall = back.
- q = shortfall / (h > 0 ? h : T); R −= q.
- If R < 0 and A ≤ 0: q = max(R + q, 0); A += R × (h or T); c −= R × (h or T); R = 0.

### 3.4 Other entry points [H]

- **Can fund 0x6129c0:**
  - without a struct: max(max(min(ETA, TL), T) × R + A, 0) ≥ cost;
  - with a struct: max(max(h, T) × R + A, 0) ≥ max(req, 0) + alloc − a.
- **Time to afford n 0x613720:** have = min(alloc, c + a); d = n + s − have.
  - If (int)d > 0: rate = max(alloc − c − a, d), divided by h when h > 0; time = d / rate.
  - Otherwise 0.
  - Used by the build market (§2.3).
- **Release 0x614880:** A += c + a; delete the struct.
  - If c + a > 0 and the player is PAYG (AI): r = 0x5f7d40(goal's plan, c + a) (committed spend reclaimed from unfinished production) [M]; A −= r, P −= r, budget +0x80 += r.
  - So +0x80 accumulates reclaimed spend, enters Σ every tick, and the drift term offsets it. Port it as is.
- **Spend:** `Allocate_Resources(n)` and the build-task charge 0x6110e0: req −= n, alloc += n.
- **Flush_All_Resources 0x6bcdf0:** 0x6931e0(goal, goal cost) then 0x6927d0 [M: releases alloc down to cost, then re-adds].
- **Flush_Category(name) 0x6bce70:** look up the category through enum 0x140b31088, then 0x612cb0(budget, cat) [H call]. Its effect "available = 0" (AI_GALACTIC §1.8) is [M]; the body was not re-read.
- **AdditionalCost** is charged once at plan instantiation (AI_GALACTIC §1.9.4 step 5). It also seeds PP gross/net cost (§1.4 Budget), so `MinimumCost` compares gross cost against MinimumCost + AdditionalCost.

**Port rule.** Replace the "pools = credits × weight" invariant with §3.1–3.3. They reduce to that invariant only on the reset tick.

---

## 4. Galactic perception tokens

Source: leaf-by-leaf read of the galactic registrations (Planet 0x65f7b0, GalacticPlayer 0x66b9f0, GalacticGame 0x66e380, PlanetForce 0x6dcb70, Player base 0x6e09c0, GameForce 0x6e3d40, Game base 0x6e2170, fogged leaves 0x65aa20). The full tables (leaf RVA, cache lifetime, fog mask, failure cases for about 120 paths) were produced in the session scratch `ai2re\tok\tokens.md`. This section keeps what the port needs.

### 4.1 Mechanics [H]

- **Leaf caches.** Lifetime in seconds per leaf (AI_TACTICAL_PERCEPTION §1.2); this is behaviour.
  - Planet force totals 15 s, Nearby* 30 s, Has*Force 7 s, HasNearby* 11 s.
  - `Game.<side>.*` 30 s.
  - Player PlanetsControlled 3 s, Space/GroundTotal 10 s, Maxed* 5 s.
  - TradeRoutes forever (−1).
  - Most planet base/level leaves 0 (no cache).
- **Fog.** Fogged leaves (vt 0x8b4638 / 0x8c1608) are live whenever the mode's fog option is off: GameConstants +0x2EC `AIUsesFogOfWarGalactic`, shipped **False**. So all galactic values are live, `*.TimeLastSeen*` = 0 and `AverageAgeOf*Intelligence` = 0. `TimeLastSeen*` exists only as a suffix of a fogged leaf.
- **Normalisation suffixes** (PlanetForce, X = Ground/SpaceTotalUnnormalized). Result: num = 0 → 0; num ≤ den → num/den; else 1.

  | suffix | denominator |
  |---|---|
  | plain `GroundTotal` / `SpaceTotal` | `Game.<side>.X` + `Game.<opposite>.X` (share of all forces) |
  | `NBTD` | `Game.<side>.X` |
  | `NBPTM` | `Game.<side>.PerTargetMax…Force` (the strongest planet of that side) |
  | `NBTT` | `Target.<side>.X` without the caller's params (category fraction) |

  - `Game.<side>.X` sums `Target(p).<side>.X` over the target list **with the caller's params dropped**. Units in transit are excluded.
- **Force sides** (planet): Enemy = owner hostile to Self (0x39a4e0); Friendly = self or ally (0x39a2c0); unowned matches neither.
  - Space = units of orbiting fleets of that side (+ per-tech members); the starbase's member list, space-capable ground structures and orbital structures count only when the planet's owner is on that side.
  - Ground = the surface list (+ structure garrisons when the owner is on that side); Enemy ground adds planet-type list +0x20C8 entries affiliated with any player hostile to Self [M meaning].
  - `Parameter_Category` defaults to all bits; structures use type mask +0x1658 (default `Capital`).

### 4.2 Port corrections (port guess → original)

**Target (planet) root**

| Path | Original rule |
|---|---|
| `IsContestable`, `HasBuiltObject`, `Force`, bare `EnemyForce` / `FriendlyForce`, bare `TimeLastSeen*`, `Hints.High_Ground` | **not registered → fail** (the equation fails) [H] |
| `TargetPoliticalControl` | signed: (int)\|control\| / `MaximumPoliticalControl` (5), negative when the controlling player ≠ Self [H; M Self] |
| `TargetPlayerID` | the political-control record's player, not the planet owner [H] |
| `StarbaseLevel` / `GroundbaseLevel` (normalised) | level / 5 (GC max) [H] |
| `Max*Level`, `MaxStructureSlots`, `OpenStructureSlots(Unnormalized)`, `OpenGroundCompanySlots` | read the planet data pack (D+0x238 / +0x230 / +0x234 …); OpenStructureSlots = 1 − used/(ground + space slots); company slots = `Max_Ground_Forces_On_Planet` − surface forces [H] |
| `BaseIncome`, `TradeRoutes` | value / max over **all** planets [H] |
| `BaseIncomeNBTP` | / max over planets with the same TargetPlayerID [H] |
| `ActiveTradeRoutes` | the fraction of the planet's routes that are friendly to the owner [H] |
| `IsConnectedTo` | the neighbours are planets within `MaximumFleetMovementDistance` (100) **or** route-connected. Match on planet-type name ∈ `Parameter_Type` or `Target(n).IsFaction`. Attenuator default 0 → 1.0 on the first match; else max(1 − d/100·A) [H] |
| `HasStarbaseOfLevel` | level ≥ (int)(`Parameter_Level` + 0.5); fails if ≤ 0 [H] |
| `HasStructure` / `StructureCount` | structure lists + the current queue item (unless `Parameter_Only_Consider_Complete`); **no owner filter**; fails without `Parameter_Type` [H] |
| `ConnectsIsolatedPlanetsBy{Force,Length}`, `ConnectsLargestIslands` | flags set by island analysis 0x4e4850 (force-cost / length paths between Self's islands) [H] |
| `IsCorrupted`, `IsCorruptionTransitionActive` | 1.0 when true; otherwise the output is **left unwritten** (keeps 0) [H] |
| `TimeSince{Space,Ground}Conflict`, `TimeSinceConversion` | real stamps (signals 0x12 / 0x14 / 0x1A); never happened → **1e18** (not 1e6) [H] |
| `Markup` | SCRIPT_GAPS.md (`Apply_Markup` accumulator), default 0 |
| `Hints.X` | the campaign `Markup_Filename` file value, else the `Galactic_Hints` hint-set default (0); unknown X fails [H] |
| `Nearby{Space,Ground}Total` | Σ over neighbours **including the planet itself** of (1 − d/100·(1 − A)) × `Target(n).<side>.X`, A default **1.0**; / (both sides' Game totals), no clamp [H] |
| Missing in the port, used by shipped eqs | `OpenGroundCompanySlots`, `BlackMarket{AbilitiesAvailable, MinimumAbilityPrice, PriceModifier}`, `Type.Token`, `EnemyForce.{Space,Ground}UnitsBitfield`, `FriendlyForce.GroundUnitsBitfield`, `EnemyForce.AntiStealthEffectiveness` |

> **Status 2026-10-30:** These rows are now implemented in `engine/ai/src/galactic_ai_tokens.cpp`: `OpenGroundCompanySlots` (line 927), `BlackMarket*` (1055-1061), `Type.Token` (1054, [L]) and the `*UnitsBitfield` leaves (692). Still missing: `EnemyForce.AntiStealthEffectiveness` (line 729, per-unit value 0x395830 not modelled).

**Player root**

| Path | Original rule |
|---|---|
| `PlanetsControlled(Unnormalized)` | planets whose `TargetPlayerID` is the player [H] |
| `Space/GroundTotalUnnormalized` | walk the player's objects, **including in transit**. Space: DUMMY_STARSHIP / DUMMY_SPACE_FIGHTER_SQUADRON power; structures by +0x1658 except orbital dummies without +0x1E. Ground: DUMMY_GROUND_COMPANY only. Plus per-tech members [H] |
| `MaxedStarbases` / `MaxedGroundbases` | the fraction of owned planets where level == the planet's own max (max 0 counts as maxed) [H] |
| `MaxStarbaseLevel(Unnormalized)` (player) | the **max current level** over owned planets [H] |
| `NumStarbaseOfLevel` / `NumStructure` | max(0, n − round(`Parameter_Count`) + 1); Has* = (that > 0) [H] |
| `WorstIslandFractionOfLargest` | Self only: **second-largest / largest** island; < 2 islands → 0 [H] |
| `AnyCurrentThreats` | human player → 0; else 1 if a human-owned planet with a human fleet in orbit neighbours a planet of this player [H] |
| `AverageAgeOf{Space,Ground}Intelligence` | Self only (otherwise **fails**); the names are swapped in the original; 0 with fog off [H] |
| `BudgetFractionToBuild` | Self only: remaining cost of `Parameter_Type` / (btime × category income rate + category available) [H/M] |
| `CanAdvanceTech` | the next tech-upgrade type is buildable at some planet, or already queued [H/M] |
| `HasTechToProduce` | **any** listed type is affiliated with the faction, not unique-locked, and its tech ≤ the player's tech [H] |
| `HasUnit` | owned objects + queued items (unless complete-only) ≥ `Parameter_Count` [H] |
| `CreditsUnnormalized` | AI player: the galactic budget total 0x6136b0 [M]; human: credits |
| `IsDifficulty` | human or no AI → 0; else the AI difficulty index == the param [H] |
| Missing in the port, used by shipped eqs | `GrossIncomeNBPTM` / `NBTF`, `MaxStarbaseLevelUnnormalized`, `StoryTrigger`, `HomePlanet.Type.Token`, `Human.{Space,Ground}DefendedPlanets` (fraction of owned planets with that force present) |

> **Status 2026-10-30:** These rows are now implemented in `engine/ai/src/galactic_ai_tokens.cpp`: `GrossIncomeNBPTM` / `NBTF` (line 1308), `MaxStarbaseLevelUnnormalized` (913), `StoryTrigger` (1354) and `Human.{Space,Ground}DefendedPlanets` (1157). Still missing: `HomePlanet.*` including `HomePlanet.Type.Token` (line 1359, "not modelled").

**Game root**

| Path | Original rule |
|---|---|
| `Age` | the frame of the AI player's mode / FPS [H] |
| `Timeline` | mode +0x9C [M] |
| `IsCampaignGame`, `IsLandControlGame`, `IsSetupPhase`, `IsStoryCampaign` | real mode flags [H] |
| `ForceVisibility` | not registered in galactic → fails [H] |
| `PlanetsCorrupted` | / target-list size [H] |

### 4.3 Variable_Human with no human [H]

- The human binding is the first player in list order with `Is_Human` (0x294a00). The proposal, template, budget, FindTarget, Best_Of and `Evaluate_Perception` sites bind Human only when it exists.
- An unbound root makes the path **fail**; there is no default value.
- Effects at the call sites:
  - **proposal:** the candidate is skipped;
  - **template trigger:** the result is pre-set to 0 → not triggered;
  - **budget weight:** pre-loaded with the previous weight → keeps it.
- The last two assume the DLL leaves the output untouched on failure [L].
- Shipped equations that need Human: `Num_Enemy_*_Territories`, `Enemy_Planets_Controlled`, `Needs_More_Space_Force_Globally`, `Am_I_In_Winning_Position`, `*_Force_Limit_For_Unrestricted_Grab`, `Should_Shutdown_Offense_For_Story_Campaign`, `Underworld_Offensive_Budget_Allocation`.
- **This means AI-vs-AI galactic play in the original also loses every Human-dependent goal.** The port must reproduce that, not substitute the opponent.
- Enemy is bound only when the opponent index (context +0x30) ≠ −1.

### 4.4 `Script_` references [H]

- Every `Data/Scripts/Evaluators/*.lua` is loaded once (0x5e8220 / 0x5e7cb0) as a **single shared Lua instance** behind the root token `Script_<File>` (ScriptPerceptualEvaluator, vt 0x8b43e0).
- **Evaluate** 0x652040, with **no cache**:
  - set `PlayerObject` = Self, and `Target` = the context target if any;
  - call `Evaluate(<Parameter_Script_String values…>, <Parameter_Script_Number>)`;
  - then `Evaluator_Clean_Up()`.
- The result must be a Lua number; anything else fails. Script globals persist across calls and players.
- The only galactic one, `Script_AnyFreeLandSlots` (in `Allow_Blind_Ground_Production`), calls `FindTarget.Reachable_Target(..., "Can_Park_Ground_Unit", ...)`, which **consumes synced RNG** (weighted pick 0x698de0 → 0x532480).

---

## 5. Smaller items

### 5.1 Goal-system list +0x2A0 = Block_Goal_Proposal (AI_GALACTIC open item 6) [H]

- `TaskForce.Block_Goal_Proposal()` (0x5fa150 → 0x6c7e80) clears the proposed set (0x721820 on goal system +0xD8) and appends the plan's goal to +0x2A0.
- `Unblock_Goal_Proposal` (0x600a50 → 0x6cafe0) removes every occurrence of that goal.
- While +0x2A0 is non-empty, the system proposes and activates nothing for this player and mode (0x6ca110, Resolve). The scope is one goal system.
- Port:
  - keep a vector of blocking goal ids;
  - clear the proposed set on block;
  - remove the goal's entries when its plan ends. Plan-end cleanup was not traced; use the Unblock path, and record it as [L] if the plan exits without unblocking.
- Shipped users: `Library` helpers and one SpaceMode plan.

**Other bodies** [H]
- `Test_Target_Contrast(b)` (0x6008f0) writes PP+0x4D.
- `Are_All_Units_On_Free_Store` (0x5fa090) returns `not 0x6bffe0(PP)`.

### 5.2 Plan event code ↔ name (DAT_140b3a208, filled by 0x689200 from the static initialisers) [H]

| Code | Name | Code | Name |
|---|---|---|---|
| 0 | Space_Conflict_Begin [M: ecx zeroed] | 12 | Target_In_Range |
| 1 | Space_Conflict_End | 13 | Reached_System_On_Path |
| 2 | Unit_Destroyed | 14 | Unit_Damaged |
| 3 | Fleet_Destoyed (sic) | 15 | Special_Weapon_Online |
| 4 | Production_Failed | 16 | Hardpoint_Target_In_Range |
| 5 | Original_Target_Owner_Changed | 17 | Hardpoint_Opportunity_Target_Acquired |
| 6 | Current_Target_Owner_Changed | 18 | Unit_Move_Finished |
| 7 | No_Units_Remaining | 19 | Unit_Ability_Finished |
| 8 | Ground_Conflict_Begin | 20 | Unit_Diversion_Finished |
| 9 | Ground_Conflict_End | 21 | Unit_Ability_Cancelled |
| 10 | Original_Target_Destroyed | 22 | Unit_Ability_Ready |
| 11 | Current_Target_Destroyed | 23 | Unit_No_Threat |

This corrects AI_GALACTIC §1.9.4: owner changed = 5 and target destroyed = 10 were right; the galactic TF empty code 7 = No_Units_Remaining is confirmed. Handler names are `<TF>_<Name>`, with `Fleet_Destoyed` spelled as in the exe.

### 5.3 `Apply_Markup`

It is a real Lua global (0x69d110, ApplyMarkupClass). See SCRIPT_GAPS.md. It feeds the `Markup` planet token (§4.2).

### 5.4 FleetManagement paths used by the AI [H unless noted]

- `Land_Units` (0x6cec80):
  - ground units land directly;
  - transports detach (op 2) then land (op 5);
  - up to `Max_Ground_Forces_On_Planet`.
- `Launch_Units` (0x6cf070): launch (op 4), then form a fleet (op 0); aborts if blockaded.
- `Form_Units` (0x6cd5b0): merge (op 3).
- **Build tasks** use the same ops through 0x647b90 / 0x5afb60:
  - launch before pathing = op 4 then op 0;
  - pre-path separate = op 2;
  - transports to orbit = op 4 then op 0.
- **Free-store moves** split the unit into its own fleet before each hop (AI_GALACTIC §2.8).
- Op names stay [M]. The orders are executed directly (AI_GALACTIC §1.12).

---

## 6. Mid-game pacing: why the original keeps attacking

There is no engine-side "aggression" timer. Continuous offence comes from these interacting rules, all of which the port must have.

1. **The free store grows without attacks.**
   - `Build_Space_Forces` / `Build_Ground_Forces` (Global, desire `Allow_Blind_*_Production`) are non-removable plans with percent TFs and `AllowFreeStoreUnits = false`. The Plan score is capped by PP+0x3C, the category money by Time_Limit (§1.4).
   - Their units go to the free store.
   - Conquest PPs then score free units at 2–5 versus produced units at ≤ 0.5. Contrast is met mostly from the free store, so the conquest goal becomes fundable (net cost ≈ 0) and its PP Init succeeds. **If blind production is missing, Conquer goals mostly fail Find_Plan_For_Goal (contrast or budget veto) and are never proposed. This is the most likely cause of a passive port.**
2. **The proposal pass recurs every ~5 s.** Goal sleep is 0 in galactic. Every pass re-solves a PP per (goal type, target) with the current free store, so a goal that was unaffordable becomes proposable as soon as forces or money accumulate.
3. **The learning penalty expires.**
   - Per_Failure_Desire_Adjust (−5 … −10) per recent failure lasts only `Tracking_Duration`.
   - Contrast escalates with failures (f × PerFailureContrastAdjust).
   - Pruning runs every 10 s (AI_GALACTIC §1.11).
   - A port that never expires records stays passive after a few losses.
4. **Attack pacing is in Lua and pauses only after a conflict.**
   - `Exit_Plan_With_Possible_Sleep` (ConquerOpponentPlan; CrushPlan, RaidPlan and DeathStarUsePlan call `GalacticAttackAllowed` likewise) sleeps `DifficultyBasedMinPause` (Easy 300, Normal 200, Hard 45 s) **only** if the plan fought a conflict (`WasConflict`) and `GalacticAttackAllowed` returns false.
   - The sleeping plan keeps its goal active and non-removable, holding that slot. Other offensive goals may still be resolved within the category limit.
   - `GalacticAttackAllowed` [H, PGTaskForce.lua]:
     - with `num_ai_attacks_left` > 0: decrement it and allow;
     - otherwise draw `player_gains_before_attacking` = GameRandom(min, max) once (Easy 4–8, Normal 3–6, Hard 1–2) and snapshot `Num_Enemy_{Land,Space}_Territories`;
     - when the human has gained ≥ that many territories, `num_ai_attacks_left = GameRandom(0, max − 1)`. **The script uses `max_player_wins_unresponded − 1`; `max_ai_attacks_allowed` is unused** (shipped bug, preserve it). Reset and allow;
     - a negative gain re-snapshots.
   - Both territory counts are Human-rooted perceptions. With no human they fail (§4.3), so `EvaluatePerception` returns its failure value [L: nil → Lua arithmetic error inside the plan thread, killing that thread]. **In AI-vs-AI this path is unreliable in the original; record it as a probe item.**
5. **Desire bonuses.**
   - `FundBases` gives `Build_Initial_Starbase_Only` / `Groundbase` +15 for 5 s after a conquest.
   - `PathBonusForDistantTargets` (Always, desire `One`) and `GiveDesireBonus` in story and plans push follow-ups.
   - Global_Exclusions and the incumbent hysteresis (+1.6) keep active conquests alive until they resolve.
6. **Templates.**
   - `Basic_*_Default` keeps Offensive on with a budget equation.
   - Only `Shutdown_Offense` (story campaigns, `Should_Shutdown_Offense_For_Story_Campaign`, Human-rooted) turns conquest off.
   - `Tech_Upgrade` shifts money to MajorItem while triggered, which temporarily slows offence.
7. **Budget.** Offensive money accrues per income tick (§3.1). A port that keeps pools at credits × weight but charges conquest goals their full gross cost (not net of free units) will starve offence.

---

## 7. Open items

1. **PerceptionFunctionG.dll failure semantics:** whether `Evaluate` leaves the output untouched when a leaf fails, and whether any operator swallows a failure. This decides the template/budget behaviour without a human, and `EvaluatePerception` returning nil in `GalacticAttackAllowed`. Needs the DLL decompile or a probe.
2. **Event code 0** (`Space_Conflict_Begin`) is inferred from a zeroed ecx.
3. **Flush_Category body** 0x612cb0 and **Flush_All_Resources** 0x6931e0 / 0x6927d0 were not re-read.
4. **Budget tick period** 0x4b2010 and the meaning of the refund accumulator +0x80 (it is never reset outside init).
5. **Free-store eligibility field names** (§1.4 engaged test), the health threshold constant 0x140b27338 (.bss, set at runtime), and 0x5f22c0 (terrain preference).
6. **Plan end and Block_Goal_Proposal:** whether a plan's exit removes its +0x2A0 entries.
7. **Template int +0xE8** (extra goals per category, AI_GALACTIC §1.6) was not re-verified.
8. **Probe** (tools/probe_mod; Gabriel must launch): log the per-frame synced RNG draw count during a galactic AI-vs-AI proposal pass. Expected: Find_Plan pick + Plan Setup C team picks + IgnoreTarget contrast floats + MagicPlan production floats (§1.5).
9. **Token bodies not read:** GrossIncome / NetIncome / Maintenance tails, AverageStarbase/Groundbase, bitfield / antistealth details, planet-type list +0x20C8.
10. (2026-10-28) Survival = per-type survival ratio of the learning system (1.0 when unseen), the MagicPlan veto 0x375270 = "Super" category, flag +0x29 = squadron member, 0x5C/0x5B = DUMMY_STAR_BASE / DUMMY_GROUND_BASE: STRATEGIC_GAPS §1.1, §1.2, §1.5. Equation grammar / precedence: STRATEGIC_GAPS §3.

## 8. Rules the port should use instead of its first cut

| Item | Rule |
|---|---|
| Marshaller order | Galactic: FreeStore, Budget, TechTree, Plan, Production, Hero, TerrainType, Contrast, Survival. Setup A, then B, then C; score = sum in order; 1e18 = veto (permanent for Plan and Production); 50 % same-type rule; strict max, lowest index; commit all in order; ETA recomputed after each commit (§1.2) |
| Candidates | Types; one selection = one unit; candidate order = first insertion over TF → team → type list (+ upgrade variants, heroes) |
| Team specs | `a,b` → min a / max b; `N` → N/N; `N%` → percent renormalised to Σ = 100; category-mask teams expand over all non-dummy types with mask overlap and the TF filter (§1.3) |
| Scores | FreeStore 2 + (1 − dist/maxDist) [+2 for a flagged unit], 1.0 once found; Production 0.5 for a free unit, else ±0.5 speed; TechTree 0.4/level, veto above player tech; Plan 1 − n/max or 1.0 (percent); Contrast w(1 − (d/m)²), veto once met unless something is still unmet; Survival = success ratio; Budget veto when cost + net > avail + t·rate |
| Finalize | Fail on gross < Min + Add cost, slot < min, TF force/size below the minimum, a RequiredCategories mask unmet, empty selection without TaskForceRequired, contrast residual > tolerance |
| RNG | Plan Setup C int draws (≥ 2 teams, rejection sampling); IgnoreTarget contrast floats and MagicPlan production floats per candidate per iteration; all at proposal time |
| Production | ETA = max planet queue completion (travel only chooses the planet); market every > 30 frames per planet queue: re-check, claim one, bid (queue + build + travel) × level factor × cost ratio, start the least-slack task; no time-out; failures → `Production_Failed` |
| Budget | §3.1 tick formula verbatim, §3.2 reweight, §3.3 add-goal, time-to-afford and can-fund as given; net cost (free units free) |
| Tokens | §4.2 table; fog off = live values; failures propagate (no Human → fail) |
| +0x2A0 | Block_Goal_Proposal list; non-empty → no proposal or activation |
| Events | §5.2 table |
| Pacing | Blind production feeding the free store, learning expiry, Lua pause only after a conflict and only when `GalacticAttackAllowed` is false (§6) |

# A2k t02: RE answers to A2K_T02_RE_QUESTIONS (2026-11-06)

RE lane 4 of a2k. Answers to `A2K_T02_RE_QUESTIONS.md` §5 (Lane B, branch claude/a2k-laneB, 8d78fda; copied unchanged
next to this file). Sources: the Ghidra decompilation and a capstone disassembly of 0x5f1f90 of the FoC x64
`StarWarsG.exe`, and the shipped FoC XML / Lua (`CAMPAIGNS_UNDERWORLD_GC.XML`, `PLANETS.XML`,
`PathBonusForDistantTargets.lua`, `ConquerPiratePlan.lua`, `PGAICommands.lua`, `OffensiveGalacticGoals.xml`,
`DifficultyAdjustments.xml`, unit XML). No decompiled code is reproduced.

Tags: [H] read in code or data; [M] inferred from code with a gap, or depends on game state not read here; [L] a
guess or an estimate by hand.

Player rows as in the questions: Underworld (UW) is the AI. "Owned" below means 0x4ad9f0: the planet's political
record (0x3956c0, `{float control, int owner}`) has an owner whose relation to the player is 0 (self or ally) and
control > 0 (A2F_RE_READS §3.2).

## Q1. `_FindStageArea` (FindStageAreaClass::Lua_Call 0x6990f0)

### Q1(a) Which function returns the stage [H]

1. Arguments:
   - arg 1 must be a player wrapper (otherwise an error);
   - arg 2 is an AITargetLocationWrapper or a GameObjectWrapper;
   - arg 3 is an optional task force wrapper.
2. The AI target:
   - an AI target wrapper gives its target directly (wrapper +0x38);
   - a game object is resolved to its planet by walking the parent chain (+0x2B0) up to the first object with a
     planet index byte (+0x335 != -1). That planet index selects the entry in the player's AI target list
     (player +0x360 → 0x4b06d0(..., 0) → +0x38, vf+0x40). With no parent planet the call errors with "Couldn't find
     a parent planet for target".
3. The level: if arg 3 is a task force, level = its plan's goal type `Reachability` (TF +0xF0 → +0x88 → +0x48 →
   +0x2C). Without arg 3 the level is **0** (Friendly_only).
4. The call is `target vf+0x38` (its PlanetReachabilityClass), then **vf+0x30 = slot 6, 0x664450**, with
   (level, linkMask = -1, exclude = null, include = null).
5. Return value:
   - an AI target argument is wrapped back as an AI target (0x60e0a0);
   - a game object argument returns the stage's game object (0x5751d0);
   - a null slot-6 result makes the call return **nil**.
6. Arg 1 (the player) is used only to find the AI target list. The function calls 0x6021a0(player) a second time and
   discards the result.

PathBonusForDistantTargets.lua calls `_FindStageArea(PlayerObject, planet, EmptyForce)`. Its goal type
`Distant_System_Path_Bonus` has `<Reachability> Any </Reachability>` = **5** (the AIGoalReachabilityType table in
AI_GALACTIC.md) [H data].

### Q1(b) Candidates [H]

`PlanetReachabilityClass` slot 6 (0x664450, galactic):
- level == 4 (Single_Hop_Disconnected): return the target itself (reach +0x10);
- level > 5: return null;
- otherwise:
  1. refresh the source list for that level (0x665160(reach, level, 0));
  2. walk the list (reach +0x18 + level × 0x18) in order and return the **first** entry that is not in `exclude` and,
     when `include` is non-empty, is in `include`;
  3. the empty list returns null.
- **The linkMask argument is ignored in galactic.** The galactic `_FindStageArea` passes null exclude / include.

The source list 0x665160(reach, level, force):
- Cache: it is rebuilt only when `force`, never built yet, or `now − stamp > 10 × FPS` (strict; stamp = the frame of the
  last build, written before the build). **One cache per target reachability object and level**: the list belongs to
  the (AI player, target planet) pair, not to the player.
- Request (0x3eefd0): player = the reachability's player (reach +0x1D0 → +0x28), source = null, destination =
  **the target planet**. Flags from the level m: f0 = f1 = (m == 5), f2 = (m == 1 or 5), f3 = (m > 2), f4 = (m > 1),
  f5 (fog) = 1 (FIXPASS_GALACTIC §1.4). The level 4 bit is passed as the flood's "keep all" flag.
- The flood 0x3f12e0 then maps its planet output, in order, through the reachability's planet → AI target table.
  It also fills a per-level bit set (reach +0x128 + level × 0x10), which slot 5 tests.

The flood 0x3f12e0 (the galactic path finder singleton; node order and adjacency built by 0x3f1c10):
1. Player or destination null → empty.
2. f4 clear and the target is not owned → empty. So level 0/1 return nil for a target the player does not own.
3. Find the target's node. Then:
   - **Target owned** (and, when f2 is clear, not dangerous / corruption-benefit): BFS from the target (0x3f0ce0).
   - **Target not owned:** for each neighbour n of the target, in adjacency order:
     - skip n if this search already visited it;
     - if n fails the passability test 0x3f2400, mark it visited and skip it;
     - otherwise BFS from n into the spare buffer. Keep that buffer when its count of owned planets is **strictly
       greater** than the best so far, so the first neighbour wins ties.

     Visited marks are shared across these BFS runs. The target itself is not marked first, so a BFS from a
     neighbour can pass back through the target.
4. BFS 0x3f0ce0:
   - the start node is marked visited and makes up frontier 0;
   - frontiers are processed level by level, each in discovery order;
   - each popped node that is owned (0x4ad9f0) is appended to the output. With the "require base" argument (0 here)
     it also needs a starbase or ground base;
   - then its neighbours, in adjacency order, that are not yet visited are marked visited and, if 0x3f2400 passes,
     pushed to the next frontier.
   - Output = owned planets in BFS pop order.
5. Passability 0x3f2400(node):
   - fog: an AI player that is not human and is not revealed (0x3fa420) fails;
   - **passable for the player** (0x3f2510: owned or ally, corrupted with Benefits_From_Corruption, or allowed by
     f0 / f1): pass if f2, or corrupted + benefit, or not dangerous (0x3f18c0);
   - **impassable:** pass only if f3 and (not dangerous, or corrupted + benefit).
   - Under level 5 every revealed planet passes. Under level 2 (Enemy_destination) only the player's own / allied /
     corruption-benefit planets pass (f0, f1, f3 clear).
6. Node order and adjacency (0x3f1c10):
   - nodes are the GOM planet list (`FUN_1402a9ff0(..., 3, ...)`), the same query as `FindPlanet.Get_All_Planets`
     (0x697190);
   - a pair (i < j) is linked when a trade route joins them (0x3f95a0) or the 3-D distance is ≤
     `MaximumFleetMovementDistance` (100.0);
   - the link is appended to both nodes' lists while i rises, so **each adjacency list is in ascending node index**.

### Q1(c) Key and tie-break [H]

There is no weight, no hop count from the target, and no 2-D distance. The stage is **the first planet owned by the
player (or an ally) in BFS order**:
- the BFS starts at the target when the target is owned;
- otherwise it starts at the target's first passable neighbour in node order (with the largest-count rule above);
- it travels only through planets that pass 0x3f2400 for the level.

Ties at the same BFS depth go by discovery order, which follows node order (= Get_All_Planets order) through each
adjacency list.

### Q1(d) nil [H]

`_FindStageArea` returns nil when:
- the level is > 5 (any tactical threat level);
- the flood is empty: player or target missing, f4 clear (levels 0 / 1, including "no third argument") and the target
  not owned, or no owned planet reachable through passable planets;
- every entry is excluded (not for this caller).

Level 4 always returns the target itself.

### Q1 check against retail (HotM, Underworld)

Data [H]:
- Node order (= the port's observed Get_All_Planets order, which matches retail's markup sequence: Kashyyyk at
  frame ~222, then Kuat ~252):

  | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | Yavin | The_Maw | Tatooine | Shola | NalHutta | MonCalimari | Kashyyyk | Kuat | Kamino | Naboo | Felucia | Corellia |

  Galaxy_Core_Art_Model sits at (0,0,0) and has no link (nearest planet 114 > 100).
- Links (10 trade routes + 4 free hops ≤ 100: The_Maw–Kashyyyk 86.0, Tatooine–Naboo 60.2, Shola–NalHutta 84.9,
  MonCalimari–Kamino 77.6). Adjacency in node order:
  - Corellia: [Tatooine, Kuat];
  - Kuat: [Kashyyyk, Corellia];
  - Kashyyyk: [The_Maw, NalHutta, Kuat, Felucia];
  - Tatooine: [Naboo, Corellia];
  - Naboo: [Tatooine, Shola];
  - Shola: [The_Maw, NalHutta, MonCalimari, Naboo];
  - The_Maw: [Yavin, Shola, Kashyyyk];
  - NalHutta: [Shola, Kashyyyk];
  - Felucia: [Kashyyyk].
- Start owners: UW holds NalHutta, The_Maw, Shola, Felucia; the Empire holds Kashyyyk, Kuat, Corellia, Tatooine; the
  Rebels hold Naboo, Yavin, MonCalimari, Kamino.

Stages at level 5 under the retail rule (start owners) [H rule, M state]:

| target | BFS start | first UW planet found | Find_Path from it | Kashyyyk on the path? |
|---|---|---|---|---|
| Kashyyyk | The_Maw (first neighbour) | **The_Maw** (depth 0) | The_Maw → Kashyyyk | yes (0.2933) |
| Kuat | Kashyyyk | **The_Maw** (depth 1; Kashyyyk's list starts with The_Maw) | The_Maw → Kashyyyk → Kuat | yes (+0.3244) |
| Corellia | Tatooine (first neighbour) | **Shola** (depth 2: Tatooine; Naboo, Corellia; Shola, Kuat) | all-pairs Shola → Corellia; the port's weights pick Naboo, Tatooine | no, while that path is cheaper |

This reproduces retail's 0.2933 / 0.6178 sequence and no third markup at 372.

When can the Corellia handle put Kashyyyk on its path?
- The stage stays Shola as long as UW owns Shola and owns neither Tatooine nor Naboo.
- If UW does not own Shola at the walk (the Corellia handle is re-applied once per 12-planet walk, about 360 frames),
  the depth-3 frontier is [The_Maw, NalHutta, MonCalimari (from Shola), Kashyyyk (from Kuat)]. The stage becomes
  **The_Maw**, whose short route to Corellia is Kashyyyk–Kuat (236 vs 599 units).
- So retail's single 1.0122 sample at frame 22800 fits either a transient change of Shola's owner / control (0x4ad9f0
  needs control > 0) during one walk, or a matrix weight change that made Shola → Corellia go The_Maw/NalHutta,
  Kashyyyk, Kuat. The binary does not decide between them [M]; see Q3.

The port's `cheapest_stage` (lowest W[s][t]) happens to give Shola for Corellia here too. In general it is a
different rule:
- retail ignores weights and measures depth from the target's first neighbour, not from the target;
- the 10 s cache is per target, not per player.

### Q1 port change

All in engine/ai; no code here.
1. Replace the player-level source list with a per-target reachability object. Today `PlayerAi::source_planets(level)`
   (galactic_ai.cpp:555) and `PlayerAi::flood` (galactic_ai.cpp:531) flood from all of the player's planets. The new
   object keeps, per (AI player, target planet) and level 0..5:
   - the owned-planet list in BFS order, built as in Q1(b) with the request flags of FIXPASS §1.4;
   - the reached bit set;
   - the 10 s stamp.
2. `_FindStageArea` (the Lua binding that now calls `cheapest_stage`, ai_path_finder.hpp, with the `nearest_owned`
   fallback, galactic_ai.cpp:311): return list[0] at the TF's goal Reachability (0 without a TF), the target at
   level 4, nil when > 5 or the list is empty. Drop `cheapest_stage` and the target branch of `nearest_owned`.
3. The same list is the stage for FreeStore Setup A (Q2(c)) and the production source list of the ETA 0x6639f0
   (`GalPp::recompute_eta`, galactic_pp.cpp:168, and galactic_pp.cpp:555). The per-target bit set is what slot 5
   (0x664560) tests for `Reachable_Target` [M for that consumer, not re-read here].
4. Goldens move for every galactic AI run (markups, stages, ETA sources): the coordinator regenerates them.

Confidence:
- the rule: [H];
- node order = Get_All_Planets order: [H] same query and the order matches the retail markup sequence;
- the HotM ownership state: [M].

## Q2. The Conquer_Pirate @Kashyyyk solve (ConquerPiratePlan, UW, Medium)

### Q2(a) Capital-row enemy strength at Kashyyyk [H rule; M state]

Contrast Setup A 0x72bee0 → 0x6c5de0:
- Galactic: tolerance T = 1 − (f·PFCA + Min)/(f·PFCA + Max). ConquerPiratePlan sets Min 1.1 and Max 1.15, with
  PerFailureContrastAdjust 0.1 (PGAICommands default), so T = 0.0435 at f = 0 [H].
- The list is 0x5f1990(player, target, plan, out). **Order correction to AI_GALACTIC_2 §1.4:**
  - entry 0 = (null, 0x5f2430(…, 0, **1**) = the untyped **ground** total, byte 1);
  - entry 1 = (null, the untyped **space** total, byte 0);
  - then one entry per EnemyContrastTypes row with the ground value (byte 1);
  - then one per row with the space value (byte 0).
  - Flag 1 = `GroundTotalUnnormalized` (token 0x140a43d08) plus the ground structures and the garrison; flag 0 =
    `SpaceTotalUnnormalized` (0x140a43d0c) plus the starbase. The entry byte is "ground" [H]. AI_GALACTIC_2 had
    entries 0 / 1 labelled space / ground.
  - A row = 0x5f2430(row mask) × (Max + f·PFCA) × the AI's difficulty `Galactic_AI_Contrast_Multiplier`
    (0x4b0500 +0x34; Medium = 1.1, DifficultyAdjustments.xml). The two totals are unscaled. A total ≤ 0 zeroes its
    rows.
- ConquerPiratePlan defines no EnemyContrastTypes, so the rows are PGAICommands `Set_Contrast_Values`: Fighter,
  Bomber, Transport, Corvette, Frigate, Capital, … (loaded once by 0x5f2b80 for the plan map +0x68) [H].

0x5f2430(player, target, mask, groundFlag), space (flag 0):
1. v = the live perception value of `Target.EnemyForce.SpaceTotalUnnormalized` with `Parameter_Category` = mask
   (leaf 0x6dbd50). That is: the power of the enemy fleet units in orbit plus their per-tech members, plus the hostile
   starbase type's **per-tech members** (Tech_0 fall-back), **not** its own power (A2I_TOKENS_RE §2.1).
2. Then, if the planet is **not owned** by the AI or an ally, add the starbase type's own `AI_Combat_Power` (D +0x140,
   0x3f8b40) **when its CategoryMask meets the row mask** (A2F_RE_READS §3.1).
3. Perceived or live: `AIUsesFogOfWarGalactic` is False in the shipped GameConstants, so the fogged leaves answer with
   **live values**, subject only to the leaf cache lifetime (AI_GALACTIC_TOKENS §0) [H]. It is not a last-seen value.

Kashyyyk (Empire_Star_Base_1 + Acclamator_Assault_Ship, start forces) [H data]:
- Empire_Star_Base_1: CategoryMask Capital, AI_Combat_Power 5000, Tech_0 members 2 × TIE_Fighter_Squadron +
  2 × TIE_Bomber_Squadron.
- Acclamator: Frigate | AntiCorvette, 2500, members 1 TIE_Fighter_Squadron + 1 TIE_Bomber_Squadron.
- **Capital row** = 0 (no Capital unit in orbit; the TIE members are Fighter / Bomber) + 5000 (the starbase's own
  power) = 5000. Target = 5000 × 1.15 × 1.1 = **6325**. This is the port's 6325: its "5500 × 1.15" is 5000 × 1.1 ×
  1.15. The starbase's spawned squadrons count only as members in the Fighter / Bomber rows.
- **Frigate row** = 2500 × 1.265 = **3162.5**, as long as the Acclamator is alive in orbit. Fighter / Bomber rows come
  from the starbase and Acclamator members.
- Lane B's port trace has the Kashyyyk node cost falling from 75 to 50 after the port-only battle at 287. 7500 → 5000
  is exactly the Acclamator's 2500, so at the port's solve (323 / 410) **the Acclamator is gone and there is no Frigate
  row**. In retail there is no Kashyyyk battle in this window, so the Frigate row should exist [M: assumes the Empire
  did not move it].

### Q2(b) The Kedalbe's P and w [H]

- P (Contrast score 0x72b940 and commit 0x6bf840) = AI_Combat_Power × the hull fraction of the chosen free unit
  (0x396df0). A fresh Kedalbe_Battleship gives **P = 4250**.
  - 0x6bf840 also multiplies by a per-category factor array: for each set bit b of the type mask, take the larger of
    the bits' values plus 1 and multiply when non-zero. The array is zero in vanilla (`ContrastTypeScale = {}`)
    [M: what the array is].
- w = **0x5f1f90**(T, plan, row key), read from the disassembly:
  1. no row → 0;
  2. walk the row's FriendlyContrastTypes entries in order: an entry that **is exactly T** returns its weight at once;
  3. otherwise, every entry whose category mask meets T's CategoryMask sets "matched", and when its weight ≠ 1.0 adds it
     to a sum and a count;
  4. result = sum / count if count > 0; else 1.0 if matched; else 0.
- Then × `0x5f22c0` (terrain effectiveness, 1.0 in vanilla). The commit applies it only to **ground** rows (entry
  byte set) with a known terrain.
- Kedalbe = Capital | AntiFrigate:
  - **Capital row** {AntiCapital 2.0, Corvette 0.33, Frigate 0.5, Fighter 1, Bomber 1, Capital 1.0}: only Capital
    matches → **w = 1.0**, w·P = 4250;
  - **Frigate row** {AntiFrigate 3.0, Corvette 0.5, Fighter 1, Bomber 1, Frigate 1, Capital 1}: AntiFrigate 3.0 and
    Capital 1.0 match → mean of the non-1.0 weights = **3.0**, w·P = 12750.
- So **one Kedalbe does not meet the Capital row**: 6325 − 4250 = 2075 left. The port's "met with 125 to spare" is
  Kedalbe 4250 + 2 × Vengeance_Frigate (Capital w = Frigate 0.5 × 2200 = 1100 each) = 6450, i.e. 125 over 6325. All
  three come from The_Maw.
- Score per pair (0x72b6a0): s = (1 − (d/m)²) × w, with d = max(R − wP, 0) and m = max(R, wP); only w > 0 and R > 0
  count. With the Frigate row present:
  - Kedalbe vs Frigate: R 3162.5, wP 12750 → **s = 3.0**;
  - Kedalbe vs Capital: R 6325, wP 4250 → s = 0.892.
  - The Kedalbe takes the Frigate entry (rec +0x14). Commit 0x6bf840 subtracts w·P **only from that entry**, plus
    the raw P from the untyped total of its layer [H]. In retail's state the Kedalbe therefore covers the Frigate row
    and contributes nothing to Capital.
- The port's `ContrastTable::contrast` (engine/galactic/src/auto_resolve.cpp:61) returns the **first** matching entry.
  It agrees with 0x5f1f90 for every UW unit in this solve (Kedalbe, Vengeance, Interceptor4, Crusader, StarViper
  against these rows) [H by hand]. It differs for a type that matches a 1.0 entry before a non-1.0 one, or two
  different non-1.0 entries.

### Q2(c) Is a free unit at The_Maw eligible [H code, M field names]

FreeStore Setup A 0x7287a0, per TF:
1. **Stage** = target reachability slot 6 at the goal's Reachability (Conquer_Pirate: `Enemy_destination` = 2),
   linkMask TF +0x94, exclude = the stages so far when TF +0x98, include = them when TF +0x99. With no result the
   stage is the goal target.
2. For Kashyyyk at level 2:
   - Kashyyyk's first neighbour The_Maw is UW-owned with a UW starbase, so not dangerous and passable. The BFS from
     The_Maw through UW planets only (f0, f1, f3 clear) gives [The_Maw, Shola, NalHutta].
   - Felucia's BFS has 1 entry, fewer than 3.
   - So the **stage = The_Maw** for MainForce, not Kashyyyk.

Then each free-store unit, in free-store list order:
1. Reservation: skipped when reserved by another PP (+0x30 ≠ 0), except on the first solve (PP +0x104 == 0).
2. Health: skipped when the health 0x396470 < constant 0x140b27338.
3. Galactic distance key (target present):
   - Units with 0x3a9760 true (object +0x3A0 bit 1, or a fleet / team sub-test) skip the key and keep key 0.
   - The unit's planet: walk the parents to the first object with +0x335 != -1, or through the fleet (+0x336) to its
     planet. It must exist and pass `Is_Planet_Revealed` 0x3fa420, **else the unit is skipped**.
   - For each TF stage: c = stage reachability slot 7 (0x663810)(unit planet, mode 3; mode 4 when the goal
     Reachability is 4):
     - 0 when the unit is at the stage;
     - otherwise the AI all-pairs path unit planet → stage (0x690f10). For mode 3 the path must pass 0x6913a0 with
       threshold 1e-4, i.e. **every planet after the start has node cost 0** (the high 16 bits of the summed hop
       weights are 0). Then c = (low 16 bits of that sum) / 10, about the path length in distance units;
     - otherwise c = 1e18.
   - key = Σ_tf c² (finite c only) + Σ_tf (2-D distance unit position → stage position)²; divided by the number of
     TFs when any c was finite.
   - **Galactic: a unit with no finite c is skipped** (the tactical branch would fall back to the distance to the
     goal target) [H]. The maximum key goes to 0x140b3a520.
4. Engaged test: reserved by this PP, or AllowEngagedUnits (PlanDef +0xD1, default 1; ConquerPiratePlan does not change
   it), or not battle-pending (+0x348 / +0xA8 → +0x1C4). And either:
   - +0x361 == -1 and +0x369 == -1 [M: two behaviour-slot bytes, -1 = absent]; or
   - +0x158 ≠ 0 and 0x4914e0 ≥ 1.0 and (no mode object, or vf+0x220 null, or 0x527620 true).
5. Not building or moving (+0x210 state, +0x3A7, +0x3B4, 0x39ad40).
6. Type without behaviour 0x20 for this mode (0x370320).
7. Not attached to an object that refuses detach (+0x35B → 0x42e3b0 / 0x42e3d0).

Answer:
- **The Kedalbe at The_Maw is eligible**: it sits on the stage, so c = 0 and the key is the smallest [H code; M that
  none of the state bytes is set at frame 300..420].
- **Every UW unit at Felucia is ineligible in retail.** Felucia's only link is Kashyyyk, whose node cost is
  (Empire SpaceTotal + starbase 5000)/100 ≥ 50, so the Felucia → The_Maw path fails the zero-node-cost test and
  c = 1e18 [H code; M state]. Felucia holds StarViper, Interceptor4, Crusader and three ground companies (start
  forces).
- Shola and NalHutta units reach The_Maw through UW planets only (NalHutta–Shola is a free hop), so they are
  eligible.

### Q2 what stops retail from activating Conquer_Pirate @Kashyyyk at desire 0.311

Gates, in the order the binary applies them [H; AI_GALACTIC §1.6, A1_READINGS §28.6]:
1. **Proposal:** `Find_Plan_For_Goal` → PP Init → first solve. A solve that fails (any Finalize clears PP +0x4C)
   gives no plan, so **no proposal**: no activation-failure record and no −5 penalty. The desire stays 0.311.
2. **Accept**, in Resolve_Category for Offensive:
   - the greedy phase runs only while the category's available credits (cat +0x44) are **> 0**. Then it needs the
     re-solve 0x6c4a50 (reservations now enforced) to succeed and 0x6129c0 to fund it; otherwise an activation failure
     is recorded (Per_Activation_Failure_Desire_Adjust −5 for Activation_Tracking_Duration 60 s);
   - with credits ≤ 0 the weighted random phase can still pick it (eff 0.311 > 0, fundable, net cost 0), subject to
     the same re-solve.
   - AllowedTime never rejects a net-cost-0 free-store plan here.
3. **Contrast in the solve:**
   - each candidate scores against its best (entry, w) pair;
   - Contrast vetoes when v ≤ 0 and the layer is not yet satisfied;
   - **Finalize 0x72af80 fails** when an active layer's untyped total has remaining/target > T, or the layer's typed
     residual Σ max(R, 0) / Σ target > **T = 0.0435** (0x72ac70; one test per layer, not per row).

The retail inputs differ from the port's at 323 / 410 in two binary-grounded ways:
- (i) **the Frigate row exists** (the Acclamator is alive), and it captures the Kedalbe (s 3.0 vs 0.892);
- (ii) **Felucia's units are not eligible** (stage The_Maw, zero-node-cost path rule).

Hand estimate [L], start forces, the Acclamator in orbit, TIE squadron powers 140 / 120:
- Space typed targets ≈ Capital 6325 + Frigate 3162.5 + Fighter ≈ 531 + Bomber ≈ 455 ≈ 10474. The layer allows
  ≈ 455 residual.
- After the Kedalbe takes Frigate and a Crusader (w 4.0) each takes Fighter and Bomber, the eligible units left for
  Capital are:
  - 2 Vengeance × 1100;
  - 2 Interceptor4 (Shola, NalHutta) × 700 (w 0.5);
  - 4 StarViper squadrons × 400 (w 1.0);
  - 1 Crusader × 412.5 (w 0.33).
  - Total ≈ 5612, leaving ≈ 713 on Capital, over 455, so **the space layer fails and the solve fails** at proposal.
- With Felucia's three space units (+1512) the residual would be ≈ 0 and the solve would pass.
- So in retail's state each difference alone could decide the outcome. The estimate ignores selection order, the 50 %
  rule and FreeStore / Production / Survival score terms, so it is [L].

The likely reading [M]: retail never proposes, or never accepts, the attack because its PotentialPlan solve fails on
the space contrast layer. The port passes because:
- (a) its port-only battle at 287 removed the Acclamator and with it the Frigate row (a trajectory gap, not this
  mechanism);
- (b) its FreeStore Setup A does not apply the stage / finite-path filter.

The sign of the desire is not the cause.

### Q2 port change

- **engine/ai `FreeStoreM::setup_a`** (galactic_pp.cpp:298-333):
  - stage per TF = the target's reachability list[0] at the goal Reachability, with TF +0x98 / +0x99
    exclude / include (Q1 port change 1);
  - key = Σ c² + Σ 2-D d² to the **stage**, where c is the zero-node-cost path length above (0x663810 mode 3, or 4),
    not hops to the target;
  - galactic: **drop units with no finite c** and units whose planet is unrevealed or not found;
  - 0x3a9760 units keep key 0.
  - Today the port uses hops² to the target and keeps every unit.
- **engine/ai `GalPp::power_eff`** (galactic_pp.cpp:154): × the chosen free unit's hull fraction (0x396df0). Today it
  is health 1. This matters after battles.
- **engine/galactic `ContrastTable::contrast`** (auto_resolve.cpp:61): exact type first; else the mean of the matching
  weights ≠ 1.0; else 1.0 if any matched; else 0 (0x5f1f90). No effect on this HotM solve.
- **engine/ai contrast entries**: entry 0 = untyped ground, entry 1 = untyped space, then ground rows, then space rows
  (0x5f1990). This only matters for score ties, where the lowest entry index wins.
- The commit's terrain factor applies only to ground rows (0x6bf840) [H]. Check `recompute_remaining`
  (galactic_pp.cpp:262), which applies `weight()` with terrain to every row.
- The Acclamator / port-only battle at 219 / 287 is a separate trajectory item. Lane B already lists it.

## Q3. What a retail probe has to log (not run here)

The binary settles the rules. Two state questions stay open:
- is the Acclamator still at Kashyyyk at 300..420, and does the solve fail at proposal or at accept;
- what happens at frame ~22800.

A kit (tools/probe_mod, run / cleanup scripts, Gabriel launches) would log, for the Underworld AI:
1. Every 30 frames from 240 to 600, through equations the shipped files already evaluate (A2J_T02_PROBE_PLAN §3,
   typed tokens of the a2f DT channel):
   - `Target.EnemyForce.SpaceTotalUnnormalized` at Kashyyyk with `Parameter_Category` = each of Capital, Frigate,
     Fighter, Bomber, Corvette, Transport, and untyped;
   - the same for `GroundTotalUnnormalized`;
   - `StarbaseTotalUnnormalized` at Kashyyyk.
   - Expected under this spec: Capital leaf 0 + starbase 5000 (the probe sees the leaf only, so 0), Frigate 2500
     while the Acclamator lives.
2. The Empire's fleet contents at Kashyyyk and Kuat, by type, at the same frames. This answers "Acclamator alive and
   in orbit, not moving".
3. UW free-store units by type and planet at 300..420 (`Find_All_Objects_Of_Type` + `Is_Object_On_Free_Store`), and
   whether any UW unit leaves The_Maw / Shola / NalHutta / Felucia in that window.
4. The Offensive category's available credits are not reachable from Lua. Log UW `CreditsUnnormalized` and whether
   any Offensive plan starts (the goal-system debug log, if the kit's channel has it). This tells "no proposal"
   (solve failed) from "proposal not accepted" (credits or re-solve).
5. Around 22500..23100: the owner and political control of Shola, Naboo, Tatooine and Kuat each 30 frames, and the
   Corellia path.
   - The path needs a probe plan with a task force (`_FindStageArea(PlayerObject, Corellia, tf)` with a goal of
     Reachability Any): without a task force the call is level 0 and returns nil for an enemy target (Q1(d)). If
     a probe plan is not safe, the owners alone separate the two explanations of the 1.0122 sample.
   - Also log the UW node weights of Naboo, Tatooine, Kashyyyk and Kuat if a token exposes them (EnemyForce
     SpaceTotal of each planet is enough: node cost = (that + hostile starbase)/100).

## Open items and the evidence that settles each

| # | open | settles it |
|---|---|---|
| 1 | whether retail's Acclamator is at Kashyyyk at 300..420 (the Frigate row) | Q3 items 1-2 |
| 2 | whether the retail solve really fails (hand estimate [L]) | after porting Q2's FreeStore change, run the port's solve with the Acclamator present and Felucia units filtered, and compare with Q3 item 4 |
| 3 | proposal failure vs accept failure (credits, re-solve, −5 penalty) | Q3 item 4 |
| 4 | the frame-22800 Corellia markup: Shola owner change or matrix weights | Q3 item 5 |
| 5 | meaning of +0x361 / +0x369, 0x4914e0, 0x3a9760 in Setup A | not needed for this solve (start units); read 0x4914e0 / 0x527620 if a later solve hinges on them |
| 6 | per-category factor array in 0x6bf840 (zero in vanilla) | read its source if a mod sets `ContrastTypeScale` |
| 7 | slot-5 consumers (`Reachable_Target`) with per-target lists | re-read 0x698360 against Q1(b) when the port moves to per-target reachability |

## Confidence summary

| claim | tag |
|---|---|
| `_FindStageArea` = target reachability slot 6, level = TF goal Reachability (0 without TF), linkMask ignored | H |
| stage = first owned planet in BFS from the target / its first passable neighbour, node order adjacency, per-target 10 s cache | H |
| retail stages Kashyyyk → The_Maw, Kuat → The_Maw, Corellia → Shola at start | H rule, M state |
| contrast row order (entry 0 ground, entry 1 space) corrects AI_GALACTIC_2 §1.4 | H |
| Capital row 6325 = starbase 5000 × 1.15 × 1.1 (live values) | H rule + data |
| w = 0x5f1f90 rule; Kedalbe w 1.0 (Capital), 3.0 (Frigate); P = 4250 × health | H |
| one Kedalbe does not meet Capital; the port's margin comes from 2 Vengeance | H arithmetic |
| Kedalbe at The_Maw eligible; Felucia units ineligible (no finite stage path) | H code, M state |
| retail solve fails on the space layer (Acclamator row + Felucia filter) | L estimate, M mechanism |

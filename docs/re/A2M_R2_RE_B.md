# a2m round 2, RE agent B: production cancel on task end, galactic bounty, hero selection (H1)

Date: 2026-11-12. Source: Ghidra decompile of Steam FoC x64 (`re_reference/ghidra/StarWarsG_decompiled.zip`, `Global.c`),
the shipped FoC XML / Lua, and the existing docs/re specs. Behaviour only; no code copied. Addresses are RVAs.
Tags: [H] read in the decompile, [M] inferred from code paths or data, [L] guess.

Questions (from the a2m r2 Underworld and Rebel fix lanes):
- Q4: which path removes an in-production AI queue entry with a refund when its plan or build task ends, and when.
- Q5: the galactic auto-resolve bounty: call path from the kill to 0x392600, who is paid, multiplier, rounding,
  starbases and ground structures. Check the port on `origin/claude/a2m-fix-underworld-r2`.
- Q6 (probe H1): which heroes and flagships the AI unit selection may pick for a task force, the HeroM veto, and why
  every ContrastToReconnect / Conquer_Pirate instance asks for new production.

Corrections to existing docs are listed in §4. Short answers:
- Q4: the cancel is 0x53ba60, run at the start of every AI planet-queue service. It cancels the producing entry of an
  **ended** build task with the paid-fraction refund, **unless the PAYG reclaim (+0xD9) took the task**. The reclaim runs only when the
  goal system releases a goal's budget while the goal still has its plan. So: goal-system release → the funded builds
  finish and the unfunded builds are cancelled; a plan that ends by its own script → all its in-production builds are
  cancelled.
- Q5: 0x60da50 does not call the bounty. The auto-resolve kill (combatant vf +0x120, with the opposite side's first
  combatant as the killer) runs the object death handler 0x39bdb0, which calls 0x392600. In galactic mode the
  killer is paid the type's bounty (rounded, no percentage) when the killer's faction has `Benefits_From_Corruption`
  **and** the dying object's planet is corrupted with type 3 (Racketeering). Galactic starbases and structures
  removed with a planet pay nothing. The port's activation rule and category rule are wrong (§2.6).
- Q6: Home_One and Sundered_Heart are named heroes with object scripts. Both scripts call `ScriptExit()` outside
  space mode, so the galactic objects lose their scripts on their first galactic pump. Without a script they are not
  hero records, so HeroM scores them 0 and they are ordinary units for the FreeStore / Contrast selection. The port
  treats "the type has a Lua_Script" as "has a script", and so it vetoes them.

---

## 1. Q4: in-production AI builds when the build task ends

### 1.1 The cancel: AIPlanetBuildQueue 0x53ba60 [H]

Q is an AIPlanetBuildQueue (one per planet per queue index, AI_GALACTIC_2 §2.3): Q +0x10 task list, Q +0x18 current
(producing) build task, Q +0x20 production entry id, Q +0x28 planet object, Q +0x54 queue index.

0x53ba60(Q) does the following when Q has a current task (+0x18) **and that task has ended** (task +0xA8 set, which only
0x64ad00 does):
1. P = the player whose id is the planet owner's id (0x3956c0 → id → 0x294bc0).
2. If P pays as it goes (0x289050; AI players) **and** the task's reclaim flag +0xD9 is clear:
   - remove the **head entry** of queue Q +0x54 at planet Q +0x28 with refund = true (0x403900, the same "cancel the
     head" the UI uses, GALACTIC_ECONOMY §2.6);
   - Q +0x20 := −1.
3. In every case (PAYG or not, reclaimed or not): drop the reference to the task, Q +0x18 := null.

The function does not check that the head entry's id equals Q +0x20. The head is the entry the task started (production
start 0x64abf0 binds the task to the head) [M].

What 0x403900 does with refund = true [H]:
- refund x = 0x499000(entry). For a PAYG payer (the entry's player +0x20) this is clamp(1 − remaining/T, 0, 1) × the
  entry cost (+0x28). remaining is the cached remaining seconds (+0x1C) and T the build time (+0x10), so x is the
  paid fraction.
- Add_Credits(P, x, **notify 1**) (0x4023d0, logs "Planet %s: Refunding %.2f credits to %s player (ID %d) for %s").
  - Add_Credits step 1 multiplies a positive amount for an AI player by its `Credit_Multiplier` (GALACTIC_ECONOMY §1.1).
  - The notify flag adds x' × w to every galactic budget category's P (0x615b40, A2K_T10 Q2).
- Raise signal 3 on the planet with the removed entry. The queue's own listener 0x53c4f0 then sees an ended task with
  no produced object and only clears Q +0x18 / +0x20.
- Remove the entry and start the next one (0x4033c0).
- The build-cancelled sound is played only for the local player.

### 1.2 When 0x53ba60 runs [H]

- At the top of the AI queue service 0x53cd60, when Q has a task list (+0x10). The planet production service 0x402840
  calls 0x53cd60 for queue 0 and queue 1 of every planet before it advances the production queues.
  - So the cancel happens at the planet's next production service after the task ended, before that frame's
    progress.
  - A task that ended earlier in the frame is cancelled in the same frame when its planet is serviced after the end
    [M: production service runs every frame].
- At the top of the queue save 0x53c7f0. A save between the task end and the next service applies the cancel first.

### 1.3 Who sets +0xD9: the PAYG reclaim [H]

The PAYG reclaim 0x5f7d40(plan, amount) (AI_GALACTIC_3 table, AI_GALACTIC_2 §3.4) runs only inside the budget release
0x614880(budget, goal). The release does this:
1. A += c + a for the goal's BudgetedGoalStruct, and deletes the struct.
2. Then, only if c + a > 0, the context is galactic, the player is PAYG **and the goal still has its plan** (goal +0x60):
   - r = 0x5f7d40(plan, c + a);
   - A −= r, P −= r, budget +0x80 += r.

0x5f7d40 does this:
- It takes the plan's build tasks from the execution system's list (+0x40) whose production has started (task +0x78 ≠ −1)
  and whose remaining cost is in (0, amount].
  - The remaining cost is 0x649130: (1 − p) × cost of the head entry of the task's queue at its producing planet
    (task +0x70), where p is the entry's cached progress (0x401b30).
- It sorts them oldest first, ties by AI_Combat_Power descending.
- It walks them in that order, and takes each task whose cost still fits the remaining amount. A task that does not
  fit is skipped, and the walk goes on.
- Each taken task gets +0xD9 = 1. r = the sum of the taken costs.

Callers of the release 0x614880, all in the goal system, all with the plan still attached:
- the category maintenance 0x722a30. This covers the goal that can no longer be funded (0x6129c0 false), the goal past
  its ETA allowance, the goal whose running plan fails the contrast re-test (0x6c47c0), and a finished or plan-less
  goal (this last case does no reclaim, because the goal has no plan any more).
- the resolve 0x6c8970, for goals dropped at a pass, followed by goal deactivation 0x693750.

The release runs before anything ends that plan. So the plan's tasks are still in the list when the reclaim walks it.
How the released plan ends afterwards (goal reference drop → goal destructor 0x691840 → 0x5f6a50, or the plan
service) is [M].

### 1.4 Who ends a task (+0xA8) [H]

All of these go through 0x64ad00. It sets +0xA8 and state 0x15, unregisters the task's signals, frees its bid record,
and re-admits the claimed unit (+0x68) to the free store unless +0xD8 is set (the unit was already handed to the task
force).

| caller | when |
|---|---|
| 0x5f6a50 plan end, first loop | every build task of the plan (execution-system list), at any plan end |
| 0x4d5150 free-store cleanup of the plan (plan end) | the plan's tasks held by the free store |
| 0x6c6550 `ProduceForceBlockStatus` destructor | the block's tasks (block destroyed) |
| 0x6d0370 task force loses its last unit | the TF's build tasks (A2M_T08_T10_T14 §t02) |
| 0x53cd60 / 0x53c4f0 (signal 0xE) | Production_Failed paths (bid < 0, planet lost) |
| 0x649be0 (signals 0x10 / 0x1b / 0x22), 0x64aec0 (FSM) | the task's own unit / production events |

Plan end paths:
- **Self end.** The plan service 0x5f8310 sees the script finished (exit flag +0x121 or no live thread). It
  deactivates the goal (0x693750) and runs 0x5f6a50. 0x5f6a50 detaches the plan from the goal (0x693970(goal, 0)).
  No budget release happens on this path, so the later release finds no plan and reclaims nothing.
- **Goal-system end.** The release 0x614880 runs first (§1.3), then the plan ends.

### 1.5 The rule

A task ends while its unit is in production at planet X:
1. Ended by a goal-system release with c + a > 0, and taken by the greedy reclaim: production continues. The finished
   object goes to the free store (signals 3 / 4 with the task gone, AI_GALACTIC_2 "Claiming the produced unit").
   - The remaining installments are paid through the no-task branch of the installment 0x53b900 (Q +0x18 is now null).
   - That branch debits budget +0x80, which the reclaim credited.
2. Every other case (not reclaimed, or the plan ended by its own script, or the task force emptied, or a Production_Failed
   path): at X's next production service the head entry is cancelled and the owner gets back the paid fraction ×
   cost × `Credit_Multiplier`, notify 1. Exception: a human owner (not PAYG) keeps the production; it then goes to the
   free store.

Confidence:
- the mechanism: [H];
- that retail's cancels at 2959 / 4253 are case 2 and the completed saboteurs (1697, 2844, 3087) case 1: [M]. The
  amounts match the paid fraction (`A2M_R2_UW_BOUNTY_TRACE.md` §2 on the Underworld branch). Which plan-end path each
  one took is not in that note. The Q2 traces with plan hooks (`tools/probe_mod/q2_traces`, existing runs, no new run)
  can tell: a plan end logged as released by the goal system vs a script exit at the same frame.

### 1.6 What the port must do

Port state (dev b39baeb5): `GalacticAiWorld::Impl::end_plan` computes the reclaim at every plan end, whichever path ended
it. `end_plan_builds` then marks a producing queue `kQueueTaskEnded` / `kQueueTaskEndedReclaimed`, and the production
always finishes.

1. Compute the reclaim only on the goal-system path (`end_plan(id, by_goal_system = true)`), from the goal's c + a at
   the release, and before the plan's tasks are ended. With `by_goal_system = false` (script exit, no live threads),
   there is no reclaim.
2. When a build task ends while it produces, and is not reclaimed, and the player is PAYG:
   - at that planet's next production service, before the queue advances, cancel the head of that queue;
   - refund `clamp(1 − remaining/build_time, 0, 1) × cost` with `add_credits(owner, x, notify = true)`, which applies
     `Credit_Multiplier` and budget P += x' × w;
   - free the queue.
   Use `GalacticSim::cancel(player, planet, queue, 0, refund = true)` if it has these semantics.
3. Apply rule 2 to the other task-end paths too: TF emptied (0x6d0370), Production_Failed (0x53cd60 / 0x53c4f0) and the
   ProduceForce block destructor, not only plan end.
4. Reclaimed tasks: unchanged (finish to the free store, installments from the refund pool).
5. Save: apply pending cancels before writing the queue state (0x53c7f0) [port choice whether to need it].

Do not ship the unconditional cancel. The DIAG run showed a build-and-cancel loop, and retail keeps the reclaimed
builds.

---

## 2. Q5: galactic auto-resolve bounty

### 2.1 Call path [H unless marked]

- 0x60da50 only appends (type, owner) loss records for the result screen (BATTLE_TRANSITIONS §4.4 step 6). It does not
  call the bounty.
- The kills:
  - 0x60aac0 (loser transports) and 0x60a140 (survival roll) kill a combatant through its vf +0x120(1, damage source
    CRC 0x5d5e0b6b, **killer**).
  - In 0x60b930 the killer is fixed before the rolls:
    - for the loser's units (and the transport losses): the first combatant in the **winner's** list;
    - for the winner's units: the first combatant in the **loser's** list.
  - So the killer's owner is the other battle side [H for the selection; M that vf +0x120 reaches the object damage
    0x3ab890 with that killer].
- Object damage 0x3ab890: health ≤ 0 → death handler 0x39bdb0(object, …, killer object, …, killer player id).
- 0x39bdb0 [H]:
  - sets the dying object's byte +0x330 (killer player id) to the killer object's owner (+0x58), or to the explicit
    killer id when there is no killer object;
  - also sets it on the parent (+0x2B0) when the parent has behaviour 0x16;
  - **if bounty collection is on (global 0x140b305e9), calls 0x392600(object)**;
  - then continues the death; linked objects (+0x150) die through 0x39bdb0 with the same killer.
- So every galactic object that dies is checked separately: each fighter of a squadron, each company member, each
  transport, each ship.
- Bounty collection is on unless story reward `DISABLE_BOUNTY_COLLECTION` (code 0x7C) clears it.
  `ENABLE_BOUNTY_COLLECTION` (0x7B) sets it. Its start value is 1 [M: the traces pay bounty; only the Underworld story
  mission 1 toggles it].

### 2.2 0x392600(object): gates and amount [H]

Let K = the killer id (+0x330), O = the object's owner, T = its type.

Gates:
- K ≠ −1;
- O is absent, or O ≠ K and O is not allied with K (relation table, 0x2823c0);
- the player K exists.

Bounty B of T:
- Campaign (not skirmish, 0x28afb0 false): `Override_Bounty_SP` (type +0x2458). Skirmish: `Override_Bounty_MP`
  (+0x245C).
- The field is a **float** (parser kind 0x8). If it is < 0, B = the **maximum**, over every category bit in T's
  `CategoryMask` (+0x1648), of `Default_Bounty_By_Category_SP` (`_MP` in skirmish; 0x2c7ef0 / 0x2c7e40, lookup by
  category bit), floored at 0.
- An override of exactly 0 gives 0 (no category fallback).
- Nothing happens when B ≤ 0 or there is no current mode.

### 2.3 Galactic branch (current mode is not tactical, mode vf +0xF0 false) [H]

**Case A: the killer's faction has `Benefits_From_Corruption` (faction +0x10B; vanilla: Underworld only).**
1. Walk from the dying object up the parent chain (+0x2B0) to the first object that has the PLANET behaviour
   (+0x335 ≠ 0xFF): ship → fleet → planet, company → (transport / fleet) → planet.
2. If that planet has no Planetary data pack (+0xB8), nothing is paid.
3. If its corruption type (data pack +0x2F4) is not **3 (Racketeering)**, nothing is paid.
4. Otherwise: n = round(B) (0x22f9c0: half away from zero); Add_Credits(K, n, notify 0); player K stats +0x454 += n.
   Log "Awarding a %d credit bounty to player %S for destroying a %s."
5. With no planet in the chain, nothing is paid.

**Case B: otherwise, when the victim's faction has `Benefits_From_Corruption`** (the killer's faction does not):
- the same n is paid to K when K owns any object of type `NalHutta` or `NalHutta_Story` in the current store (0x407e00
  type by name, 0x2a87d0 instances, owner +0x58 = K);
- there is no planet or corruption test;
- this is the galactic form of the NalHutta bounty, a hard-coded full bounty with no 0.15 factor.

Other killers get nothing.

What is not there in galactic: no `Bounty_On_Faction_Ability` lookup, no `Target_Faction_Names`, no
`Bounty_Percentage_Amount`, no `Stacking_Category`, no `Apply_To_All_Allies`. Credits are paid per object, each rounded
separately, and an AI killer's amount is multiplied by its `Credit_Multiplier` inside Add_Credits. The bounty is paid
whether K won or lost the battle.

### 2.4 Tactical branch (mode vf +0xF0 true), for reference [H]

For each player p in index order that is allied with K (relation 0, which includes K itself [M]):
- A = the mode's per-player bounty object, vf +0x220(p id).
- f = 0x527260(A, victim faction name). It looks up the lower-cased faction name in A's map (+0x2E0). Within each
  stacking category it takes the **largest** value. It **sums** over the categories. Then it applies an optional
  minimum (+0x38, flag +0x40) and an optional maximum (+0x3C, flag +0x41).
- If f > 0: n = max(1, round(f × B)). Add_Credits(p, n). Stats +0x454 += n. Local player: a floating text at the object.

This answers the stacking question of `A2M_R2_UW_BOUNTY_TRACE.md`: max per category, sum across categories. It holds
for tactical only. The galactic branch has no percentage.

### 2.5 Starbases and ground structures [H code, M consequence]

The only call site of 0x392600 is the death handler 0x39bdb0, reached from object damage 0x3ab890 (and 0x630c20).

- **Space:** the galactic starbase is not an object. It is the planet's starbase type and level (planet data +0x140 /
  +0x148, 0x4bda40). The survival roll downgrades or removes it through 0x3f88b0 / 0x3fe750. These set the planet's
  +0x330 to the killer but kill nothing. So a starbase downgrade or loss pays **no bounty**.
- **Land, planet combatant destroyed:**
  - the planet's structure lists are cleared: 0x3f7440 → 0x4b7450 (list D+0x170, with the killer id) and, in land
    battles only, 0x3f7430 → 0x4b7350 (ground structures D+0x158, killer id −1);
  - both remove each object through 0x4bb4a0, which sets +0x330, raises signal 0x25 and destroys the object with
    0x391310;
  - 0x391310 does not reach 0x39bdb0, so these structures pay **no bounty**.
- **Structures that are combatants of their own:** these are the garrison / defence objects that the planet combatant
  adds (BATTLE_TRANSITIONS §4.2). They die through vf +0x120 like units and pay their own bounty [M].
- **Ownership change:** objects removed by a capture pay nothing. The UW trace matches this (REBEL_GROUND_MINING_FACILITY
  at 1808, barracks).

### 2.6 Check of the port on `claude/a2m-fix-underworld-r2` (84b47916, `ConflictManager::pay_auto_resolve_bounty`)

| port rule | retail | verdict |
|---|---|---|
| ability active while the side holds any planet whose corruption type c maps to an upgrade with `Bounty_On_Faction_Ability` | killer faction has `Benefits_From_Corruption` and the **dying object's own planet** (parent chain) is Racketeering-corrupted (type 3) | **wrong**: retail pays only for kills at a Racketeering planet. In HotM that is every Underworld-owned planet (they start with type 3, GALACTIC_ECONOMY §4.2) and every enemy planet the Underworld corrupted with Racketeering. A kill at a non-Racketeering planet pays nothing. |
| percentage from the ability, max per `Stacking_Category`, summed | no percentage in galactic | equivalent for vanilla (1.0); drop it for galactic |
| victim must be in `Target_Faction_Names` | any owner not K and not allied with K (an ownerless object too) | equivalent for vanilla HotM; use the relation test |
| one `add_credits(sum, notify 0)` per side per battle | one Add_Credits per dying object, each rounded half away from zero, notify 0, × `Credit_Multiplier` per call | change to per object |
| category default = first `Default_Bounty_By_Category_SP` entry (document order) the mask contains | the **maximum** over the mask's bits | **wrong** for multi-category types whose defaults differ |
| `Override_Bounty_SP` read as an int | float field (parser 0x8); < 0 means category default; 0 means none | port bug (lane follow-up): fix the schema type, not only bounty.cpp |
| holder (squadron / company) counts its members, holder 0 | every dying object pays its own type's bounty; the holder pays its own (0 in the traced battles) | keep members; give the holder its own bounty [M] |
| starbases and ground structures included | starbase: no object, no bounty; planet structure lists: no bounty | **wrong**: exclude them |
| the side's own objects excluded | allied objects excluded too | same in HotM |
| NalHutta bounty not modelled | case B (§2.3): the owner of NalHutta / NalHutta_Story gets the full bounty for every Underworld object it kills in galactic | missing rule; rare before 13500 in HotM |
| who is paid: the other battle side | the killer = the other side's first combatant's owner | same |

Both traced battles fit the retail rule only if the 7162 space battle was at a Racketeering-corrupted planet. 7497
fits: Shola was still Underworld-owned and so type 3. The fix lane should check the 7162 planet in the trace. If that
planet was not Racketeering, the parent-chain step of §2.3 needs a second look [H for the code as read].

Before the port rule changes, a check of the existing Q2 traces is enough: Underworld credit at auto-resolve battles
fought at a planet that is not Racketeering-corrupted should not rise. No new retail run is needed.

---

## 3. Q6 (probe H1): heroes and flagships in AI task-force selection

### 3.1 The hero-record test, read again (Hero Setup C 0x726710) [H]

For each candidate type T, with H = 0x6c00c0(T) (first company member, else first squadron member, else T):
1. H is a named hero (+0x13), a generic hero (+0x14), `Is_Special_Weapon_In_Space` (+0x1E) or has behaviour 0x37 →
   otherwise not a record.
2. Allowances (0x140b3a526 hero attach, 0x140b3a527 special weapon) as AI_GALACTIC_3 §2.1.
3. Unit u:
   - first the eligible free-store unit of type T (FreeStore setup list);
   - then a pool entry of type T, which overrides it (tactical only);
   - none in a campaign battle → not a record.
4. Script:
   - S = the **live object script of 0x6c0020(u)**, which is the unit itself, or its first fleet member, or the first
     object it carries: its +0x2D8;
   - if there is none (or no unit), S = **H's** type-level instance (+0x1AF8, only with `Execute_Script_On_Type`);
   - no S → not a record.
   The type tag `Lua_Script` is not consulted.
5. Only then: the Lua calls, and +0x54 = 1.

Hero Score 0x726f40 gives 0 for a non-record. For a record in a non-magic plan it gives 1e18 (veto), because the escort
accumulation is dead code (AI_GALACTIC_3 §2.4, unchanged).

### 3.2 Galactic hero objects lose their scripts [H data and engine, M combined]

- Every object whose type has `Lua_Script` gets its own script at construction, in galactic too. The attach puts the
  object on the serviced list (OBJECT_SCRIPTS §0, §1).
- Its first pump runs at the next service. On that pump the state machine's `State_Init` `OnEnter` runs.
- `ScriptExit()` frees the script at the end of that pump. The object service then drops +0x2D8 for good (OBJECT_SCRIPTS
  §2.2).

Shipped scripts that exit in galactic at their first pump (FoC `Data/Scripts/GameObject`; `Get_Game_Mode()` test in
`State_Init`):
- Home_One (`ObjectScript_PowerToShields`) and Sundered_Heart (`Sundered_Heart`): exit unless the mode is `Space`;
- Millennium_Falcon: exits unless `Space`;
- Darth Vader, Obi-Wan, Yoda, Han Solo, Palpatine, IG-88, Urai Fen, Silri: their state-machine scripts exit unless
  `Land` [M: for a file that requires both HeroPlanAttach and PGStateMachine, the later `main` wins].

Scripts that keep running in galactic, so their heroes stay hero records:
- `HeroService` loops with no mode exit: BobaFettPlan (Boba Fett, Tyber Zann), FleetCommander, FieldCommander,
  GenericBountyHunter, Tarkin, Veers, WedgePlan, LukePlan, HanSoloPlan, IonCannon, DeathStarPlan [M from the files];
- the `Execute_Script_On_Type` types (8 generic-hero types), whose type instance never exits.

Consequences for HotM:
- Home_One (named hero, Capital | SpaceHero | AntiFrigate, power 4000) and Sundered_Heart (Corvette | SpaceHero, power
  1000) are **not hero records** from their first galactic service on. HeroM scores them 0.
- FreeStore and Contrast price them like any other free unit. This is how retail's Conquer_To_Reconnect task force at
  about 6925 holds both (a2m r1 Rebel diagnosis, cause 4).
- ContrastToReconnect and ConquerPirate have no `DenyHeroAttach`, so hero attach is allowed. A hero that still has a
  live script (a Tyber Zann team) stays a record and is vetoed in these plans.

### 3.3 What the port must do

Port state: `HeroM::setup_c` (engine/ai/src/galactic_pp.cpp) sets `script` = "the unit's type or representative has a
`Lua_Script`".

That over-vetoes. Change it to:
- with a unit: the unit's (0x6c0020-resolved) object still has a live object script;
- without a live object script: H's type-level script (`Execute_Script_On_Type` on the **representative** H, not T).

The port does not host galactic object scripts. Choose one of these and record the choice as a `[port]` entry:
1. **Host them.** Attach galactic object scripts and run their pumps. This is the faithful option; it also draws one
   synced RNG value per attach, a tier-3 draw-order matter.
2. **Model only the alive flag.** Compute "script alive in galactic" per object as follows:
   - the object has a `Lua_Script`;
   - and it has not yet had its first galactic service, or its script does not exit in galactic.
   "Exits in galactic" is a per-script property. Derive it once by running the script's first pump in a sandbox Lua
   state with `Get_Game_Mode()` = "Galactic". Do not derive it from a hand-written name list.
   Objects created at frame 0 or 1 are records only until their first service, the next frame.

Expected effect: Home_One and Sundered_Heart (about 5000 power) become selectable. This removes HeroM as the
`last veto Hero` reason in the Rebel Conquer_Pirate / Conquer_To_Reconnect re-solves. Re-measure before anything else.

### 3.4 Why each ContrastToReconnect / Conquer_Pirate instance asks for production

Retail rule, no new finding:
- Both plans call `Produce_Force()` when not all selected units are on the free store. The thread then blocks until the
  production is done and returns, and the plan ends. The produced units go to the free store: the task is gone →
  signals 3 / 4, or 0x64ad00 re-admits the claimed unit when +0xD8 is clear.
- The next instance re-solves from scratch (Contrast against the target's current power).
- So an instance asks for production whenever the free units it may use do not cover the Contrast need of that
  moment.

In the port, three inputs push every instance into production:
1. the HeroM over-veto (§3.3), about 5000 power missing;
2. enemy power at Shola that grows too early: port-only Underworld starbase upgrades at Shola, through the
   GroundbaseLevel / Needs_Starbase_Upgrade input (other RE agent, D+0x130);
3. builds of ended instances: the port finishes them all, retail cancels the unreclaimed ones (§1.5).

Item 3 changes money, not power. Fix 1 and 2 first.

---

## 4. Corrections made to other docs in this commit

- **AI_GALACTIC_2.md** "Claiming the produced unit": the line "task gone → production finishes to the free store" holds
  only for a PAYG task taken by the reclaim, or a non-PAYG owner. Otherwise the ended task's production is cancelled
  with refund (§1.1). §3.4 Release: the reclaim and its consumer are [H] now.
- **GALACTIC_ECONOMY.md §2.6**: 0x403900 has an AI caller, 0x53ba60 (refund = true).
- **BATTLE_TRANSITIONS.md §3.5**: the line "no credit reward is paid by the battle code itself" is wrong for
  bounty. The galactic bounty rule is §2 here.
- **AI_GALACTIC_3.md §2.1 item 4 and probe H1**:
  - the script is the unit's **live** object script, and the type instance is H's;
  - named heroes whose scripts exit in galactic are plain units, so "heroes join plans only as plain units in
    DenyHeroAttach plans" was too strong;
  - H1's expected observation is refined: Vader's script exits in galactic, so Vader is a plain unit too.
- **OBJECT_RUNTIME.md**: byte +0x330 is the killer player id (last attacker's owner), written by the death handler
  0x39bdb0 and by the planet-structure removal 0x4bb4a0.

---

## 5. Retail runs

None are required. Optional checks on **existing** Q2 traces (`tools/probe_mod/q2_traces/out/...`, Underworld = player 6,
`production.q2q` + plan hooks):
- Q4: at 1583, 2959 and 4253, was the saboteur's plan ended by the goal system (released) or by its script? Was the plan
  at 1697 / 2844 / 3087 released with goal c + a ≥ the saboteur's remaining cost?
- Q5: the planet of the 7162 battle and its corruption type; credits at any later Underworld auto-resolve at a
  non-Racketeering planet.
- Q6: HOME_ONE / SUNDERED_HEART in AI task forces after frame 1 (seen at about 6925).

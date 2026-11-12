# a2m round 2, RE answers A: planet ground-base level, goal-pass cadence, dropped Build goals

Clean-room RE answers for the a2m round-2 Underworld and Rebel fix lanes (2026-11-12). The decompiled output was read for
this note; it holds behaviour only, no code. Tags: [H] read in the code, [M] inferred, [L] guess. Data facts come from the
shipped FoC XML (`extracted/foc_data_text.zip`). Questions are the ones in the r2 lane requests
(`scratchpad/plans/a2m_r2_uw_re_request.md` Q2 / Q3, `docs/re/A2M_R2_REBEL_NABOO_TRACE.md` §5 items 1-3 on
`claude/a2m-fix-rebel-r2`). The plan-end production cancel (UW request Q1) is answered by the other RE note of this round.

Short answers:

| # | Question | Answer |
|---|---|---|
| 1 | Who writes planet D+0x130 (GroundbaseLevel)? | One recount, 0x4bdaa0: the number of **live, finished** ground structures (DUMMY_GROUND_STRUCTURE) in the planet's ground-structure list, clamped to 0..5. Queued structures never count. HotM start: Felucia 1, NalHutta 3, Shola 1. This is what the port already computes (apart from the clamp), so GroundbaseLevel is **not** why retail never queues the early Felucia barracks. |
| 1b | Then what keeps the barracks (and the early starbase upgrades) out? | The accept-time ETA test. For a structure the ETA includes the **queue wait at the target**: retail marks the target's queue-pool entry used, so the ETA is the planet's queue time + build time. The port adds the build time to that entry but never marks it used, so its ETA is the build time alone and the barracks passes `ETA <= AllowedTime` (17.28 s). [H rule, M that it is the whole story] |
| 2 | a2m t11 cadence | evalsLastPass = the number of `Evaluate` calls in the pass. Init seeds it with sources x targets and the seed is **not cleared**, so passes 1 **and 2** run at the cap. The cap uses **every player in the PlayerList** (one per faction: 8 in FoC) minus active humans: HotM 8 - 1 = 7 -> cap 3, not the port's 4. A pass with E evaluations at quota q takes floor(E/q) + 1 frames. |
| 3 | Does a maintenance drop leave a cooldown? | No. Maintenance records no activation or goal outcome. Build_Underworld_Barracks / Upgrade_Starbase have no learning tags (defaults 0), so even the outcome records that exist would change nothing. Nothing stops the next pass from proposing it again at full desire; retail does not because the accept test (1b) rejects it first. |

---

## 1. Planet ground-base level, D+0x130

Notation: D = the planet object's data pack (`PlanetaryDataPackClass`, planet object +0xB8). GC = GameConstants.

### 1.1 The writer: recount 0x4bdaa0 [H]

D+0x130 has one writer in the game logic, 0x4bdaa0 (construction and save / load not checked [M]). It:

1. Counts the entries of D's ground-structure list (DynamicVector at D+0x150: data +0x158, count +0x160) that are non-null
   and whose object flags +0x3A0 have neither 0x02 (dead / removed) nor 0x40 set.
2. Count 0: D+0x130 = 0, ground-base owner D+0x124 = -1 (8-byte write with D+0x120), base type D+0x128 = null. Returns 0.
3. Count > 0: D+0x130 = min(count, 5). Then:
   - if the planet's ground owner D+0x74 is -1: D+0x124 = -1, D+0x128 = null, and the persistent tactical-built-object
     list (D+0x08) and the persistent upgrade-object list (D+0x20) are emptied;
   - else: D+0x128 = the faction's DUMMY_GROUND_BASE type of that level (0x407b60(level, owner's faction, 0x5B)),
     D+0x124 = D+0x74; persistent tactical-built entries of another owner, or whose stored level is above the new level,
     are removed; persistent upgrade entries of another owner are removed.

So **the ground-base level is the number of live ground structures, capped at 5**. There is no per-type weighting (a
palace, a barracks and a factory each count 1), no Base_Level field and no max over types.

### 1.2 When it is recounted [H]

| Caller | When |
|---|---|
| 0x4b6db0 (add) | a DUMMY_GROUND_STRUCTURE (behaviour 0x5F) object is created at the planet by 0x400b40, the "create type at planet" routine (A2K_T07_STORY_LOAD_RE §2): finished production items (GALACTIC_ECONOMY §2.5), `Starting_Forces` (0x3fae10), story / Lua spawns. 0x400b40 sets D+0x74 = owner first when it is -1. Its "skip when D+0x130 > 5" guard can never fire, because the recount caps the value at 5 |
| 0x4bb420 (remove one) | object removal 0x391310 and four other removal sites |
| 0x4bb1a0 (trim to a level L) | removes the structures whose type's required ground-base level (+0xF0C) is above L, then removes from the list's end while the level is above L (callers 0x2aa510, 0x3faa10, 0x5b2910, 0x6cb930, 0x6ef860; not identified) |
| 0x4b7350 (remove all) | callers 0x3f7430, 0x3f7280, 0x3f76c0, 0x3f3340, 0x3f98a0, 0x2bff60 (planet owner re-derivation and conquest paths [M]) |
| 0x4b5fb0 | the data pack's destructor |

Consequences:
- **A structure in the production queue does not count.** It joins the list only when the finished item is created by
  0x400b40. Nothing in the queue code writes D+0x130. [H]
- A structure that dies keeps counting only until its removal (0x391310 -> 0x4bb420); the flag test in step 1 makes the
  recount ignore it from then on. [H]
- Orbital structures (0x60) and starbases (0x5C) go to other fields (D+0x168 list, D+0x148 level); they never change D+0x130.

### 1.3 Values at Heart of the Maw start [H data, H rule]

`Sandbox_Heart_Of_The_Maw_Empire` (CAMPAIGNS_UNDERWORLD_GC.XML) gives the Underworld these ground structures; every one of
them has `<Behavior>DUMMY_GROUND_STRUCTURE</Behavior>` (SPECIALSTRUCTURES_UNDERWORLD.XML):

| Planet | Starting ground structures | D+0x130 | GroundbaseLevel (normalised, /GC+0xF8 = 5) |
|---|---|---|---|
| Felucia | U_Ground_Barracks | 1 | 0.2 |
| NalHutta | U_Ground_Palace, U_Ground_Barracks, U_Ground_Droid_Works | 3 | 0.6 |
| Shola | U_Ground_Vehicle_Factory | 1 | 0.2 |

A palace queued at Felucia leaves Felucia at 1 until the palace is finished; then it is 2.

MaxGroundbaseLevel (0x372400 -> 0x4b7eb0) = min(5, D+0x230) where D+0x230 is the planet's ground structure slot count
(`Special_Structures_Land`, 5 / 6 / 8 on these three planets), so it is 5 (normalised 1.0) on all three [M: D+0x230 = that tag]. MaxStarbaseLevel
= D+0x238 (`Max_Space_Base` 5 [M]) -> 1.0. StarbaseLevel at start: Felucia 2, NalHutta 1, Shola 1 (0.4 / 0.2 / 0.2).

### 1.4 How the named functions read it [H data]

All five are XML perception equations (AI_EQUATIONS_UNDERWORLD_GALACTIC.XML, INFRASTRUCTUREGALACTICEQUATIONS.XML); the
ground-base level enters only through these terms:

- **Needs_Groundbase_Upgrade** (target): `(GroundbaseLevel > 0) * (MaxGroundbaseLevel != GroundbaseLevel) * (4 Chokepoint
  + 2 BaseIncomeNBTP + (MaxGroundbaseLevel - GroundbaseLevel) + 4 Is_Connected_To_Enemy) / 7`. At Felucia (0.2 vs 1.0) it is
  > 0. The port's 34.92 barracks desire implies NGU = 0.88 there, which fits this equation.
- **Should_Underworld_Save_For_Desirable_Structures** (target): `(GroundBaseLevelUnnormalized > 1) * ((Should_Build_
  Underworld_Vehicle_Factory > 15) + (Should_Build_Underworld_Palace > 15))`. At Felucia D+0x130 = 1, so this is **0** at
  HotM start and stays 0 until a second Felucia structure is finished.
- **Should_Build_Underworld_Barracks** (target): `18 * (1 - clamp(SelfBarracks / PlanetsControlled, 0, 0.5)) * (NGU +
  TargetBarracks + 2 * (3 > SelfBarracks)) * (2 > TargetBarracks) * (Save_For_Desirable == 0) * (Should_Save_For_Corruption
  == 0)`, every StructureCount complete-only. At Felucia: SelfBarracks 2 (Felucia, NalHutta), planets 4 -> 0.5; TargetBarracks
  1 -> (2 > 1) = 1; so desire = 9 * (NGU + 3). `Should_Save_For_Corruption` = (HasUnit Underworld_Saboteur == 0) * (0.67 >
  PlanetsCorrupted) * (complete palaces > 1) = 0 while only NalHutta has a finished palace. **Retail therefore scores the
  Felucia barracks above zero at frame ~119 too**: the equations give no veto there.
- **Needs_Starbase_Upgrade** (target): `(StarbaseLevel > 0) * (MaxStarbaseLevel != StarbaseLevel) * ((GroundbaseLevel >
  StarbaseLevel) + (MaxStarbaseLevel >= MaxGroundbaseLevel)) * (...)`. The middle factor is a **sum**: NalHutta 1 + 1 = 2,
  Shola 0 + 1 = 1, Felucia 0 + 1 = 1. Because MaxStarbaseLevel 1.0 >= MaxGroundbaseLevel 1.0 on all three, GroundbaseLevel
  can only double the score, never zero it.
- **Should_Underworld_Build_Starbase** = Needs_Starbase_Upgrade * (Should_Save_For_Corruption == 0): the same.

So no rule over D+0x130 explains "no barracks before 9000" or "no NalHutta / Shola starbase upgrade before 11059 / 13500".
After the Felucia palace is finished, Felucia's level is 2 and `Save_For_Desirable` can veto the barracks whenever the
Felucia vehicle-factory desire is above 15 [M]; that part already follows from the port's live-structure count.

### 1.5 What keeps the structures out in retail: the structure ETA at accept [H rule; M as the cause]

Accept (galactic slot 10, 0x6b8660, AI_GALACTIC §1.5) rejects a candidate when `0 < AllowedTime < goal ETA (+0x7C)`. The
ETA is the PotentialPlan ETA from 0x6639f0 (AI_GALACTIC_2 §2.1), set at the first solve (Find_Plan_For_Goal -> Init
0x6c0d00). Resolve re-solves (0x6c4a50) and applies the same test (AI_GALACTIC §1.6 step 5).

In 0x6639f0, a selected **structure** (behaviour 0x5C / 0x5B / 0x5F / 0x60) is handled like this [H]:
1. Its source is the target planet.
2. structureSum += btime(T, **list[0]**), where list = the target reach object's planet list for the goal level
   (0x665160). For a target the player holds, the flood starts at the target, so list[0] **is** the target
   (A2K_T08_RE_ANSWERS §Q3.3 step 3).
3. If list[0] is the target: the pool entry 0 of the type's queue pool (pool A = ground queue for 0x5F types with a cost,
   pool B = space queue for starbases) is **marked used** and its queue time grows by the same build time.
4. ETA = max(largest queue time over **used** entries of both pools, structureSum).

The pool entry's start value is the planet's queue time: on the first solve (the byte 0x140b3a524 = 1, Init at proposal)
0x401ba0(planet production, planet, queue) = the current item's remaining time plus the build times of the rest of that
production queue; on the re-solves (flag 0) 0x53bb20(AIPlanetBuildQueue, 1, 0) (AI_GALACTIC_2 §2.3). [H; the queue index
argument of 0x401ba0 is not visible in the decompile, M that it is the pool index]

So **a structure's ETA is the target's queue wait plus its build time**. Felucia's barracks: AllowedTime = 18 s x 0.8
(Galactic_Build_Time_Multiplier, Normal) x 1.2 (Build_Time_Delay_Tolerance) = 17.28 s; btime = 14.4 s; the goal is
accepted only when Felucia's ground queue holds less than 2.88 s of work. With the palace in production there (25 s x 0.8),
the barracks is rejected at slot 10 on every pass [M: assumes the palace or another item occupies Felucia's ground queue,
as the r2 trace says]. When the palace is done, Felucia's level is 2 and the Save_For_Desirable veto of §1.4 can take over.
Starbase upgrades follow the same rule with the space queue (pool B): a starbase upgrade at NalHutta or Shola is accepted
only when that planet's space queue holds less than 0.2 x its build time (Upgrade_Starbase tolerance 1.2) [M as the cause
of the missing early upgrades].

The port (`galactic_pp.cpp` `recompute_eta`, structure branch) adds the build time to `pool[0].time` but **does not set
`pool[0].used`**, so its structure ETA is structureSum alone; that matches the r2 trace (accepted at 119, dropped at the
next pass by maintenance with ETA 68 > 17.28, because maintenance's 0x525200 does count the queue wait).

### 1.6 What the port must do

1. `recompute_eta`, structure branch: when `list[0] == target`, set `pool[0].used = true` as well as adding the build time;
   take the build time at `list[0]`'s planet (retail calls btime with list[0], which is the target whenever the branch
   applies). Use the first-solve queue source (0x401ba0: the planet's production queue only) at the proposal solve and
   0x53bb20 at re-solves (A2M_T04_RE_ANSWERS Q2 already asks for the flag). This is a tier-1 rule fix; it changes goldens.
2. `GalacticSim::ground_base_level`: clamp to 5. Otherwise it already follows §1.1 (live 0x5F structures, finished items only).
   Do **not** add queued structures (the r2 experiment "GBL + 1 while in production" is not the rule).
3. Record the [port] note in `docs/PHASE4.md` that the D+0x130 rule is now [H] (the comment in `campaign.cpp` says [M]).

---

## 2. Goal Service 0x6cacd0 cadence (a2m t11)

### 2.1 The counters [H]

Goal-system fields: +0x178 quota, +0x17C source cursor, +0x180 target cursor, +0x184 pass-complete flag, +0x188
evalsLastPass, +0x18C countdown.

**Proposal slice 0x6cb120.** It loops while a slice counter is below the quota. Per iteration:
- type not proposable (0x6ca110), or the target cursor past the end of the target list: next source, **not counted**;
- targeted type whose target does not `Apply`: next target, not counted;
- slot 9 (0x6b84f0) says skip (goal already active, or the target's reach object returns no planet for the goal level):
  not counted;
- otherwise the candidate is **evaluated** (`Evaluate` is called): +0x188 += 1 and the slice counter += 1, whatever the
  result (Evaluate false, desire <= 0, slot 10 rejecting, or accepted).
- When the source cursor passes the last source, the cursors reset, +0x184 = 1 and the slice returns.

So **evalsLastPass = the number of Evaluate calls in the pass**; the quota counts exactly the same events. A global type
(flag 0x10) is one candidate.

**Service 0x6cacd0.** If the countdown > 0: decrement, return. Else run a slice; if +0x184 is set: budget weights
(0x614590), Commit (0x6c8710), clear +0x184, then

`quota = max(1, min( (int)ceilf(E / (FPS x 5.0)), (int)(20.0 / max(1, N - H) + 0.5) ))`, E = +0x188, then +0x188 = 0 and
countdown = slot 11 (galactic 0x6b8630: FPS x `Galactic_AI_Goal_Cycle_Sleep_Duration`, 0.0 on every shipped difficulty).

The cap term is computed in double (20.0 / (double)(float)x + 0.5, truncated). FPS = 30.

**The player count** [H]: N = the size of the PlayerList vector (0x140a16fd0); H = 0x294b80 = players with `Is_Human`
(+0x62) and active (+0x180, cleared only by `PlayerListClass::Deactivate` 0x2940c0). The PlayerList is filled by
`Create_Default_Players` 0x293390: the required (session) players first, then **one player for every faction in the faction
table that has no player yet**, in faction order. Players are never removed (deactivation only clears +0x180). FoC has 8
factions (FACTIONS.XML: Rebel, Empire, Pirates, Neutral, Hostile, Sarlacc; EXPANSION_FACTIONS.XML: Underworld, Hutts), so a
FoC galactic game has **N = 8**: HotM with a human Empire gives N - H = 7 -> cap **3**; with no human, 8 -> cap 3 as well
(20 / 8 + 0.5 = 3.0).

**The first pass** [H]: goal-system Init (slot 6, 0x6ca1f0) sets
- +0x188 = S x max(1, T), S = the number of (goal type, function) sources of the player's AIPlayerType for the mode
  (0x60f6c0, the same list the slice walks), T = the size of the perception target list (unfiltered);
- quota = max(1, min(ceil(+0x188 / 150), cap));
- cursors, flag and countdown = 0, **but not +0x188**.

The slices of pass 1 add their evaluations to the seed, so at the end of pass 1 the quota is computed from
seed + E1, not E1. With HotM's S x T in the thousands, **passes 1 and 2 both run at the cap (3)**; from pass 3 on the quota
is ceil(E_prev / 150) capped at 3.

**Frames per pass** [H]: a slice ends as soon as its counter reaches the quota, even if only non-counted candidates remain;
the end of the source list is found on a later call. A pass with E evaluations at quota q therefore takes
**floor(E / q) + 1** service frames, and its Commit runs in the last of them (the next pass starts the frame after).

### 2.2 Expected cadence and the port

| | Retail rule | Port today (`ai_goals.cpp`) |
|---|---|---|
| Players in the cap | all PlayerList entries (FoC: 8) minus active humans | session players (`quota_players`: HotM 6) minus humans |
| Cap, HotM | 3 | 4 |
| Pass 1 quota | min(ceil(S x max(1,T) / 150), cap) | cap |
| Pass 2 quota | min(ceil((seed + E1) / 150), cap) = cap | ceil(E1 / 150) = 2 |
| Pass n >= 3 quota | min(ceil(E_{n-1} / 150), cap) | same formula |
| Frames per pass | floor(E / q) + 1 | ceil(E / q) (one frame less when q divides E) |

With E about 150..155 per pass: retail ends passes near 51, 103 (both quota 3, matching retail's Underworld plan records
at 52 and 103), then one pass of E + 1 frames if E2 <= 150 (quota 1) or about 78 frames if E2 > 150 (quota 2). The retail
records 103 -> 250 (147 frames) fit a pass at quota 1 with E about 146, which needs E2 <= 150. The later record gaps
(156..165) cannot be single quota-1 passes (that needs E > 150, which makes the next pass quota 2); they fit two quota-2
passes of about 78..83 frames with a plan record on every other pass [M]. So retail's E straddles 150 in HotM, and the
port's E of 151..158 (PHASE4 a2m t11 note) is a few candidates too high early on; where the extra evaluations come from
(target list, `Applies`, slot 9) is a port-side comparison, not an RE question: the rules above fix the counting.

What the port must do (tier 2, parity of timing):
1. `quota_players`: N = number of factions in the faction table (the PlayerList holds one player per faction), H = human
   players; do not count only the session's players.
2. Init: seed `evals_last_` with sources x max(1, target count) and compute the first quota from it; keep the seed so pass
   1's evaluations add to it; reset at the end of each pass as now.
3. End a slice when `used` reaches the quota (do not scan on for the end of the list in the same frame).

---

## 3. Build_* goal dropped by maintenance: no cooldown [H]

Maintenance 0x722a30 (run by Resolve 0x6c8970 three times: on the active set when no proposal survives, on the kept set
after the classification, and on the active set when the filtered proposals are empty) drops a goal by
1. 0x7225d0: removing it from the per-type goal index (and clearing the indexed object's +0x88);
2. 0x614880: removing its budget entry (galactic mode) and returning the entry's two amounts to the category's available
   credits, then, for a pay-as-you-go player whose goal has a plan, taking the plan's reclaim (0x5f7d40) back out of the
   category (the PAYG side is the other r2 RE note's subject);
3. erasing it from the active vector (its reference is released).

It calls neither learning recorder:
- activation outcomes (0x58a5d0, read by the proposal as c2 x Per_Activation_Failure_Desire_Adjust) are recorded only
  by Resolve's greedy and random phases (flag 0 on a failed re-solve, failed funding or `0 < AllowedTime < ETA`; flag 1 on
  a new activation);
- goal outcomes (0x58a720, c1 x Per_Failure_Desire_Adjust) are recorded only when a plan ends (0x5f6a50), with the plan's
  result byte (+0xA8).

When the dropped goal's last reference goes, its destructor 0x691840 ends its running plan through 0x5f6a50, which
finishes the plan's build tasks (0x64ad00), records one goal outcome with the plan's result and releases the goal [H for
the destructor; M that maintenance releases the last reference].

Even those records change nothing for the infrastructure goals: the AIGoalType constructor (0x6b7c70) defaults
`Tracking_Duration`, `Per_Failure_Desire_Adjust`, `Activation_Tracking_Duration` and `Per_Activation_Failure_Desire_Adjust`
to 0, and Build_Underworld_Barracks, Build_Underworld_Palace and Upgrade_Starbase set none of them. 0x58a5d0 records
nothing at all when `Activation_Tracking_Duration` is 0 (a negative value records an entry with no expiry, -1 [M]). Of the
galactic goals only Corrupt_Planet, Sabotage_Planet, the Conquer_* goals, Weaken_Planet, Raid, the Unrestricted_Grab goals
and Build_Space_Forces carry learning tags.

So after a maintenance drop the next pass proposes the same (type, target) again at full desire (slot 9 no longer finds it
in the active set). The port needs no cooldown; the fix is §1.6 item 1, which stops the barracks from being accepted.

---

## 4. Corrections made to existing docs

- `AI_GALACTIC.md` §1.3: the first-pass seed, the player count of the cap and the frames per pass (§2 here). §1.6:
  maintenance's elapsed term is added only when 0x525200 > 0 (already in A2F_RE_READS §1.4 item 1); maintenance also runs on
  the kept set; it records no learning outcome; an activation failure is recorded only for a type with
  `Activation_Tracking_Duration` != 0.
- `AI_GALACTIC_2.md` §2.1: the structure branch marks the target's pool entry used (so the ETA includes the queue wait) and
  takes the build time at list[0]; the initial queue time is 0x401ba0 on the first solve, 0x53bb20 on re-solves.
- `AI_GALACTIC_TOKENS.md`: GroundbaseLevel row names its writer.
- `HOTSPOTS.md` §1: the cap is `20 / max(1, players - active humans)`, not `20 / num_players`.
- `A2L_T03_RE_ANSWERS.md` Q2.1 step 1: a goal already in the active set is **skipped** by slot 9 (it said "not blocked").

## 5. Open items

- Which extra candidates make the port's per-pass evaluation count 151..158 where retail's is about 146..153 (port-side
  comparison of the target list, `Applies` and slot 9 against AI_GALACTIC §1.5 and A2L_T03 Q2; no retail run needed).
- Whether the palace (or another item) really occupies Felucia's ground queue from about frame 100 to the end of the
  barracks window in retail: the existing Q2 trace kit (`tools/probe_mod/q2_traces`, production queues per planet) already
  records it; check the reference runs before relying on §1.5 as the whole cause.

# A2k t02: D4 (Underworld -> Kashyyyk markup and accept), port trace and RE questions (2026-11-06)

Lane B of a2k. Port side: `alamo_headless --economy --galactic-ai --story --campaign Sandbox_Heart_Of_The_Maw_Rebel
--human Rebel --seed 1 --workers 0`, traced with temporary stderr prints (not committed). Retail side: run
20261106-074145 (`A2J_T02_T04_PROBE_RESULTS.md` section 1, `perception.tsv`). Tags as elsewhere: [H] read from a run, [M]
inferred, [L] guess. Player rows: Rebel p0 (human), Empire p1, Underworld p5.

## 1. Where the port's third markup came from [H port]

PathBonusForDistantTargets.lua walks `FindPlanet.Get_All_Planets()` (one planet per `Sleep(1)`, 30 frames apart; the
Underworld's walk starts at frame 42) and calls `Apply_Markup` on the planets of `Find_Path(_FindStageArea(planet),
planet)`, with value (GenericPlanetValue + Is_Neglected_By_My_Opponent_Space) / 2 and one handle per target planet.
The walk order is the reverse of the campaign's `<Locations>` list (Yavin, The_Maw, Tatooine, Shola, NalHutta,
MonCalimari, Kashyyyk, Kuat, Kamino, Naboo, Felucia, Corellia).

| frame | target | port stage before (fewest hops) | path | Kashyyyk markup |
|---|---|---|---|---|
| 222 | Kashyyyk | Felucia | Felucia, Kashyyyk | +0.2933 |
| 252 | Kuat | Felucia | Felucia, Kashyyyk, Kuat | +0.3244 (0.6178) |
| 372 | Corellia | **Felucia** | Felucia, Kashyyyk, Kuat, Corellia | **+0.3944 (1.0122)** |

The port's `_FindStageArea` was `nearest_owned`: the Underworld planet with the fewest graph hops to the target, lowest
planet row on ties. For Corellia all four Underworld planets are 3 hops away (Felucia, NalHutta and The_Maw through
Kashyyyk / Kuat, Shola through Naboo / Tatooine), so Felucia (lowest row) won. Felucia's only link is Felucia-Kashyyyk,
so Find_Path had to pass Kashyyyk. Find_Path itself (the AI all-pairs Get_Path) and the walk order were not the cause.

## 2. What retail shows [H]

`perception.tsv`, UNDERWORLD>Kashyyyk `MK`: 0.2933 at 240, 0.6178 from 270, and **1.0122 at exactly one sample, frame
22800**, back to 0.6178 at 23100. 1.0122 - 0.6178 = 0.3944 is Corellia's value. So retail's Corellia handle does put
Kashyyyk on its path, but only for one walk (about 360 frames). Its stage or path therefore follows something that
changes during the game, such as the AI node costs (enemy strength), not the static hop count. In the port at 372 the
Underworld's all-pairs weights are Shola -> Corellia (Naboo, Tatooine) 28706018 and Felucia -> Corellia (Kashyyyk,
Kuat) 32113304, so the cheaper start is Shola.

## 3. Port change (a2k t02) [M]

`_FindStageArea` for a planet target is now the player's own planet with the lowest all-pairs weight W[stage][target]
(`cheapest_stage`, ai_path_finder.hpp: unsigned compare, first row on ties, unreachable pairs skipped). Without a
created finder it falls back to the old fewest-hops planet. With it the port's sequence is 0.2933 at 222 and 0.6178 at
252, and at 372 the Corellia path goes Shola, Naboo, Tatooine, Corellia, so there is no third markup.

The port still adds the Corellia markup later, at 735. By then its Kashyyyk / Kuat node costs have fallen from 75 / 225
to 50 / 150, after a port-only Underworld fleet move to Kashyyyk at 219 and a battle at 287 (retail has no Kashyyyk
battle in this window). That is a trajectory gap, not this mechanism.

## 4. The accept decision at desire 0.311 [H port]

With the change, `Conquer_Pirate @Kashyyyk` has a desire of 0.311 from 252, as in retail. The port still attacks:
- The first solve is at proposal, frame 323 (`CONQUERPIRATEPLAN`, MainForce 13 free units, gross 19310, net 0, eta 0).
- In the resolve at 410 it is the **only** Offensive proposal. Credits 112.7 > 0, fundable, time OK, so the greedy phase
  accepts it. The plan starts at 410. Kashyyyk goes to the Underworld at 878, and the Empire
  takes it back at 1107.
- Contrast: tolerance 1 - 1.1 / 1.15 = 0.043. The deciding row is the space Capital row, target 6325 (= 5500 x 1.15).
  It is met by **one Kedalbe_Battleship** (from The_Maw) with 125 to spare. A row fails when more than 0.043 x 6325 =
  272 is left, so a 6.3 % higher enemy Capital strength in retail (or a 6.3 % lower Kedalbe value, or the Kedalbe not
  being eligible) makes this solve fail. The goal would then not be accepted, or it would record an activation failure
  (-5 desire for 60 s, `Per_Activation_Failure_Desire_Adjust`), which keeps a 0.311 desire below 0.
- Other rows: space total 5970 (left -4330), ground total 1062 (-74), ground rows 318.8 / 645.2 / 379.5, all met.

So whether retail attacks depends on the PotentialPlan solve, not on the sign of the desire. That matches the
"accept side" reading of A2J_T02_T04 section 1 [M].

## 5. RE questions (binary only; do not answer from the port)

**Q1. `_FindStageArea` (FindStageAreaClass::Lua_Call 0x6990f0) for a planet target in galactic.** The direct callees
are 0x3f8500, 0x6021a0, 0x4b06d0, 0x256d40, 0x5791e0, 0x5751d0, 0x60e0a0 and 0x248190, so the stage choice is probably a
virtual call (the target's PlanetReachabilityClass 0x663530, as in FreeStore Setup A 0x7287a0, or the task force).
- (a) Which function returns the stage?
- (b) What are the candidates: the player's own planets, allies, or the reachability source list 0x665160 at which
  level? Which level / link mask does an IgnoreTarget plan's `EmptyForce` (Distant_System_Path_Bonus) pass?
- (c) What is the key: the AI all-pairs weight W[s][t], hops, 2-D distance, or flood order? What is the tie-break?
- (d) When does it return nil?
- Check: retail's Corellia handle includes Kashyyyk only at frame ~22800 (section 2).

**Q2. Capital-row inputs for `ConquerPiratePlan @Kashyyyk` (Underworld, Medium, frames 300..420).**
- (a) The enemy strength Setup A 0x6c5de0 sums for the space rows at Kashyyyk (Empire_Star_Base_1 + Acclamator, plus
  any starting squadrons): is the starbase's own AI_Combat_Power or its spawned squadrons counted (A2 t03 spawn lists),
  and is the value the perceived (last-seen) one?
- (b) The Kedalbe_Battleship's power P (0x72b940 `power_eff`) and contrast weight w for that row. The port gets target
  6325 (= 5500 x 1.15) and a Kedalbe that fills it with 125 left.
- (c) Is a free-store unit at The_Maw eligible in Setup A for a Kashyyyk target (the engaged / fleet-pending tests,
  +0x361 / +0x369)?

**Q3 (probe, if Q2 does not settle it).** One retail sample of the Underworld's `EnemyForce` space totals by type for
Kashyyyk at 300..420 (typed tokens already in the a2f kit's DT channel: HSF only gives "has"). Safe only with the
tokens shipped equations already evaluate (A2J_T02_PROBE_PLAN section 3).

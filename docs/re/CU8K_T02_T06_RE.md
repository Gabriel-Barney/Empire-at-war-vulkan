# cu8k t02 / t06: adopt sites, stop-and-hold callers, skirmish start formations (RE, 2026-11-09)

Read from the retail FoC x64 exe (v1.121) and its decompile, plus the cu8k t04 retail trace
`tools/probe_mod/cu8h_probes/out/20261109-001746-trace-cu8k-endor-land/trace.log` (CU8K_T04_COORDINATOR_TRACES item 2)
and the two jetpack traces of the same day (`out/20261109-061804-jetpack/`, `out/20261109-062916-jetpack/`).
Tags: [H] read in code (or in the trace), [M] inferred with good confidence, [L] guess. No decompiled code is
reproduced. Terms as in MOVEMENT §4.6 / §4.7: "adopt" = `0x39bcb0` (MOVEMENT §4.7.1 predicate, then a new coordinator
and `Issue_Degenerate_Move`: one FormationClass ctor draw `int(1, FPS)` at once); "composition" = the land
composition `0x43e5b0` at the coordinator update `0x426eb0` (common tick step 10: one ctor draw per new formation);
"stop and hold" = `0x3a9880`.

## 1. t06: skirmish start (land and space)

### 1.1 The adopt returning at 0x395421 [H]
0x395421 is inside the **GOM post-load finish `0x3952e0`**, called from the Load_Map tail `0x2b4440` (return 0x2b44a7)
on the load thread. It is the MOVEMENT §4.7.3 "Load_Map tail" pass (every object in GOM order through the full
predicate), not the starting forces. On `_MP_LAND_ENDOR` it adopts one object, the Is_Neutral map `Land_Speeder`
(cu8i), giving the single degenerate draw at gf 0 right after the environment pick (trace n 3362).

### 1.2 Why the starting team containers show no adopt draw [H]
- The starting forces **are** on the `0x2a4fc0` → company placer `0x29e960` → team helper `0x2a5a60` path: the trace
  has 425 SKIRMISH_FORCES creation draws at gf 0 with `2a5659` (return into `0x2a4fc0`) in the stack, 57 of them
  through the helper's container creation (`29ecb5` ← `2a5da3`).
- The helper does call the adopt for each container, but it does nothing: a land team container's locomotor is
  `LandTeamContainer` (vtable 0x8a3630), whose behaviour init (vf9, `0x5abac0`, run by the behaviour attach
  `0x38cb30` during `0x29f810` unless the object is being loaded) **registers the container itself**: it makes a new
  coordinator (`0x426010`), adds the container (vf3: sets the coordinator's "recompose" flag +0xa8 := 1) and gives it a
  `Move_To` its own position (`0x4f8200`). Move_To makes no formation. So by the time the helper adopts, the container
  is already in a coordinator and check 5 of the predicate (`0x426140`) fails: no draw.
- The formation (and its ctor draw) comes at the **next coordinator update**, step 10 of the next tick, as one land
  composition per container: the `43e712` draws (← `4f80a8` ← `426f28` ← `363c83`).
- Nothing else sets +0xa8 for them; the predicate's check 1 is not the cause (the coordinator system exists during the
  starting-forces creation).

### 1.3 What the compositions are [H trace, M attribution]
CU8K_T04's "18" counts every hit with the composition stack up to mode frame 120, not only gf 1. Grouped by frame:

| gf | compositions | what |
|---|---|---|
| 1 | 12 | 10 starting team containers (all players), then the 2 containers the indigenous spawner made during gf 1's object services (trace n 4067-4074, `5320d4` = SpawnIndigenousUnits) [M: the 10 / 2 split] |
| 22, 25, 29 | 2 each | the indigenous spawner's later spawns (each preceded by its creation draws through `5320d4`) [M: 2 containers per Endor spawn] |

Each is one `int(1, 30)` (FPS 30; every one stepped the LCG once in this run). No r' (no vanilla container type has
`PathFrameDelay`). Units of companies without `Create_Team` (vehicles, heroes outside a team) get no coordinator from
creation and draw nothing until their first order.

### 1.4 Rule for the port (land skirmish start)
1. Load (load thread, after the environment pick): the Load_Map-tail pass, one degenerate `int(1, FPS)` per object
   that passes the predicate (Endor: the neutral Land_Speeder; done in cu8i).
2. Post-load starting forces: the creation draws only. **No formation draw at a team container's creation.**
3. Tick 1 (gf 1), common tick step 10 (the coordinator update), after that tick's object services: **one
   `int(1, FPS)` per land team container that still sits in the coordinator it made for itself**, in coordinator
   creation order: the starting containers in the order the starting forces created them (players as `0x2a4fc0`
   walks them, companies in list order, chunks of `Max_Squad_Size` in order), then the containers created during tick
   1 (indigenous spawns) [M: the order is the coordinator list order].
4. General form (any land battle): every land team container gets its own coordinator when it is created and
   composes at the next step 10, unless an order moves it into another coordinator first; a coordinator emptied
   before the update composes nothing.

### 1.5 Space skirmish start [M]
- Squadron containers use `Team` (`TEAM_LOCOMOTOR`, `TeamLocomotor` vtable 0x8aff88), which does **not** register
  itself: the helper's adopt passes and draws **at once**, one degenerate draw per container (return 0x2a61d5), right
  after that container's creation draws, before the next chunk (MOVEMENT §4.7.3 helper bullet, unchanged).
- Ships whose locomotor is `SimpleSpace` (vtable 0x8aeaf8) register themselves like land containers (init `0x625090`:
  own coordinator, Move_To own position), so they draw nothing at creation and compose (space composition `0x5d58f0`)
  at the next step 10. Fighters inside a squadron are team members and never draw.
- Not traced: the 2026-11-09 Kuat run was armed late (CU8K_T04 item 3). The one-line trace check: arm before Start
  Game on a space skirmish and expect `39bda0,2a61d5` per squadron at gf 0, then `5d5c19` per ship at gf 1.

## 2. t02 (a): callers of stop and hold `0x3a9880`

Stop and hold: with a LOCOMOTOR (+0x333), locomotor vf +0x138(vf +0x188()) (stop), leave every coordinator
(`0x426d50`), adopt. **One draw when the stopped object passes the §4.7.1 predicate** (not a team member, not
hyperspacing, …); a squad member (an infantry ability user in a `Create_Team` company) draws nothing. Unit-ability ids
as ABILITIES_HARDPOINTS §2; "apply" = the special-ability class's vf30 (+0xf0, ABILITIES_HARDPOINTS §5).

| Caller (return) | Class | XML (FoC users) | When it stops and holds | Object | Then |
|---|---|---|---|---|---|
| `0x3a4070` (0x3a4138) | unit-ability behaviour toggle, called by `Set_Unit_Ability_Active` `0x38a350` for abilities implemented by a behaviour | `Unit_Ability` LURE (10), DEPLOY_TROOPERS (38), HUNT (6) off only [M: on / off reading of the flag] | every toggle except HUNT on and SELF_DESTRUCT (39) | the unit | the behaviour's activate / deactivate |
| `0x3a4070` (0x3a41e6) | same | same, not HUNT / SELF_DESTRUCT | turning off, when the behaviour reports it is running (`0x4c3700`) | the unit (a second draw) | behaviour deactivate |
| `0x443700` (0x443780) | GarrisonUnitBehavior (GARRISON_UNIT) approach, land | units with `Garrison_Value` | in range of the host and the host has room | **the host** (garrison vehicle / hover transport; a structure has no locomotor: no-op) | short coordinator move or start entering (L1_S4_REMAINING §4) |
| `0x4446b0` (0x44474f) | GarrisonUnitBehavior entry | same | the unit enters (fade done), after it is moved to the host (`0x3abb80`, itself an adopt) | the entering unit | garrison list, STORY_GARRISON_UNIT |
| `0x58f6a0` (0x58f7b0) | DeployTroopersBehavior activate (+0xb8) | DEPLOY_TROOPERS users | start of a deploy, when fewer than the maximum are out | the unit | movement lock (`0x3a8e50`, 1) |
| `0x5b07c0` (0x5b0962, 0x5b096a) | TacticalBribeAbility apply | `Tactical_Bribe_Ability` Tyber_Tactical_Bribe | the owner can pay the bribe | the user, then the target | credits taken, owner change |
| `0x5b07c0` (0x5b0c8a) | same | same | after the owner change | the bribed object [M: or its team container] | |
| `0x630950` (0x630b78) | SelfDestructBehavior activate (+0xb8) | SELF_DESTRUCT (39) users | the type's stop flag (+0x1b1) is set [L: which XML tag] | the unit | movement lock |
| `0x635ee0` / `0x635fc0` (0x635efb / 0x635fd6) | GarrisonHoverTransportBehavior vf28 (+0xe0) / vf29 (+0xe8) | garrisonable hover transports | +0xe0: a unit reserves a slot (approach start); +0xe8: [M] the reservation / boarding ends | the host | locomotor +0x2a8 := 3 |
| `0x635f60` / `0x636030` (0x635f7b / 0x636046) | GarrisonVehicleBehavior vf28 / vf29 | garrisonable vehicles | same | the host | |
| `0x6cc640` (0x6cc6eb) | InfectionAbility apply | `Infection_Ability` IG88_Infection | in range | the user | movement lock |
| `0x6f3800` (0x6f39ca), from the service `0x6f2d50` | CableAttackAbility | `Cable_Ability` (CABLE_ATTACK, snowspeeder) | after more than 30 wraps, once the walker has stopped | **the target** (walker) | movement lock on the target |
| `0x700860` (0x7009de) | GrenadeAttackAbility apply | `Grenade_Attack_Ability` Infiltrator / Mara_Jade / Kyle_Katarn | in range | the user | movement lock |
| `0x712600` (0x712727) | RadioactiveContaminateAbility apply | IG88_Radioactive_Contaminate | the throw starts (anim 0x70) | the user | |
| `0x713a90` (0x713aed) | ForceSightAbility apply | Yoda_Force_Sight | not already in anim 0x69 / 0x6a / 0x6b | the user | anim 0x69 |
| `0x719c90` (0x719db2) | RemoteBombAbility apply | Saboteur_Remote_Bomb_Attack | the placement is valid | the user | movement lock |
| `0x71a4d0` (0x71a517) | ProximityMinesAbility apply | Grenadier_Proximity, Grenadier_Proximity_Solo | target in range | the user | |
| `0x71ad50` (0x71ae72) | DrainLifeAbility apply | NightSister / Silri / Turret / Lancet_Drain_Life | no drain running | the user | the type's attack anim |
| `0x71c430` (0x71c4b2) | BuzzDroidsAbility apply | Buzz_Droids_Viper | not hyperspacing | the user | |
| `0x71cde0` (0x71ce72) | SummonAbility apply | Silri_Summon | no summon pending | the user | summon point recorded |
| `0x71d370` (0x71d40f) | CorruptSystemsAbility apply | IG2000_Corrupt_Systems | target not already corrupted | the user | anim 0x4a, effect object spawned |

So the drawing cases in vanilla are the ones whose user is not a squad member: Tyber (bribe), IG-88, Yoda, Silri,
Mara Jade, Kyle Katarn, the IG-2000, deploy / lure / self-destruct vehicles, garrison host vehicles, and the
cabled walker [M: per unit, by whether it is a team member]. Infiltrator, Saboteur, Grenadier and Night Sister
users are squad members: no draw. MOVEMENT §4.7.2's list used return addresses for some entries: 0x443733 = 0x443700,
0x4446e1 = 0x4446b0, 0x58f7a6 = 0x58f6a0, 0x5b09c8 = 0x5b07c0, 0x630b52 = 0x630950, 0x71a50d = 0x71a4d0, 0x71adfa =
0x71ad50, 0x71ce68 = 0x71cde0.

## 3. t02 (b): the other adopt sites

"Draw" = one `int(1, FPS)` at once, only when the object passes the predicate (most team members do not).

| Site (adopt return) | Trigger in game terms | Object adopted | Order on that path |
|---|---|---|---|
| movement lock `0x3a8e50` (0x3a8e9a) [H] | the **first** lock (counter obj +0x3a8 goes 0 → 1); later locks and every unlock adopt nothing. Callers (full list A1_READINGS §28.8): INTERDICT on (`0x3a2240`), Lua `Lock_Current_Orders` (`0x57cd40`, AI units), DeployTroopers, SelfDestruct, Infection / Grenade / RemoteBomb apply, the cabled walker, fighter / SimpleSpace hyperspace (`0x5c9a80`, `0x5cb020`, `0x6251d0`), `TeamLocomotor` vf82 `0x62f800` (space arrival), transport landing `0x644330`, hero clash `0x6abde0` | the locked object | if it is in a formation, Split_From_Formation `0x4f84e0` first (always one draw, stack `4f8518`); the adopt then finds it in a coordinator: no second draw [M]. Without a formation (e.g. a team container that `0x62f800` just took out of its coordinator) the adopt draws (Kashyyyk: the third draw per container) |
| `Set_Unit_Ability_Active` `0x38a350` (0x38a96c) [H] | turning on an ability whose data has `Disable_Movement` (+0x10c) while the counter is 0: an inline copy of the first lock. Vanilla: MISSILE_SHIELD (Dark Trooper, an AT-AA-type vehicle, a frigate) and INTERDICT (Interdictor, a special structure) | the unit | split / adopt as the lock row, then counter + 1; before the ability's own effects |
| DEPLOY `0x3a1ce0` (0x3a1f81) [H] | unit ability DEPLOY (2) toggled through `0x38a350` | the unit | targeting slot 9 told to stop first; leave coordinators, adopt: one draw |
| container eject / reposition `0x3abb80` (0x3abdf8) [H] | an object with a locomotor is put at a new place: garrison entry (`0x4446b0`, then stop-and-hold again) and exit (`0x444dd0`), contents released when their container is destroyed (`0x391310` kill), transport unload (`0x644330`), `0x38ed10`, `0x5433f0` (Lua `Promote_To_Space_Cinematic_Layer`) | the object; its team members are moved by recursion and not adopted | leave, move through the locomotor, adopt (one draw for an admitted object); objects without a locomotor are only moved |
| garrison leave `0x37a060` (0x37a369) [M] | a unit leaves a garrison (also on the host's death through `0x391310`) | each released unit | per released unit, in content order |
| landing `0x630fe0` (0x63107f) [H] | `LANDING` behaviour (FoC: the field commanders and their guards, Squad_Stormtrooper), land only: a dropped unit finishes its touchdown anim 0x60 | the landed unit | members of a squad: no draw [M]; a lone lander: one draw when the anim ends |
| fighter hyperspace finish `0x5c9d40` (0x5c9e62) [H] | `FighterLocomotor` end of hyperspace-in: unlock, stop, then when not a team member leave coordinators and adopt | the fighter / ship | one draw per lone arrival; squadron members none |
| SpaceRetreatCoordinator vf15 `0x532ee0` (0x533017) [H] | a space retreat is cancelled (owner matches, state allows) | each unit of the retreat list, in list order | per unit: locomotor stop (vf +0x138(0)), adopt: a draw only for units no coordinator holds |
| SpecialAbility helper `0x48ce70` (0x48cecb) [H code, M situation] | HackAbility (`0x701c10`) and RepairAbility (`0x70cb90`) release the user [M: at the end of the hack / repair] | the user, or its team container when the user is a squad member | leave the coordinator, adopt: one draw (the container passes) |
| REPLENISH_WINGMEN `0x3a2970` → helper (0x3a2c80) [M] | the ability creates replacement fighters in a new squadron container (space) | the new container | after the new fighters' creation draws, one draw per container |
| SpawnIndigenousUnits `0x531eb0` → helper (0x5320d4) [H trace] | the indigenous spawner (land) | none at once: the land container registers itself (§1.2); the spawner then orders it a group move (`0x6218f0` → `0x2a7b80`) | one composition per container at step 10 of the same tick (§1.3) |
| reinforcements `0x2bf2d0` → helper (0x2bf743, 0x2bfc2f) [M] | a reinforcement call (`ReinforceEventClass` `0x403cb0`) | space: each squadron container at once; ships register themselves and compose at the next step 10; then the arrival (lock row). Land (transport): see next row | |
| reinforcement landing (land) [H trace, M code] | the transport lands (`TransportLandingBehavior` `0x643c30` → `0x644330`), units made through the garrison grouping `0x2a5790` → helper (return 0x2a5a02) | none at once (LandTeamContainer registers itself) | jetpack traces: the creation draws at gf 1510, then one composition at step 10; `0x644330`'s own unload branch (eject, new coordinator, Move_To, lock) applies to carried units [M] |
| Lua `Spawn_Unit` `0x6a1f90` [H path] | script spawn: company placer `0x29e960` → helper, or `0x2bfdd0` for a single unit | as the skirmish forces: land containers compose at the next step 10, space containers adopt at once, singles adopt nothing (SimpleSpace ships compose at the next step 10) | |
| Lua `Reinforce_Unit` `0x6a0400` [H path] | queues a reinforcement | no direct adopt (it reaches the adopt only through the kill `0x391310`); the reinforcement rows apply when it arrives | |
| Lua `Create_Generic_Object` `0x6a1d20` [H path] | `0x29f810` only | no adopt; a self-registering locomotor (LandTeamContainer, SimpleSpace) composes at the next step 10 | |

Not in the task list but also adopting: campaign spawn `0x2a00b0`, fleet placement `0x4fca30` / `0x4fd5b0` /
`0x4fdfb0` / `0x4fea10` (MOVEMENT §4.7.3), StopMovement `0x4261f0` (only with `MoveAttackWhileStopped`, unused).

## 4. What the port needs

1. **Land team containers and SimpleSpace ships own a coordinator from birth.** When one is created, give it a new
   movement coordinator holding a move to its own position, with no formation. Draw nothing then. At the next movement
   update (step 10) the coordinator composes: one `int(1, FPS)` per such coordinator still holding its object.
2. **Land skirmish start:** draw in this order, nothing else. First, at load, the Load_Map-tail draws (cu8i). Second,
   the starting forces' creation draws, with no formation draw. Third, at gf 1 step 10, one draw per land team
   container: the starting containers in creation order, then those spawned during gf 1 (indigenous). Endor
   `_MP_LAND_ENDOR` with defaults gives 12 at gf 1.
3. **Space skirmish start:** one draw per squadron container right after its creation draws. Ships compose at gf 1
   step 10 [M, untraced].
4. **The team helper's adopt draws only in space.** For land containers it never draws, because the container already
   has a coordinator. This replaces MOVEMENT §4.7.5 item 1's land part (skirmish, reinforcements, indigenous, Lua
   `Spawn_Unit`).
5. **Stop and hold.** When one of the §2 abilities or behaviours fires, stop the object, take it out of its
   coordinator and adopt it. That is one draw if the object is not a squad member, at that point in the ability's
   apply: after its checks, before its animation, lock or spawn. Two of these draw twice:
   - the garrison entry (eject, then stop and hold);
   - the LURE / DEPLOY_TROOPERS toggle when turning off a running behaviour.
6. **First movement lock.** The first lock on a unit that is in a formation splits it out: one draw. If it has no
   coordinator, it is adopted instead: one draw. Later locks and unlocks draw nothing. `Disable_Movement` abilities
   lock this way when they turn on.
7. **Other adopts.** These draw at once for admitted objects:
   - DEPLOY;
   - the container eject or reposition, including the contents of a destroyed container and garrison exit;
   - a lone lander's touchdown;
   - a lone fighter's hyperspace finish;
   - a space-retreat cancel, per unit no coordinator holds;
   - the Hack / Repair release, the user or its squad container.
8. **Lua.**
   - `Spawn_Unit` follows rules 1 and 3.
   - `Reinforce_Unit` follows the reinforcement path.
   - `Create_Generic_Object` adopts nothing beyond rule 1.

## 5. RVA index

| RVA | What |
|---|---|
| 0x39bcb0 | adopt (§4.7.1 predicate; vf8 return 0x39bda0) |
| 0x3952e0 / 0x395421 | GOM post-load finish, Load_Map tail adopt (from 0x2b4440, return 0x2b44a7) |
| 0x2a4fc0 / 0x2a5659 | skirmish starting forces (return from the company placer) |
| 0x29e960 / 0x29ecb5 | company placer → team helper |
| 0x2a5a60 / 0x2a5da3 / 0x2a61d5 | team helper; container creation return; adopt return |
| 0x5abac0 | LandTeamContainer locomotor init (vf9; vtable 0x8a3630): own coordinator + Move_To |
| 0x625090 | SimpleSpace locomotor init (vf9; vtable 0x8aeaf8): same |
| 0x38cb30 | behaviour attach (vf9 init unless loading) |
| 0x426010 / 0x426d50 / 0x426140 / 0x425e30 | new coordinator / leave / held-by-coordinator test / admission |
| 0x426eb0 / 0x43e5b0 / 0x43e712 | coordinator update / land composition / its ctor-draw return |
| 0x4f8200 | Move_To (no formation) |
| 0x4f84e0 / 0x4f8518 | Split_From_Formation / its ctor-draw return |
| 0x3a9880 | stop and hold |
| 0x3a8e50 / 0x3a8e9a | movement lock counter / adopt return |
| 0x3a4070 | unit-ability behaviour toggle (stop and hold at 0x3a4138, 0x3a41e6) |
| 0x38a350 / 0x38a96c | Set_Unit_Ability_Active / its Disable_Movement adopt |
| 0x3a1ce0 / 0x3a1f81 | DEPLOY |
| 0x3abb80 / 0x3abdf8 | container eject / reposition |
| 0x37a060 / 0x37a369 | garrison leave |
| 0x443700, 0x4446b0, 0x443d10, 0x443ff0 | GarrisonUnitBehavior approach / entry / signal handler / service |
| 0x635ee0, 0x635fc0 / 0x635f60, 0x636030 | GarrisonHoverTransport / GarrisonVehicle host vf28 / vf29 |
| 0x58f6a0 | DeployTroopersBehavior activate |
| 0x630950 | SelfDestructBehavior activate |
| 0x630fe0 / 0x63107f | LandingBehavior vf6 |
| 0x5c9d40 / 0x5c9e62 | FighterLocomotor hyperspace finish |
| 0x532ee0 / 0x533017 | SpaceRetreatCoordinator vf15 (retreat cancel) |
| 0x48ce70 / 0x48cecb | SpecialAbility release helper (from Hack 0x701c10, Repair 0x70cb90) |
| 0x5b07c0, 0x6cc640, 0x6f2d50 / 0x6f3800, 0x700860, 0x712600, 0x713a90, 0x719c90, 0x71a4d0, 0x71ad50, 0x71c430, 0x71cde0, 0x71d370 | TacticalBribe, Infection, CableAttack service / wrap, GrenadeAttack, RadioactiveContaminate, ForceSight, RemoteBomb, ProximityMines, DrainLife, BuzzDroids, Summon, CorruptSystems (apply, vf30) |
| 0x62f800 | TeamLocomotor vf82 (space arrival: leave, lock) |
| 0x3a2970 / 0x3a2c80 | REPLENISH_WINGMEN / helper return |
| 0x531eb0 / 0x5320d4, 0x6218f0 | SpawnIndigenousUnits / helper return, its group move |
| 0x2bf2d0 / 0x2bf743, 0x2bfc2f | reinforcements / helper returns |
| 0x643c30 / 0x644330, 0x2a5790 / 0x2a5a02 | TransportLandingBehavior / landing unload, garrison grouping / helper return |
| 0x6a1f90, 0x6a0400, 0x6a1d20 | Lua Spawn_Unit, Reinforce_Unit, Create_Generic_Object |
| 0x391310 | object kill (releases contents through 0x37a060 / 0x3abb80) |

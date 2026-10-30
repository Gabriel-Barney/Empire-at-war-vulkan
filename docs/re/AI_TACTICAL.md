# Tactical AI: land and space (roadmap A1)

Clean-room behavioural spec from the Ghidra 12.2 decompilation of the 64-bit Steam `StarWarsG.exe` (FoC v1.121, image base 0x140000000; all addresses are RVAs), vtable slots read from the same exe, and the shipped FoC data (`Data/XML/AI/*`, `Data/XML/DifficultyAdjustments.xml`, `Data/XML/GameConstants.xml`, `Data/Scripts/{AI/LandMode,AI/SpaceMode,FreeStore,Library}/*.lua`). No decompiled code is reproduced.

Confidence tags: **[H]** read directly from code or data; **[M]** one inferred link (a field meaning, a dropped register argument, a name); **[L]** plausible, not verified.

**Scope.** This is the A1 spec: what the AI *player* does in a land or space battle. It does not repeat:
- **AI_GALACTIC.md §1** — the shared framework: AIPlayer, per-mode context (`TacticalAIManagerClass`), subsystem cadence, goal proposal / commit / Resolve_Category, templates, plans and PotentialPlan, plan threads and events, free-store service loop, learning, RNG list, save layout. Everything there applies to land and space unless this document says otherwise.
- **TACTICAL_COMBAT.md** — the *unit's own* targeting (TargetingBehavior rescans, priority sets, hard-point targeting), weapons, damage and the difficulty damage/health multipliers on the combat side. The AI player only steers units (move, attack, guard, abilities, priorities); the unit picks its shots itself. Already implemented in `engine/tactical/combat` (PHASE4 S8).
- MOVEMENT.md (how a move order is executed), COMMANDS.md (AI/Lua execute orders directly), LUA_API.md (binding RVAs), HOTSPOTS.md §1–2, OBJECT_RUNTIME.md (behaviour slot table at obj +0x332).

Notation: FPS = 30 (0x140b0a340), NOW = logic frame (0x140b0a320), F = current mode frame (mode +0x10). "Synced RNG" = LCG at 0x140a13e24 (TACTICAL_COMBAT §0.2): float draw 0x532480 `Get_Float(lo,hi)` and its 0..1 variant 0x5324d0, int draw 0x1ffb40.

---

## 1. Tactical AI player setup at battle start

### 1.1 Which players get a tactical AI [H]
- Every player whose `PlayerClass+0x360` (AIPlayer) is set gets a per-mode context when the land/space mode is created: 0x285760 → `0x4b0950` → factory `0x5f5160(modeType, aiPlayer)` → context Init `0x5f55f0` (subsystem factories, AI_GALACTIC §1.1). Mode type: 1 land, 2 space.
- Skirmish slots use `Faction.Basic_AI` (`BasicEmpire`, `BasicRebel`, `AI_Player_Underworld`); campaign players use `AI_Player_Control`; hostile/neutral sides may run AIPlayerType `None` (AI_GALACTIC §1.2). `PlayerWrapper.Enable_As_Actor` (0x4b0250) forces a context and sets its actor byte +0x68.
- The tactical subsystems are: `LandPerceptionSystemClass` (vt 0x8be3e8, 0x138 B) / `SpacePerceptionSystemClass` (vt 0x8be480, 0xf8 B); `LandGoalSystemClass` (vt 0x8be308) / `SpaceGoalSystemClass` (vt 0x8be378), both 0x2b8 B; the shared Planning, Execution, Learning and Template systems.

### 1.2 First service (context slot 3, 0x5f5ab0, while +0x69 == 0) [H unless tagged]
Runs on the AI player's first T7 slot after the mode starts, not at mode creation.
1. **Opponent** (+0x30): scan players in index order. Candidates are players hostile to this AI (relation table `player+0x370[myId] == 1`, 0x2824f0) that own at least one object in the mode (GOM per-player list count +0x30 ≠ 0, 0x2acb70) and whose faction `Is_Neutral` (+0x108) is 0. The first candidate is taken; later candidates overwrite it only if they are the battle's attacker or defender (conflict record from 0x28a7a0: +4 / +8; read by 0x28a780 / 0x28a910). Result: the last attacker/defender enemy in index order, else the first enemy with units. [H; M for "+4/+8 = attacker/defender"]
2. Slot 6 ("begin mode") on Perception, Goal, Planning, Execution, Learning, in that order, then Template init 0x6bbed0 (AI_GALACTIC §1.7). Tactical specifics of each slot 6 are in §1.3.
3. Set +0x69 = 1.

### 1.3 What each subsystem does at battle start
| Subsystem | Land | Space |
|---|---|---|
| Perception slot 6 | 0x6b9140: register the game evaluator and one player evaluator per player (binding id = player name id, cached at player +0x478); **add every existing object** through slot 11 (§3.1) in GOM object-list order; then add every **AI hint zone** of the map as a region target (0x6b8d80, §3.2). [H] | 0x6ba570: same evaluators; add existing objects with `Has_Space_Evaluator` (0x6ba320); then build the **region grid** (0x6b9da0, §3.2). [H] |
| Goal slot 6 | base init (quota from goal-type × target counts, AI_GALACTIC §1.3) **plus the tactical credit transfer** below. [H] | base init only (Space has no override). [H] |
| Execution slot 6 (0x526120) | creates `LandExecutionPathFinderClass` (zone graph) and runs Recompute_All_Pairs once; next recompute staggered per player (PHASE2_ANSWERS §11). Then starts the free store (slot 0xa8 → script start, §4). [H] | no path finder (only modes 0 and 1 create one); free store start. [H] |
| Template init | picks the mode's template list from AIPlayerType `Templates/Land` or `/Space` (shipped: `Test_Land`, `Test_Space`). [H] | same |

**Tactical credit transfer (land only)** [H code; M field names]
- At land slot 6 (0x6b87e0): if the mode under the land mode in the GMM stack is **galactic** (campaign battle) and the AI player is not human, `0x612b00` runs on the **galactic** context's budget: for each budget category, `take = min(allowance(+0x5c) × available(+0x44), available)` when both are positive; subtract `take` from the category's available and pool; `Σtake` becomes the player's credits, and `credits − Σtake` is saved at budget +0x88 (flag +0x84).
- At land slot 7 (battle end, 0x6b8860 → 0x614a70): player credits = saved; then `Add_Credits(leftover − released(+0x8c), aiGain=true)` — so tactical leftovers go back through `Add_Credits`, which multiplies positive AI gains by `Credit_Multiplier` (0x27f370) [H; the multiplication of returned leftovers is a quirk to keep].
- `allowance` is the template `Tactical_Budget_Allowance` (AI_GALACTIC §1.7). Shipped galactic templates comment it out or set 0, so campaign AIs normally fight land battles with 0 credits unless a plan calls `PlayerWrapper.Release_Credits_For_Tactical` (0x603c70 → 0x6147d0; land only). Skirmish (no galactic parent): no transfer, the AI uses its own skirmish credits. Space battles never transfer.

### 1.4 Battle end and teardown [H]
- Slot 7 on goal system: land returns credits (above), then the base clears budget and both goal containers (0x6cafb0).
- Context destructor-side cleanup (slot 2 of the context, 0x5f5d70) destroys Goal, Planning, Execution, Perception, Learning (each slot 7 then delete), then Template (0x6bc790). Plans end through the normal plan-end path (AI_GALACTIC §1.9.4: units go back to the free store, learning outcome recorded).

---

## 2. Update cadence inside the tactical tick

**Call chain** [H] (AI_GALACTIC §1.3): main loop T7 → PlayerList update (player index order) → `Player::Update` 0x287e70 → `AIPlayer::Update` 0x4b0ca0 → only the context whose mode is the current mode → slot 3 0x5f5ab0. So land/space AI runs **after** the mode tick (cinematics, mode frame++, common tick with GOM object services, combat, FOW) of the same frame (MAIN_LOOP §3.2–3.4). The galactic AI does not run during a battle.

**Per-frame order inside a tactical context** when `TheAI` is enabled for the mode (0x4d9f80; Suspend_AI clears the master byte): Perception → Goal → Planning → Execution → Learning → Template, each only when its next-service frame ≤ NOW.

| Subsystem | Interval | Tactical work |
|---|---|---|
| Perception | 10 s (300 frames) | none: Service only reschedules (land slot 2 0x4e4510 and space 0x4e0180 tail-jump to the base). Targets change only through object add/remove signals (§3). [H] |
| Goal | every frame | proposal slice / commit / quota; after a commit, sleep `Land_` or `Space_AI_Goal_Cycle_Sleep_Duration × FPS` frames (land slot 11 0x6b87b0 reads difficulty +0x48, space 0x6b8950 reads +0x44). Shipped: 15 s on Easy, 0 otherwise. [H] |
| Planning | 3 frames | resume every plan's threads (AI_GALACTIC §1.9.4). [H] |
| Execution | 3 frames | (land) zone all-pairs recompute every 90 frames; free-store service (§4.3); AIBuildTask and blocking-status FSMs. [H] |
| Learning | 10 s | prune recent records. [H] |
| Template | every frame | round-robin trigger checks. [H] |

**Mode-tick side (not the AI player):** each land/space mode tick calls 0x527dd0 per AI player in its list (land +0x350, space +0x348) at MAIN_LOOP §3.2 step 2. It only clears per-frame caches (several hash containers and an expiring list keyed by a frame stamp from 0x4c2030) [M: cache meaning]. Port: clear the equivalent perception caches at that point.

**Object-level AI that is not the AI player:** `UnitAIBehaviorClass` (behaviour 0x12) feeds the tactical threat grid `TacticalPerceptionGridClass` (global 0x140b31900, created by 0x4df890) and has its own synced draws (0x550ed0). This is unit-level and runs in the GOM object service for every player, human or AI. [M]

---

## 3. Tactical targets (AITargetLocations) and perception

The goal system proposes `(goal type × target)` pairs; targets come from the perception system's list (slot 8; land and space both return the vector at +0xe0 — `lea rax,[rcx+0xe0]` at 0x6b9130) [H]. Each target also has a perceptual evaluator registered under a name id, so equations can read `Variable_Target.*`.

### 3.1 Unit targets (`TacticalUnitAITargetLocationClass`, vt 0x8ce4a0, 0x38 B) [H]
- **Added** by perception slot 11 (land 0x6b8b40, space 0x6b9b60) for an object that: is not flagged dead/removing (obj +0x3a0 bits 0x40 and 0x80 clear); whose *representative type* (0x3973b0: fleet / transport / team container's type if the object is in one, else its own) has `Has_Land_Evaluator` (+0x66) in land or `Has_Space_Evaluator` (+0x65) in space; and that is not already registered (evaluator map keyed by the name id `"OBJECT_<id>"`, 0x4dc3a0).
  - Creates `LandUnitPerceptualEvaluatorClass` (0x6d70d0) or the space ship evaluator (0x6d27f0), registers it under the object's name id, subscribes the perception system to the object's signal 0x10 (destroyed), and **appends** the target to the list.
- **When:** at first service for all existing objects (§1.3), then for every object later added to the mode (GOM add hook 0x2b3670: for every AI player in index order whose context for that mode is initialised). [H]
- **Removed** on the object's destroyed signal (0x6b96f0): the matching target (by object id, target slot 9) is erased from the vector **keeping order**, its evaluator removed (0x4dfbe0). [H]
- Space init (0x6ba320) tests `Has_Space_Evaluator` on the object's own type, while the runtime path tests the representative type. [H; keep both]

### 3.2 Region targets (`TacticalRegionAITargetLocationClass`, vt 0x8ce548) [H]
- **Land:** one region per **AI hint zone** of the map (land mode list +0x3a0, filled by the map loader 0x3b8bc0 with `AIHintZoneClass` objects). Name = zone name (interned), centre = zone +0x30..+0x38, evaluator `LandLocationPerceptualEvaluatorClass` (0x6d55a0). Each land region owns a `ZoneReachabilityClass` (0x73f1d0, §3.4). A map without hint zones has **no land Tactical_Location targets** (and `Get_Most_Defended_Position` fails, §6.4). Regions are appended after the unit targets, in zone-list order. After load, regions re-link to zones by name (0x6b9580, case-insensitive).
- **Space:** a grid over the map bounds (mode vtable +0x208) with square cells of **`AI_SpaceEvaluatorRegionSize`** (GameConstants +0x10d0 = 0x140b168d0; shipped 2000). `nx = ceil(width/size)`, `ny = ceil(height/size)`; loop x outer, y inner; cell name `"CELL_<x>_<y>"` (0x4dc570); evaluator rect = the cell clipped to the map; target position = cell centre with z = 0 (computed from the unclipped cell). No reachability object. Built only when the threat grid exists (0x140b31900 ≠ 0).
- Region rect for perception (slot 14 0x7254f0): square of `AI_SpaceEvaluatorRegionSize` (space) or `AI_LandEvaluatorRegionSize` (+0x10d4, shipped 2000) centred on the region.

### 3.3 `Applies(flags, aiPlayer)` — which targets a goal can use [H]
Bits are AIGoalApplicationType (AI_GALACTIC §1.4). `Global` (0x10) never matches a target.

**Region** (0x725730): matches iff `Tactical_Location` (0x200) is set.

**Unit** (0x724ef0):
1. Reject if the object is fully fogged for the AI player (0x35f470 with "ignore AIUsesFogOfWar" = true), unless its type has `Last_State_Visible_Under_FOW` (+0x88) or `Initial_State_Visible_Under_FOW` (+0x89). So the AI does not see units hidden by its own fog. [H; M for the fog test semantics]
2. `friendly` = the object has an owner (owner record +4 ≠ −1) and that owner is allied with the AI player (0x4ad9f0). Unowned/neutral objects count as **not friendly** (they match the Enemy_* bits). [H; M "allied"]
3. Classify by behaviour slots (OBJECT_RUNTIME §4.3):

| Object has | Friendly bit | Enemy bit |
|---|---|---|
| a locomotor (behaviour 0x01, obj +0x333) | Friendly_Unit 0x20 | Enemy_Unit 0x40 |
| else TACTICAL_BUILD_OBJECTS (0x29, +0x35b) — build pad | Friendly_Build_Pad 0x400 | Enemy_Build_Pad 0x800 |
| else CASH_POINT (0x3c, +0x36e) | Friendly_Cash_Point 0x1000 | Enemy_Cash_Point 0x2000 |
| else type `Reinforcement_Enabling_Radius` (+0x1e20) > 0 | Friendly_Reinforcement_Point 0x4000 | Enemy_Reinforcement_Point 0x8000 |
| else (structure) | Friendly_Structure 0x80 | Enemy_Structure 0x100 |

Match iff (friendly and the friendly bit is set) or (not friendly and the enemy bit is set).

### 3.4 Reachability (land only) [H]
`ZoneReachabilityClass` slot 7 (0x73f5f0) returns a path cost or 1e18 (unreachable). It applies only in land (context mode 1) and only for reachability levels 6–10; other levels (and space) always return 1e18 → callers treat "no reachability object" as no filter (§6.2).
- Path from the source's zone to this zone through the land execution path finder (zone graph, PHASE2_ANSWERS §10 machinery), then accept if the path's threat stays below the tolerance (0x6913a0, also capped at 1000 hops/units [M]).
- Tolerance by level: No_Threat 0; Low `Low_Threat_Reachability_Tolerance` (GameConstants +0x157c, 1000); Medium (+0x1580, 3000); High (+0x1584, 5000); Any_Threat 1e18.

### 3.5 Perception tokens bound per evaluation [H]
Every tactical evaluation (proposal, FindTarget, EvaluatePerception) binds `Variable_Self` = the AI player, `Variable_Enemy` = the context opponent (§1.2; omitted if none), `Variable_Human` = the first player with `Is_Human` (+0x62) in index order (0x294a00; omitted if none), `Variable_Target` = the target's name id. Tactical token families used by the shipped equations (BasicLandEquations, OffensiveSpaceEquations, AI_Equations_*_LandSkirmish) include `EnemyUnitConcentration`, `FriendlyUnitConcentration`, `IsFriendlyStartLocation`, `IsEnemyStartLocation`, `IsContestable`, `HasBuiltObject`, `AdditionalPopulationCapacity`, `TimeLastSeen(Unnormalized)`, `DistanceToNearestFriendly/Enemy {Parameter_Category}`, `Location.FriendlyForce/EnemyForce`, `ForceNBTD`, `Health`, `Shield`, `ContainsHero`, `IsDefender`, `BaseLevel`, `FriendlyForceUnnormalized`, `ReinforcementsUnnormalized`, `UnitSpaceAvailable`, `Game.IsCampaignGame`, `Game.IsLandControlGame`, `Game.Age`. Their evaluators (`Land/Space{Game,Player,Location,Unit,Structure}PerceptualEvaluatorClass`, SUBSYSTEMS §ai_perception) are **not specified yet** (open item 1); S2 implemented the language and the galactic tokens only.

---

## 4. Goal sets, templates and difficulty (shipped data)

### 4.1 Who proposes what [H]
| AIPlayerType | Tactical function sets (in list order) | Land / Space free store | Templates |
|---|---|---|---|
| BasicEmpire | BasicOffensiveSpaceSet, BasicLandGoalSet, AI_GoalSet_ExpansionGeneric_Land, AI_GoalSet_Empire_Land | BusyTacticalFreeStore / same | Test_Land / Test_Space |
| BasicRebel | same pattern with the Rebel land set | same | same |
| AI_Player_Underworld | AI_GoalSet_ExpansionGeneric_Land, AI_GoalSet_Underworld_Land, AI_GoalSet_Underworld_Space, BasicOffensiveSpaceSet, BasicLandGoalSet | same | same |
| None | BasicOffensiveSpaceSet, BasicLandGoalSet, AI_GoalSet_ExpansionGeneric_Land | same | same |
| Basic*NoGalacticAI | as Basic* minus galactic sets; no free-store scripts declared | **none** → no free-store script runs [M] | same |

`Test_Land` / `Test_Space` turn on categories Hero, Tactical_Targeted, Defensive, Tactical_Untargeted, StoryArc, High_Priority, Med_Priority, Map_Control (space also NoBudget) and turn off Macro_Goal. Their `Budget` equations are irrelevant in tactical (the AIBudget is galactic-only, AI_GALACTIC §1.6); the budget does not limit tactical goals. Each goal's numeric desire comes from its `GoalFunctions` perception equation, plus the learning failure terms (AI_GALACTIC §1.5).

### 4.2 Land goals (LandGoals.xml and the Expansion/Empire/Rebel/Underworld land files) [H]
| Group | Goal types (application flags → targets) | Typical plan (Data/Scripts/AI/LandMode) |
|---|---|---|
| Area fights | Clear_Infantry (Tactical_Location, High_Threat), Clear_Vehicles (Medium_Threat), Land_Scout_Area | ClearInfantry, ClearVehicles, LandScout |
| Unit / structure kills | Destroy_Land_Unit (Enemy_Unit), Destroy_Structure, Destroy_Turbolaser (Enemy_Structure), Fire_Magnepulse, Steal_Vehicle (Enemy_Unit), Airstrike / Land_Bombing_Run / Orbital_Bombardment (Enemy_Unit \| Enemy_Structure, Global_Exclusions on themselves) | DestroyLandUnit, DestroyStructure, DestroyTurbolaser, FireMagnepulse, StealVehicle, Airstrike, LandBombing, AI_Plan_ExpansionGeneric_OrbitalBombardment |
| Map control | Secure_Contestable (Enemy_Build_Pad \| Enemy_Reinforcement_Point \| Enemy_Structure; two function sources), Secure_Victory_Control_Point (Enemy_Reinforcement_Point), Hold_Position (Friendly_Reinforcement_Point), Garrison_Bunker, Garrison_Transport | SecureBuildPad(Escorted), SecureVictoryControlPoint, HoldPosition, AI_Plan_ExpansionGeneric_GarrisonTarget |
| Defence | Defend_Structure (Friendly_Structure), PreDefend_Structure (Enemy_Unit), Patrol, Land_Escort, Defensive_Rush, Land_Retreat (Global, Time_Limit 240) | DefendStructure, PreDefendStructure, Patrol, LandEscortPlan, DefensiveRush, RetreatPlan |
| Building (skirmish) | Tactical_Multiplayer_Build_{Land_Units_Generic, Basic_Structure, Barracks, Command_Center, Infantry_Emergency}, Build_Econ_Structure, Build_*_Turret, Build_Bacta_Tank, Build_Repair_Bay, Build_Bunker, faction Build_* (Tactical_Location / Friendly_Build_Pad), Purchase_*_Upgrades, Tech_Upgrade_Emergency, Fund_Tactical_Structures, Sell_Tactical_Structure, Skirmish_Generate_Magic_Cash_Drop_Land, Burn_Units_Land | TacticalMultiplayerBuildLandUnitsGeneric, BuildStructureLand, BuildBarracksLand, BuildCommandCenterLand, BuildEconStructure, Build*Turret, …, PurchaseLandUpgradesGeneric, TechUpgradeEmergency, FundTacticalStructures, SellTacticalStructure, BurnUnitsLand |
| Heroes | Land_Heroes, Flame_Infantry, Droids, ObiWanAssists, HanSoloAssists, VaderAssists, PalpatineManager, Tyber_Manager_Land, Remote_Bomb_Friendly_Unit, Mine_Structure (category Hero, mostly `ReallyBig`) | LandHeroes, BobaFettFlameInfantry, Droids, *Assists, PalpatineManager, AI_Plan_Underworld_* |
| Disabled | Clean_Up and Land_Escort use the `Zero` function ("the freestore anti-idle is handling this behavior") | — |

### 4.3 Space goals (OffensiveSpaceGoals.xml, AI_Goals_Underworld_Space.xml) [H]
| Group | Goal types | Plans (Data/Scripts/AI/SpaceMode) |
|---|---|---|
| Attack | Destroy_Unit, Destroy_Unit_Minimal, Disable_Unit, Bomb_Unit (Enemy_Unit \| Enemy_Structure), Turbo_Attack_Unit, Turbo_Attack_Location, Space_Artillery, Sweep_Area, Ground_To_Space_Damage / _Disable, Fire_Death_Star | DestroyUnit, DestroyUnitMinimal, SystematicDisable, BombingRun, TurboAttack, TurboAttackLocation, SpaceArtillery, AreaSweep, GroundToSpace*, FireDeathStar |
| Positioning | Space_Scout, Hide_Transports, Hide_Surprise_Units, Return_To_Base, Patrol_Structure_Space, Space_Escort_Goal | SpaceScout, HideTransports, HideSurpriseUnits, MoveToLocation(Rush), FlankPlan, EscortPlan |
| Skirmish economy | Tactical_Multiplayer_Build_Space_Units_Generic, Purchase_Space_Upgrades_Generic, Secure_Build_Pad_Space, Build_Structure_Space, Build_Refinery_Space, Skirmish_Upgrade_Space_Station, Skirmish_Generate_Magic_Cash_Drop_Space, Defend_Space_Station, Burn_Units_Space | TacticalMultiplayerBuildSpaceUnitsGeneric, PurchaseSpaceUpgradesGeneric, BuildStructureSpace, BuildRefinerySpace, AI_Plan_ExpansionGeneric_SkirmishUpgradeSpaceStation / _DefendSpaceStation, BurnUnits |
| Retreat | Space_Retreat (Global, Time_Limit 240); Debug_Goal_Space = `Want_To_Retreat_From_Space` (NoBudget) | RetreatPlan |
| Hero | Tyber_Manager_Space | — |

Space goals use `Reachability Any` (5); land uses the threat levels. Reachability only filters through `Reachable_Target` and land slot 9 (AI_GALACTIC §1.5); proposal itself does not path-test.

### 4.4 Tactical slot overrides of the framework [H]
- Slot 9 (skip): land 0x6b8710 = active-set test plus the reachability-keyed lookup (AI_GALACTIC §1.5); space 0x6b8900 = active-set test only.
- Slot 10 (accept): land **and** space use 0x6b8850 = `Find_Plan_For_Goal` (0x6bae50) only — no budget affordability test (galactic adds one). Each accepted candidate therefore costs one synced draw at proposal time.
- Slot 11 (sleep): §2.

### 4.5 Difficulty effects in tactical
| Effect | Where | Values (Easy / Normal / Hard) | Conf |
|---|---|---|---|
| Goal-cycle sleep after each commit | §2 | land 15 / 0 / 0 s; space 15 / 0 / 0 s | H |
| Contrast multiplier on the enemy force a plan must beat | 0x5f1d00 (land +0x3c, space +0x38), AI_GALACTIC §1.9.3 | land 0.67 / 1.0 / 1.15; space 0.75 / 1.0 / 1.15 | H |
| Unit build time for AI players | 0x496f00: space reads +0x2c (`Space_Build_Time_Multiplier`), land +0x30 (`Land_Build_Time_Multiplier`) | 1.5 / 1.0 / 0.9 | H |
| Positive credit gains ×Credit_Multiplier | 0x27f370 | 0.5 / 1.0 / 1.2 | H |
| Damage dealt by AI-owned objects, AI max health, AI shields, land AI accuracy | TACTICAL_COMBAT §3.6 step 12, §4.4, 0x3727a0, 0x4b0dc0, 0x468750 | damage 0.6 / 1.0 / 1.8; health 0.4 / 1.0 / 0.75; shield 0.3 / 1.0 / 0.5 | H |
| Lua: `Should_Crush` off on Easy; Easy uses `Attack_Move` instead of `Move_To` in SecureBuildPad / GarrisonTarget; `GetChanceAllowed` is 100 for every difficulty (ability chance gate is a no-op) | PGEvents / PGCommands / plans | — | H |

---

## 5. The tactical free store (idle-unit management)

The framework part (script start, `ServiceRate` / `UnitServiceRate`, strict `now − Last > Rate` test, game-time base) is AI_GALACTIC §1.10. Tactical specifics:

### 5.1 Admission (Land 0x690a10, Space 0x6904b0 → base 0x4d2c90) [H unless tagged]
Called from the GOM add hook (0x2b3670) for the object's owner, and for allied AI players too when the session allows shared control and the type has `Is_Community_Property` (+0x27) [M]. Land admits an object only if all hold:
- owned by this AI player; not a TRANSPORT (behaviour 5); type not `Create_Team` (+0x67), not `Is_Dummy` (+0x10), not `Stay_In_Transport_During_Ground_Battle` (+0x2d); no INDIGENOUS_UNIT/EXPLOSION behaviour (0x20);
- either has LOBBING_SUPERWEAPON (0x37), or (has a locomotor and is a STARSHIP_COMBATANT or TEAM and the type does not have category bit 0x5f [M: bitfield at type+0x790]) — i.e. mobile combat units;
- a TEAM whose container type or any member type has bit 0x20 is refused; type bit 0x5b refused; obj+0x2b0 sub-object +0xf8 must be empty; not inside the mode's +0x38 manager set (0x4944e0) [M: reinforcement/garrison bookkeeping].
- Base checks: not dead/removing (+0x3a0 & 0x42 == 0, bit 0x80 clear), obj +0x3c8 set (in world) [M].
On admission: insert into the free-store map (key = object id), subscribe to signals 0x10 (destroyed) and 0x1a (owner changed), bump the per-type counter (+0xa0 map by type +0x11c), give the unit to any waiting AIBuildTask whose plan wants that type (FREESTORE_ALLOC path, AI_GALACTIC §2.4), call subclass slot 23, then call Lua `On_Unit_Added(unit)` if the context's mode is the current mode. `BusyTacticalFreeStore.On_Unit_Added` is empty.

Side tables kept by the same admission path [H code, M purpose]:
- 0x4d29a0: visible build pads (TACTICAL_BUILD_OBJECTS, no locomotor) and what is built on them (map +0xe0) — the pool behind `Get_Reserved_Build_Pads` / `Build`.
- 0x4d3070: visible objects without a build pad that this faction can use (0x372c10) — candidate garrisons [L].

### 5.2 Removal [H]
Object destroyed or owner changed (signals), or taken by a task force: erase from the map, decrement the per-type counter, unsubscribe (0x4d5000). Plan end and `Release_Unit` / `Release_Forces` re-admit units (AI_GALACTIC §1.9.4).

### 5.3 Service cadence and order (0x4d5a70, every Execution service = 3 frames) [H]
- `now = (float)F × (1/30)` seconds of mode time; `FreeStoreService()` (the script's `main` loop: one call per resume) runs when `now − LastService > ServiceRate`; `On_Unit_Service(unit)` runs for each free-store entry **in the map's list order** whose entry field +0x30 is 0 [M: the order-lock that `Lock_Current_Orders` sets], when `now − LastUnitService > UnitServiceRate`. Both stamps start at 0, so both fire on the first service with now > rate.
- BusyTacticalFreeStore: `ServiceRate = 20`, `UnitServiceRate = 2` → `FreeStoreService` about every 20.1 s; each unit about every 2.1 s (first > 2.0 s on a 0.1 s grid).
- **Iteration order hazard:** the map is an MSVC hash container keyed by object id (bucket = `(id ^ 0xdeadbeef) & mask`), whose list order depends on insertion history and rehashes. The port must use a container with a defined order. Recommended: ascending object id (deviation from vanilla only in which unit acts first within one service; each unit's decision only reads its own state and the world, but orders issued earlier in the same pass can change later units' `Find_Nearest` results). [H container, M impact]

### 5.4 `Lock_Current_Orders` (0x57cd40) [H code, M effect]
Only for AI-owned, mobile objects (has locomotor, TEAM handled). It marks the unit's free-store entry so `On_Unit_Service` skips it until the current order finishes. Scripts call it after giving a released unit an explicit order (`Try_Garrison`, `GoHeal`, `GoKite`, `Release_To_Hide`).

### 5.5 What BusyTacticalFreeStore does (shipped Lua; port runs it unchanged) [H]
- **Every ~20 s** (`FreeStoreService`): `enemy_location = FindTarget.Reachable_Target(PlayerObject, "Current_Enemy_Location", "Tactical_Location", "Any_Threat", 0.5)`, `friendly_location` likewise with `Current_Friendly_Location`; `aggressive_mode = EvaluatePerception("Allowed_As_Defender_Land", PlayerObject) > 0`. In space: keep `space_station = PlayerObject.Get_Space_Station()`; if valid, `Attack_Target(FindDeadlyEnemy(station))` and `Fire_Special_Weapon` at `Reachable_Target(..., "Needs_Hypervelocity_Shot", "Enemy_Unit", "Any_Threat", 0.5, space_station, 1500)`. Each Reachable_Target with p = 0.5 costs one synced draw when it has ≥ 2 positive candidates (§6.2).
- **Every ~2 s per free unit** (`On_Unit_Service`), skipping structures and transports:
  - unit has an attack target: heal if hull < 0.6 (`Service_Heal`), else kite if `FindDeadlyEnemy(unit)` is good against it (`Service_Kite`), else `SPOILER_LOCK` off and `Try_Weapon_Switch`.
  - unit has no active orders: `SPOILER_LOCK` on; kite; heal if hull < 1.0; `SPREAD_OUT` off; garrison management (`Service_Garrison`: eject healthy occupants good against an enemy within 600 when the container cannot fire, eject wounded ones near a healer, move a wounded container to the healer); attack (`Service_Attack`: nearest enemy not good against it → `Attack_Move` to its position if within `FREE_STORE_ATTACK_RANGE` 600, or always when `aggressive_mode` (first trying to garrison within 300); else `Attack_Move(enemy_location)` when aggressive); else guard (`Try_Deploy_Garrison`; `Guard_Target(nearest friendly structure)` or `Attack_Move(friendly_location)`).
- The `Allowed_As_Defender_Land` equation is 1 in skirmish, and in campaign gates a defending AI with a base until its army is ≥ 3 × the attacker's force + reinforcements — the "defender turtles" rule.

---

## 6. Lua query functions (exact semantics)

### 6.1 `FindTarget(taskforce, perceptionFunc, goalApplication [, p [, maxRange]])` (0x697a20) [H]
1. Arity: 3–5 arguments (the error text says "2 or 3"). `goalApplication` is parsed as an AIGoalApplicationType bitfield ("A | B"); `Global` is an error. `p` must be in (0, 1]; `w = 1/(float)p − 1`. With only 3 arguments `w = 0` and the range is an **uninitialised value** in the original [H] — shipped scripts always pass ≥ 4; the port should treat range as 0 (no limit) and log.
2. The candidate list is the perception target list of the task force's plan's player for the current mode (§3), in list order.
3. For each target that `Applies(flags, aiPlayer)`: evaluate the function with tokens Self / Enemy / Human / Target (§3.5). If the evaluation fails, skip. If `maxRange > 0` and the squared 3-D distance from the target position (slot 13) to the task-force position (TF slot +0x170) exceeds `maxRange²`, skip.
4. Selection, with `best = null`, `bestScore = 0.0` (double) and a list of (target, cumulative float):
   - if `score ≤ bestScore`: append the target with weight `(float)score` if that is > 0;
   - else the old best (if its `(float)bestScore > 0`) is appended, and the target becomes best.
   - Ties keep the earlier target as best.
5. If the list is empty or `w == 0`: return best (nil if none). Otherwise append best with weight `lastCum / w` (if > 0) — this makes P(best) = p — and pick with **one synced draw** `r = Get_Float(0, total)` and the first entry whose cumulative ≥ r (binary search, `Weighted_Random_Pick` 0x698de0; no draw when the total ≤ 0).
6. Returns a **GameObjectWrapper** if the target has a game object (unit target), else an **AITargetLocationWrapper** (region).

### 6.2 `FindTarget.Reachable_Target(player, func, goalApplication, reachability [, p [, source [, maxRange]]])` (0x698360) [H]
Same scoring and pick as §6.1 with these differences:
- The player argument selects the AI context (must be AI-controlled, "could not locate tactical AI manager" otherwise).
- `reachability` is an AIGoalReachabilityType name. When `source` (AITargetLocation, or a game object resolved to its target location) is given and the candidate has a reachability object (land regions only), candidates whose reachability cost from the source is 1e18 are skipped (§3.4). Units and space cells are never filtered.
- The range test uses the source's position (so `maxRange` needs `source`).
- Always returns an **AITargetLocationWrapper** (or nil).

### 6.3 `FindDeadlyEnemy(...)` (0x69c080) [H]
Reads the DAMAGE_TRACKING behaviour (0x3e) map of `attackerId → accumulated damage`. Damage-tracking entries decay each behaviour service by `(maxHealth + maxShield) × AI_SpaceThreatDecayStep` (GameConstants +0x10fc, 0.05) and are removed at ≤ 0 (0x58bd80) [H; M field names].
- `FindDeadlyEnemy(object or unit AITargetLocation)`: if the object has DAMAGE_TRACKING, use its map; else if it is a TEAM, sum the members' maps (recursively) by attacker id. Return the attacker with the strictly greatest total (first in map-list order wins ties) looked up by id (nil if that attacker no longer exists).
- `FindDeadlyEnemy(taskforce)`: sum the maps of all task-force units (teams expanded) by attacker id; same pick.
- `FindDeadlyEnemy(aiTargetLocation, radius > 0, player)`: query objects of that player with DAMAGE_TRACKING within `radius` of the location (GOM query 0x2a9ff0), sum their maps, same pick. [M: query filter]
- **Determinism:** the aggregation map iterates in MSVC hash-list order (minstd hash of the attacker id). Port: aggregate by attacker id and break ties by the lowest attacker id (deviation only on exact float ties).

### 6.4 `Get_Most_Defended_Position(positionSource, player)` (0x69f3a0) [H]
- Tactical modes only; exactly 2 arguments. The source is an AITargetLocation (its region) or anything with a position.
- **Land:** map the position to its hint zone (land mode slot +0x180; "map may not be zoned" error → nil). Score every threat-grid cell listed in that zone (zone +0x90, packed `x | y<<16`, list order).
- **Space:** score the threat-grid cells overlapped by a square of side `AI_SpaceEvaluatorRegionSize` centred on the position (rows outer, columns inner, clamped to the grid).
- Cell score (0x6545d0, mask all categories, ally test = true): the summed force of objects in the cell **allied with `player`** (combat power scaled by the cell area, plus the per-player threat-projection layer), ignoring players of neutral factions. [M: weighting details]
- Keep the maximum; on an **exact tie** draw `Get_Float01()` (0x5324d0) and switch to the new cell if > 0.5 — one synced draw per tie, in scan order.
- Returns a position at the chosen cell centre with z = 0 (grid y axis is flipped: `y = top − (row + 0.5) × cellH`), or the 1e18 sentinel position when nothing scored (Lua then gets a position object, not nil — PGEvents tests `if xfire_pos`). The grid origin/cell size are `TacticalPerceptionGridClass` +0xa0/+0xa4/+0xb8/+0xbc (open item 2).

### 6.5 Other bindings the tactical scripts use
| Binding | Behaviour | Conf |
|---|---|---|
| `EvaluatePerception(func, player [, target])` (0x699430) | one evaluation with the same token set | H |
| `AITargetLocationWrapper.Get_Game_Object` / `Get_Distance` / `Is_Valid` | unit target → its object; region → nil; distance from the target position | H |
| `Project_By_Unit_Range(a, b)` (ProjectByUnitRangeClass) | kiting position away from `a` by `a`'s weapon range — body not traced | L |
| `Find_Best_Local_Threat_Center(units, radius)` (FindBestLocalThreatCenterClass, clusters 0x6a8eb0) | AOE aim point for hero scripts; **consumes synced Get_Float draws** | H draw, L rest |
| `_FindStageArea` (0x6990f0) | galactic only (needs a parent planet) | H |
| `Reinforce_Unit(type, position \| false, player [, …, obeyZones])` (0x6a0400) | story / object-script reinforcement drop (tactical only); in a campaign battle it also creates the matching galactic fleet / persistent unit. **Not used by AI plans**; plans use `TaskForce.Reinforce` (§7.4) | H |

---

## 7. Task forces in battle: forming, moving, attacking, garrisoning, building

### 7.1 Plan shape [H, shipped Lua]
Tactical plans follow the framework (AI_GALACTIC §1.9): `Definitions()` declares `Category`, `TaskForce` teams (e.g. ClearInfantry: `"Infantry | Vehicle | Air = 1,5"`, `"LandHero = 0,2"`, exclusions), `AllowEngagedUnits = false`, `IgnoreTarget`, `RequiredCategories`, `AllowFreeStoreUnits = false` for build plans, `MinContrastScale`/`MaxContrastScale`, `ScriptPoolCount`. A typical land thread:
1. `BlockOnCommand(TF.Produce_Force())` — gather units (free store first, then purchase, AIBuildTask FSM).
2. `QuickReinforce(PlayerObject, AITarget, TF[, escort])` (PGTaskForce.lua): land — if not all units are on the map, `TF.Reinforce(target)` repeatedly until done, then `ScriptExit()` (the goal is re-proposed later); space — try reinforcing near the TF, the escort, the friendly start (`FindTarget(TF, "Is_Friendly_Start", "Tactical_Location", 1.0)`), the first friendly structure, capital, or the target, each with a 3 s timeout, anti-idling waiting units; exit if nothing arrived.
3. `Set_As_Goal_System_Removable(false)`, `Set_Land_AI_Targeting_Priorities(TF)` (per-category `Targeting_Priority_Sets`), then the order (`Attack_Move(AITarget, TF.Get_Self_Threat_Max())`, `Move_To`, `Guard_Target`…), polling the goal's own equation (`EvaluatePerception("Should_Clear_Infantry", PlayerObject, AITarget) > 0`) to continue.
4. Events from PGEvents.lua (`Default_Unit_Damaged`, `Default_Target_In_Range`, `Default_Unit_Move_Finished`, …) do the micro: heal/kite/garrison/crush/abilities (§8).

### 7.2 Producing units in battle
- `Produce_Force()` (0x5fe240): one AIBuildTask per missing unit of each team (AI_GALACTIC §1.9, §2.4 for the state list). In tactical the relevant states are FREESTORE_ALLOC / WAIT_FOR_FREESTORE_ALLOC / HIJACK_UNIT (take a free unit), WAIT_FOR_CREDITS, GROUND_UNIT_PRODUCTION and PRODUCTION_QUEUED (buy), JOIN_TASK_FORCE. [H states; M tactical path]
- **Land skirmish purchases** become entries in the player's reinforcement pool (player +0x2c8 table), so a bought land unit is not on the map until `Reinforce` drops it (§7.4). **Space skirmish** units are built at the space station (Tactical_Multiplayer_Build_Space_Units_Generic → `Produce_Force` of station-buildable types). Station upgrades are `Produce_Force` of `*_Starbase_Upgrade` objects (AI_Plan_ExpansionGeneric_SkirmishUpgradeSpaceStation). [M]
- Unit build time for the AI = normal build time × Land/Space_Build_Time_Multiplier (§4.5).

### 7.3 Orders (all build a blocking status and return it to Lua) [H unless tagged]
| Method | Land (LandTaskForceClass vt 0x8c36a8) | Space (SpaceTaskForceClass vt 0x8c1b88) |
|---|---|---|
| `Move_To(dest [, threatTolerance [, useZonePath]])` | 0x6ec410: dest = AITargetLocation, object, task force or position. Creates LandMovementBlockStatus (0x73baf0; mode 0 move). With `useZonePath` (3rd arg, default false) and source/dest in different hint zones it routes zone by zone through the land path finder (0x73bc50); otherwise one group move to the point (0x73bf40 / 0x73c140 → formation manager 0x426010 … 0x4f8200, the same path as a player's move). Shipped plans pass only the threat number, so they always use the direct move. | 0x6e7940: SpaceMovementBlockStatus (0x738870); optional speed / space-collision-avoidance string. |
| `Attack_Move(dest [, threat])` | 0x6e9440: same, mode 1 (attack-move; object dest uses 0x73c4f0) | 0x6e4fc0 |
| `Attack_Target(unit [, …])` | 0x6e99c0: unit targets only ("use Move_To instead"); block id 0x819 | 0x6e54d0: optional hard-point type; the string `"Random"` picks a hard-point type with **Get_Int(0, 13)**, 13 meaning "any" — one synced draw |
| `Guard_Target(target [, …])` | 0x6ebdc0: mode 2; a task-force target guards **one random unit of it** chosen with **Get_Int(0, n−1)** (no draw when n = 1) | 0x6e72c0: same rule |
| `Explore_Area(AITarget)`, `Fire_Special_Weapon`, `Fire_Orbital_Bombardment`, `Bombing_Run`, `Prepare_Ambush`, `Find_Closest_Enemy` | 0x6eaba0, 0x6eb7f0, 0x6eb730, 0x6e9d90, 0x6ec8b0, 0x6ead30 — bodies not traced | 0x6e66a0, 0x6e6ad0, —, —, 0x6e7e00, 0x6e68a0 |
| `Activate_Ability(name, on \| target)` | generic 0x5f9890: calls each unit's `Activate_Ability` (0x570a50), returns an AbilityBlockStatus | same |

Object-level orders from Lua (`unit.Move_To`, `Attack_Move`, `Guard_Target`, `Divert`, `Garrison`, `Leave_Garrison`, `Activate_Ability`) are the L1 objects bindings and execute directly (COMMANDS.md).

### 7.4 Reinforcement drops (`TF.Reinforce(target [, maxWait])`) [H unless tagged]
- **Land** (0x6ed5f0): requires the TF to have pending reinforcement types (else "taskforce has no reinforcements", nil). If the player currently cannot reinforce (0x2b03e0 false) it returns an immediately-failing ReinforceBlockStatus (id 0x81a). The target may be an object, an AITargetLocation or a task force (its position). Then:
  - **campaign battle** (galactic parent) 0x6ed9f0: draws the units from the player's galactic reinforcement pool (persistent units in orbit, 0x28aaa0 → +0x40);
  - **skirmish** 0x6ee010: draws from the player's tactical reinforcement pool (player +0x2c8 by type).
  - Both compute landing positions near the target (0x6eaf60, which honours reinforcement-point zones and `maxWait`), then, **from the last pending unit to the first**, spawn each unit whose population cost (0x373500) fits the remaining cap (0x2acb10), subscribing the block status to its arrival signal 0x19, and add the spawned unit (or its team members / ceil(members/teamSize) copies for `Create_Team` types) to the task force. [H order; M position rules]
- **Space** (0x6e8620): position-convertible target and optional max wait time; returns a SpaceReinforceBlockStatus; landing areas come from the space reinforcement FOW sampler. [M]
- `QuickReinforce` (Lua) is the only AI caller pattern; there is no separate engine "AI reinforce" logic.

### 7.5 Building structures (land) [H]
- Build plans name under-construction types (`UC_*`) in their task force; the PotentialPlan's TacticalBuild marshaller reserves (pad, type) pairs (AI_GALACTIC §1.9.3).
- `Build_All()` (0x6ea620) / `Build(typeName [, pad])` (0x6ea030): for each reservation, refuse with a log if the pad is no longer friendly/visible (0x39a2c0), if game rules block it ("pad may be occupied or enemies may be too close", 0x42e400), or if the rebuild cooldown after a destroyed building is active (0x42daf0); otherwise issue the pad build (0x42edf0, the player's build path, charges credits) and return a TacticalBuildBlockStatus (id 0x81e). `Get_Reserved_Build_Pads` returns the reserved pads.
- `Sell_Tactical_Structure`, `Fund_Tactical_Structures`, `Purchase_*_Upgrades` are plans over the same primitives.

### 7.6 Garrisons [H, Lua]
`Try_Garrison` (PGCommands): nearest own `GarrisonCanFire` container within range that `Can_Garrison(unit)`, else (if not offensive-only) any `CanContainGarrison`; `SPREAD_OUT` off, `unit.Garrison(container)`, and if the unit was in a TF, `Release_Unit` + `Lock_Current_Orders`. `Try_Deploy_Garrison` ejects occupants above a health threshold that are good against the target. `Garrison_Bunker` / `Garrison_Transport` goals run the GarrisonTarget plan (move next to the target, guard until it is ours/empty, then `Garrison`). `TaskForce.Can_Garrison` / `Garrison` (0x5fbcb0 / 0x5fc440) are the TF-wide versions.

---

## 8. Ability use by the AI

There is no engine-side "AI ability brain" in tactical: abilities are triggered by Lua (plans, PGEvents handlers, free store) and by the unit's own autofire (TACTICAL_COMBAT §5). [H]
- `Try_Ability(thing, name [, target])` = `Use_Ability_If_Able` after a chance gate that always passes (`GetChanceAllowed` returns 100 for all difficulties; `Chance(seed, 100)` is true for any seed ≥ 0). For a unit it checks `Has_Ability`, `Is_Ability_Ready` and not `Is_Ability_Active`; a task force skips the checks. No RNG.
- Event-driven uses (PGEvents.lua): on `Target_In_Range` → Power_To_Weapons, SPREAD_OUT, BARRAGE, BUZZ_DROIDS, FULL_SALVO, STIM_PACK (hull > 0.5), weapon switch; on `Unit_Damaged` → INVULNERABILITY (shield < 0.2), Defend (shield < 0.8), SENSOR_JAMMING / MISSILE_SHIELD / LASER_DEFENSE by incoming projectile type, DEPLOY_TROOPERS when not good vs the attacker, and the flee kit (Turbo, JET_PACK, SPRINT, SPOILER_LOCK, STEALTH, FORCE_CLOAK, EJECT_VEHICLE_THIEF, REPLENISH_WINGMEN). Cancelled SPOILER_LOCK / Turbo are re-enabled on `Unit_Ability_Ready`.
- `Try_Weapon_Switch`: fixed rules for T4B, Bossk, Destroyer Droid, MAL (MAL uses `GameRandom.Get_Float() > 0.8`, a synced draw evaluated only after the category test passes), else `Should_Switch_Weapons(target)`.
- Hero managers (PalpatineManager, *Assists, Droids…) use `ConsiderDivertAndAOE` → `Find_Best_Local_Threat_Center` (synced draws) and `FindTarget`.
- Special weapons and superweapons: `Fire_Special_Weapon`, `Fire_Orbital_Bombardment`, `Bombing_Run`, Death Star / ion cannon / hypervelocity plans (GroundToSpace*, FireDeathStar).

---

## 9. Retreat decisions [H unless tagged]
- Goals `Land_Retreat` / `Space_Retreat` (Global, Tactical_Targeted / Defensive, Time_Limit 240) with desire `Should_Retreat_From_Land/Space = Want_To_Retreat_* × Allowed_To_Retreat_*`. Example `Want_To_Retreat_From_Space`: 50 × (battle age > 20 s if defender, > 120 s if attacker) × (campaign: enemy force ≥ 4 × own as attacker or base-less defender, or mostly transports…; skirmish: base level 0).
- Plan RetreatPlan (both modes): `TF.Withdraw_Units()` (land 0x6ec2b0, space 0x6e77e0) asks the mode's retreat coordinator (mode +0x30) to start a retreat for the player (0x340920); false → exit. Then `Purge_Goals(PlayerObject)`, STEALTH / FORCE_CLOAK, `Block_Goal_Proposal()`, `Collect_All_Free_Units()`, non-removable, and sleep forever. The retreat itself (countdown, units leaving, battle end) is the coordinator's rule set (BATTLE_TRANSITIONS / MAIN_LOOP §3.3 step 12) and identical to a human retreat.
- Per-unit "retreats" are only Lua kiting and healing (`GoKite`, `GoHeal`, `Release_To_Hide`).

---

## 10. RNG draws in tactical AI (synced stream; add to AI_GALACTIC §1.13)

Position in the frame: T7, player index order; inside a context Goal → Planning (plan threads) → Execution (free store, build tasks).

| Site | Draw | When | Conf |
|---|---|---|---|
| Proposal accept → `Find_Plan_For_Goal` 0x6bae50 | 1 float | every accepted-desire candidate (land and space) | H chain |
| Resolve_Category random phase 0x6c8970 | 1 float per iteration | AI_GALACTIC §1.6 | H |
| PotentialPlan marshallers (Plan 0x729cc0 int, Contrast 0x72b940 float, Production 0x726010, Hero 0x7271f0) | per call | plan solve, rules not yet specified (AI_GALACTIC open item 3) | H site |
| `FindTarget` / `Reachable_Target` | 1 float | only when ≥ 2 positive candidates and p < 1 | H |
| `Get_Most_Defended_Position` | 1 float per exact tie | cell scan order | H |
| `Guard_Target(taskforce)` land/space | 1 int | target TF size ≥ 2 | H |
| Space `Attack_Target(x, "Random")` | 1 int (0..13) | per call | H |
| `Find_Best_Local_Threat_Center` | ≥ 1 float | per call | H site |
| Lua `GameRandom(lo,hi)` / `GameRandom.Get_Float()` | per call | PGEvents GoHeal/GoKite (> 0.5), Should_Crush (> 0.8, not on Easy), Try_Weapon_Switch (MAL), `DiscreteDistribution.Sample` | H |
| Perception `#` operator | per use | any tactical equation (S2) | H |
| Unit-level (UnitAIBehavior 0x550ed0, targeting, locomotors) | — | not AI-player decisions; GOM service order (TACTICAL_COMBAT §0.2) | H |

Lua short-circuit order matters: `(GameRandom.Get_Float() > 0.5) and Try_Ability(...)` always draws; `Is_Category(...) and GameRandom.Get_Float() > 0.8` draws only when the first test passes. Lua 5.0.2 semantics must be kept exactly (L1).

---

## 11. How decisions become commands; determinism and threads

- Every AI action ends in the same command handlers a player uses (move/formation move, attack, guard, ability, build on pad, reinforcement drop, retreat start, garrison), **executed directly in the AI's T7 slot**, never queued or networked (COMMANDS.md §5; AI_GALACTIC §1.12). Validation is the handlers' own (e.g. Build's three refusal checks, Reinforce's pop-cap and zone rules). For A1 "legal orders only": route AI orders through `engine/commands`' direct-execute path with the AI player as issuer, so they are subject to exactly the human rules. [H]
- **Thread counts.** All AI state changes are serial on the sim thread. Safe parallelism: perception evaluation of candidates in proposal and in FindTarget/Reachable_Target (pure functions of world state) with a serial, in-order reduction (same candidate order, same float accumulation, one RNG draw afterwards); cell scoring in Get_Most_Defended_Position (serial tie handling). Lua VMs (plans, free store) are single-threaded and resumed in a fixed order. [H for structure, M for "pure" — evaluator caches must be per-thread or precomputed per frame]
- **Order-defining containers to replace** (original order depends on hashing or pointers): free-store map (§5.3), FindDeadlyEnemy aggregation (§6.3), damage-tracking maps, perception evaluator map (lookups only, order-free), learning maps (AI_GALACTIC §1.11). Target lists, plan lists, AIBuildTask lists and hint-zone lists are ordered vectors/lists — keep their insertion order exactly.
- **Lua `pairs()`** over userdata-keyed tables (`Process_Death_Events`, `Process_Attacked_Events`, `Process_Proximities` in PGCommands.lua) needs the L1 creation-serial hashing patch; it is already in place.
- **Time bases.** Lua `GetCurrentTime()` and the free-store clock are mode time (`F/30`, float); plan waits are therefore frame-exact. The FSM `timeGetTime` stamp (AI_GALACTIC open item 1) must become frame time.

---

## 12. Open items

> **Status 2026-10-30:** Items 1, 2 and 4 are done: the tactical perceptual evaluators are in `engine/ai/tactical/src/tactical_perception.cpp`, the threat grid in `engine/ai/tactical/src/threat_grid.cpp` (AI_TACTICAL_PERCEPTION.md), and reinforcement landing with the `Use_Reinforcement_Points` rule and the can-reinforce test in `engine/ai/tactical/src/tactical_ai.cpp:1772-1813` and `engine/ai/tactical/include/alamo/ai/tactical/constants.hpp:44-45`. Items 3 and 6 are partly done: `Explore_Area`, `Prepare_Ambush`, `Fire_Special_Weapon`, `Project_By_Unit_Range`, `Get_Self_Threat_Max/Sum`, `Get_AI_Power_Vs_Unit` and `Is_Good_Against` are bound (`engine/lua/src/bind_ai_tactical.cpp`, `bind_script_remaining.cpp`), but `Should_Switch_Weapons` is a stub that always returns false (`engine/lua/src/bind_ai_tactical.cpp:324-328`). Item 7: HIJACK_UNIT is handled for tactical plans (`engine/ai/tactical/src/tactical_ai.cpp:1187-1199`).
1. **Tactical perceptual evaluators**: exact definitions of the land/space tokens listed in §3.5 (`EnemyUnitConcentration(NBTD)`, `Location.*Force`, `IsContestable`, `TimeLastSeen`, `AdditionalPopulationCapacity`, `UnitSpaceAvailable`, `IsDefender`, `BaseLevel`, …). This is the largest gap for A1 parity: every tactical goal desire depends on them.
2. **TacticalPerceptionGridClass** (0x653340 ctor, 0x6537d0, 0x657fc0, 0x6585f0): grid origin/size (likely from FOW cells × `AI_FogCellsPerThreatCell` 4), the entry weights in 0x6545d0 / 0x655e50, threat projection (`AI_*ThreatDistanceFactor`, `TurnRateFactor`, `LookAheadTime`, `AreaThreatScaleFactor`, `ThreatRangeCap`), and how `UnitAIBehavior` updates it.
3. LandMovementBlockStatus / SpaceMovementBlockStatus completion rules (arrival radius, what counts as finished for attack-move and guard), and `Explore_Area`, `Prepare_Ambush`, `Bombing_Run`, `Fire_Special_Weapon`, `Find_Closest_Enemy` bodies.
4. Reinforcement landing positions (0x6eaf60) and the space FOW sampler; ReinforceBlockStatus finish/fail rules; the 0x2b03e0 "can reinforce" test.
5. Free-store admission bit meanings (type bitfield +0x790 bits 0x20, 0x5b, 0x5f; obj +0x2b0/+0xf8; mode +0x38 set) and the +0x30 lock field writer; `Lua_Is_Object_On_Free_Store` / `Get_Object_Count`.
6. `Project_By_Unit_Range`, `Get_Self_Threat_Max/Sum`, `Get_AI_Power_Vs_Unit`, `Should_Switch_Weapons`, `Is_Good_Against`, `Get_Time_Till_Dead` (used by every land plan and PGEvents).
7. AIBuildTask tactical path (FREESTORE_ALLOC vs purchase vs HIJACK_UNIT ordering; skirmish purchase into the reinforcement pool vs spawn at the factory) — confirm with a probe.
8. Event code ↔ name table (shared with AI_GALACTIC open item 8) — needed to dispatch `Unit_Damaged`, `Target_In_Range`, etc. in the right order relative to combat.
9. Confirm `Has_Space_Evaluator` on own type at space init vs representative type at runtime (§3.1) and the conflict-record attacker/defender offsets (§1.2).
10. Probe: a skirmish AI-vs-AI land and space run logging per-frame synced-RNG counts and AI orders would validate §2, §5.3 and §10 (Gabriel launches; kit under `tools/probe_mod/`).

---

## 13. Summary for the implementer

**Key rules**
- One context per AI player per battle; first service picks the opponent (attacker/defender preference), runs the six subsystems' begin hooks, and (land campaign battles only) swaps the AI's credits for the template tactical allowance, returning leftovers × Credit_Multiplier at the end.
- Cadence is frame-driven in T7, player index order: Goal and Template every frame, Planning and Execution every 3 frames, Perception and Learning every 300 frames (no work). Easy sleeps 15 s after each tactical goal commit.
- Targets: unit targets for objects whose type has `Has_Land/Space_Evaluator`, appended on creation and erased in place on death; land regions = map AI hint zones; space regions = a 2000-unit grid named `CELL_x_y`. `Applies` classifies by behaviour slots (unit / build pad / cash point / reinforcement point / structure) and owner alliance, and hides fully fogged objects.
- Tactical goals are not budget-limited; desire = goal equation + learning terms; each accepted candidate draws once for its plan.
- The free store runs `FreeStoreService` every ~20 s and `On_Unit_Service` per free unit every ~2 s (BusyTacticalFreeStore); this anti-idle Lua is most of what a casual observer sees the AI do.
- FindTarget / Reachable_Target: score every applicable target, best-or-weighted pick with P(best) = p and at most one synced draw; FindDeadlyEnemy = max accumulated damage dealer; Get_Most_Defended_Position = max allied-force threat cell, ties by coin flip.
- All AI orders are ordinary commands executed directly with the AI as issuer.

**What to build first (A1 order)**

> **Status 2026-10-30:** This build order is finished: the tactical context, targets, perceptual evaluators, threat grid and task-force orders exist in `engine/ai/tactical/` (`tactical_ai.cpp`, `tactical_perception.cpp`, `threat_grid.cpp`) with the Lua bindings in `engine/lua/src/bind_ai_tactical.cpp`. Not complete: `Should_Switch_Weapons` is still a stub returning false (`bind_ai_tactical.cpp:324`). Step 6 (determinism harness) was not checked here.
1. Tactical context lifecycle and cadence on top of the A2 framework (§1–2), with an "ai.goals" CRC hasher.
2. Target lists and `Applies` (§3.1–3.3), plus the land hint-zone loader and space grid.
3. The tactical perceptual evaluators (open item 1) — without them no tactical goal has a desire. Start with the tokens the skirmish build and Secure_Contestable / Destroy_* / Clear_* equations use.
4. Free store admission + service (§5) running the shipped BusyTacticalFreeStore.lua through the L1 bridge; FindTarget, Reachable_Target, FindDeadlyEnemy (needs DAMAGE_TRACKING) and Get_Most_Defended_Position (needs the threat grid, open item 2).
5. TaskForce order bindings over `engine/tactical/movement` group moves and `engine/tactical/combat` attack/guard (§7.3), then Produce_Force / Reinforce / Build_All for skirmish economy.
6. Determinism harness: replace hash-ordered containers (§11), run the same scripted skirmish at 1 and N worker threads and compare per-frame CRC and RNG counts.

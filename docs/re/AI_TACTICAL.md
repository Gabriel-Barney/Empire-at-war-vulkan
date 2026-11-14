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
- `allowance` is the template `Tactical_Budget_Allowance` (AI_GALACTIC §1.7). Shipped galactic templates comment it out or set 0, so campaign AIs normally fight land battles with 0 credits unless a plan calls `PlayerWrapper.Release_Credits_For_Tactical` (0x603c70 → 0x6147d0; land only; the amount comes from `Evaluate_In_Galactic_Context` 0x69e1b0, evaluated in the galactic AI's world with the battle's planet as target: A1_READINGS §38). Skirmish (no galactic parent): no transfer, the AI uses its own skirmish credits. Space battles never transfer.

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
- **Land:** one region per **AI hint zone** of the map (land mode list +0x3a0, filled by the map loader 0x3b8bc0 with `AIHintZoneClass` objects). Name = zone name (interned), centre = zone +0x30..+0x38, evaluator `LandLocationPerceptualEvaluatorClass` (0x6d55a0). Each land region owns a `ZoneReachabilityClass` (0x73f1d0, §3.4). A map without hint zones has **no land Tactical_Location targets** (and `Get_Most_Defended_Position` fails, §6.4). Regions are appended after the unit targets, in zone-list order. After load, regions re-link to zones by name (0x6b9580, case-insensitive, first zone in list order); maps with duplicate zone names: TACTICAL_GAPS §6.8 (zone links are by 02 id, never by name; a repeated name shares the first zone's name id and evaluator).
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
| TR (Thrawn's Revenge) Basic* players (q6, 2026-11-04) | `BasicLandGoalSet` (+ SystemFunctions-type sets); no Expansion / faction land sets | **`LandTacticalFreeStore`** / `BusyTacticalFreeStore` (`LandFreeStoreScript` / `SpaceFreeStoreScript` are separate tags) | `Generic_Land` (Hero, Tactical_Targeted, Defensive, Tactical_Untargeted, High / Med_Priority, Map_Control) |

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

Space goals use `Reachability Any` (5); land uses the threat levels. Reachability only filters through `Reachable_Target` and land slot 9 (AI_GALACTIC §1.5); proposal itself does not path-test. Sources, the per-zone flood used by slot 9 and every tactical call site: A1_READINGS §25.

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
- `now = (float)F × (1/30)` seconds of mode time; `FreeStoreService()` (the script's `main` loop: one call per resume) runs when `now − LastService > ServiceRate`; `On_Unit_Service(unit)` runs for each free-store entry **in the map's list order** whose entry field +0x30 is 0 (+0x30 = the owning PotentialPlan, not the `Lock_Current_Orders` lock; the loop has no lock test: A1_READINGS §28), when `now − LastUnitService > UnitServiceRate`. Both stamps start at 0, so both fire on the first service with now > rate.
- BusyTacticalFreeStore: `ServiceRate = 20`, `UnitServiceRate = 2` → `FreeStoreService` about every 20.1 s; each unit about every 2.1 s (first > 2.0 s on a 0.1 s grid). Corrected in A1_READINGS §9: with the float frame clock the first eligible frames are F = 600 (then 1200) and F = 61 for units, then the next 3-frame Execution service.
- **Iteration order hazard:** the map is an MSVC hash container keyed by object id (bucket = `(id ^ 0xdeadbeef) & mask`), whose list order depends on insertion history and rehashes. The port must use a container with a defined order. Recommended: ascending object id (deviation from vanilla only in which unit acts first within one service; each unit's decision only reads its own state and the world, but orders issued earlier in the same pass can change later units' `Find_Nearest` results). [H container, M impact]

### 5.4 `Lock_Current_Orders` (0x57cd40) [H code, M effect]
Only for AI-owned, mobile objects (has locomotor, TEAM handled). It marks the unit's free-store entry so `On_Unit_Service` skips it until the current order finishes. **Corrected (A1_READINGS §28.2):** it increments obj +0x3a8 until signal 10/11 (order finished); the free-store entry is not touched and the On_Unit_Service loop does not test the lock (PP Setup A does). Scripts call it after giving a released unit an explicit order (`Try_Garrison`, `GoHeal`, `GoKite`, `Release_To_Hide`).

### 5.5 What BusyTacticalFreeStore does (shipped Lua; port runs it unchanged) [H]
- **Every ~20 s** (`FreeStoreService`): `enemy_location = FindTarget.Reachable_Target(PlayerObject, "Current_Enemy_Location", "Tactical_Location", "Any_Threat", 0.5)`, `friendly_location` likewise with `Current_Friendly_Location`; `aggressive_mode = EvaluatePerception("Allowed_As_Defender_Land", PlayerObject) > 0`. In space: keep `space_station = PlayerObject.Get_Space_Station()`; if valid, `Attack_Target(FindDeadlyEnemy(station))` and `Fire_Special_Weapon` at `Reachable_Target(..., "Needs_Hypervelocity_Shot", "Enemy_Unit", "Any_Threat", 0.5, space_station, 1500)`. Each Reachable_Target with p = 0.5 costs one synced draw when it has ≥ 2 positive candidates (§6.2).
- **Every ~2 s per free unit** (`On_Unit_Service`), skipping structures and transports:
  - unit has an attack target: heal if hull < 0.6 (`Service_Heal`), else kite if `FindDeadlyEnemy(unit)` is good against it (`Service_Kite`), else `SPOILER_LOCK` off and `Try_Weapon_Switch`.
  - unit has no active orders: `SPOILER_LOCK` on; kite; heal if hull < 1.0; `SPREAD_OUT` off; garrison management (`Service_Garrison`: eject healthy occupants good against an enemy within 600 when the container cannot fire, eject wounded ones near a healer, move a wounded container to the healer); attack (`Service_Attack`: nearest enemy not good against it → `Attack_Move` to its position if within `FREE_STORE_ATTACK_RANGE` 600, or always when `aggressive_mode` (first trying to garrison within 300); else `Attack_Move(enemy_location)` when aggressive); else guard (`Try_Deploy_Garrison`; `Guard_Target(nearest friendly structure)` or `Attack_Move(friendly_location)`).
- The `Allowed_As_Defender_Land` equation is 1 in skirmish, and in campaign gates a defending AI with a base until its army is ≥ 3 × the attacker's force + reinforcements — the "defender turtles" rule.

### 5.6 Land skirmish end game: how idle units reach enemies they cannot see (RE, 2026-11-02) [H unless tagged]
Scope: this section is **FoC**. TR runs a different land free store (`LandTacticalFreeStore`: visible enemy only within 600, else `enemy_location`, then heal and walk home), its `Should_Burn_Units_Land` skirmish branch is the pop-capped one only, and its `Should_Firesale_Land` is campaign-only; see §7.7.4-7.7.5.

Question from the port: late FoC skirmish survivors (e.g. an M1 and a TIE Crawler) park for thousands of frames about
600 from enemy troopers that are fogged to them. Retail has **no unit-level hunt** for this; the AI layer moves the
units, using knowledge that ignores fog. The pieces, in the order they matter:

**1. Units alone never find fogged enemies.**
- Target acquisition rejects candidates fully fogged for the shooter's player with **force 1** (TACTICAL_COMBAT §2.3
  step 4, FOG_OF_WAR §5.3): AI players are fogged there too, whatever `AIUsesFogOfWarLand` says.
- The scan cube is the attack range R; chase ranges are added only inside a formation block (TACTICAL_COMBAT §2.2,
  0x566f50) and the candidate still passes the fog filter. FoC `M1_Repulsor_Tank` / `TIE_Crawler`: attack 180 / 175,
  `Land_FOW_Reveal_Range` 200, `Idle_Chase_Range` 0, `Guard_Chase_Range` 200, `Attack_Move_Response_Range` 150.
- Both list the HUNT behaviour (0x21) in `LandBehavior`, but HuntBehaviorClass (vtable 0x8aef40) starts **disabled**
  (vf9 0x628800 → vf +0xc0) and is enabled only by the HUNT unit ability toggle 0x3a4070 (TACTICAL_GAPS §2.2). Neither
  type has the HUNT unit ability (FoC HUNT users on land: `T2B_Tank`, `Pod_Walker`, `Hutt_Pod_Walker`, `Swamp_Speeder`,
  `Vornskr_Wolf` and some companies), no shipped AI Lua activates HUNT, and AI autofire covers only special `Abilities`
  (TACTICAL_GAPS §1.3), not `Unit_Abilities_Data`. So in vanilla the HUNT behaviour of AI units never runs [H data and
  start state; M that nothing else enables slot 0x21: no other access to it was found]. IDLE (0x62e570) only plays
  idle animations.
- No map reveal happens on its own: the overrun reveal (0x364920) runs only in the networked game types 1, 2, 4, not
  in local skirmish (FOG_OF_WAR §3.3); setup-phase reveal is off; the victory monitor reveals nothing.

**2. The AI's area knowledge is live (not fogged).** With shipped `AIUsesFogOfWarLand` = False, the UNIT_AI "fogged
for p" byte is never set for an AI player (AI_TACTICAL_PERCEPTION §4.7), so the threat layer and the concentration /
force tokens see every enemy UNIT_AI object: the per-cell concentration 0x656800 counts live entries unless the object
is marked fogged for Self, and memory entries only while it is. Real fog (force 1) applies only to `Applies` unit
targets (§3.3), `Find_Nearest` (LUA_API), `DistanceToNearest*` and `TimeLastSeen`.

**3. Free store, every ~2.1 s per free unit (§5.3, §5.5; `BusyTacticalFreeStore.lua`).** For a unit with no active
orders, `Service_Attack`:
- `closest_enemy = Find_Nearest(unit, owner, false)`: nearest **visible** enemy (force 1);
- visible and not `Is_Good_Against(unit)`: `Attack_Move(its position)` when within 600, else (aggressive) after a
  failed `Try_Garrison` within 300, `Attack_Move(its position)` anyway;
- otherwise, when aggressive: `Attack_Move(enemy_location)`. `aggressive_mode` = `Allowed_As_Defender_Land` > 0,
  which is **always 1 in skirmish**. `enemy_location` is refreshed every ~20 s by
  `Reachable_Target(Self, "Current_Enemy_Location", "Tactical_Location", "Any_Threat", 0.5)`: score =
  `EnemyUnitConcentration` of each hint zone (live, item 2), no source so no reachability filter, best with
  probability 0.5, others weighted by score (one synced draw when ≥ 2 zones score > 0; §6.2). Zones with no enemy
  UNIT_AI object whose centre cell lies in the zone's cell list score 0; no zone > 0 → nil.
- `enemy_location` nil → `Service_Guard`: `Guard_Target(nearest friendly structure)`, else
  `Attack_Move(friendly_location)`.

So a retail AI unit with nothing to shoot walks to the zone centre (+0x30, §3.2) of a zone that really holds enemies,
re-ordered whenever it is idle again, and acquires them once its own reveal circle (or an ally's) uncovers them.
[M: retail can still stall when the hidden enemies sit in cells that no hint zone lists, or when the zone centre is
farther than ~200 + R from them; `_MP_LAND_UTAPAU` has zones, TACTICAL_GAPS §6.8.]

**`Has_Active_Orders`** (0x579f10, read in disassembly 0x140579fd9–0x14057a0f8) decides who gets the no-orders branch:
- true when the unit has a current target (0x397a40);
- false when the object has no locomotor slot (+0x333 = −1); when the locomotor behaviour or the mode's formation
  manager (mode vf +0x228) is null the binding returns no value (nil, falsy);
- with formation F = locomotor pack +0x1f0: **F null, or F +0x240 = 0 (not yet at the end of its only block,
  FIGHTER_FLYING §4.10) → true**; F's last block with flag 0x10 → false; otherwise true iff the locomotor is not in
  an idle state (vf +0x150; Walk 0x42c890: states 0, 0x16, 0x1C, 0x1E, 0x1F, 0x25–0x27, and 0x19 Deployed when type
  +0x23b0 > 0) or is moving (vf +0x1c0).
- Quirk: a unit that has never had a formation counts as busy, so the free store leaves it alone until something
  orders it [H code; M how often that happens: factory and reinforcement units normally get a move].

**4. Area-clearing plans (goal system, `BasicLandGoalSet`).** `Clear_Infantry` / `Clear_Vehicles` (Tactical_Location,
High / Medium_Threat, Time_Limit 30) score zones with `EnemyUnitConcentrationNBTD {Infantry}` / `{Vehicle}` (live).
Plans `ClearInfantry.lua` / `ClearVehicles.lua` take 1–5 Infantry | Vehicle | Air units (and 0–2 land heroes; not
already engaged), `Attack_Move` the zone, then repeat `Attack_Move(Find_Nearest(MainForce, "Infantry", Self, false))`
(visible only) while `Should_Clear_Infantry` > 0. `Land_Scout_Area` (`LandScout.lua`, 1 Infantry | Air | LandHero)
`Attack_Move`s and `Explore_Area`s zones fogged for a long time (`TimeLastSeenUnnormalized` > 120 / 400 s, real fog;
× 0.5 in skirmish).

**5. Last resort: `Burn_Units_Land` and the "firesale" reveal** (goal Global, High_Priority; `BurnUnitsLand.lua`).
- Desire `Should_Burn_Units_Land` > 0 when (skirmish, `Game.Age` > 400 s, not saving for refineries, and
  `Want_To_Retreat_From_Land`: for a non-defender, the enemy's landed force ≥ 2 × own force + reinforcements; retreat is
  never allowed in skirmish) or (skirmish, no unit space left, and reinforcements > 25 % of the fielded Infantry |
  Vehicle | Air force or > the enemy's fielded force).
- If `Should_Firesale_Land` > 0 (the same retreat wish, `Game.Age` > 300 s, **no command centre / palace**
  (`Imperial_Command_Center`, `Rebel_Command_Center`, `U_Ground_Palace`), no refinery, credits < 2000):
  `FogOfWar.Reveal_All(Self)` (0x6a5b00: the AI's grid +0x50 → `FOW_Object_Hidden` = visible for everything), and in a
  non-multiplayer skirmish also `Reveal_All` for `Find_Player("local")`.
- Then, until `Should_Firesale_Land` is 0 (so a single pass when it was 0 at the start): `Purge_Goals`, eject
  non-vehicle garrisons, `Collect_All_Free_Units`, `SPREAD_OUT` off, nearest enemy with property `InBase` else any
  nearest enemy (`Find_Nearest`, visible — i.e. everything after the reveal), and `Attack_Move` that one enemy in
  ≤ 20 s blocks until it is dead (the loop does not re-pick a target while it lives).
- After the reveal, the AI's own unit targeting (force 1) and `Find_Nearest` see every enemy, so its units also
  auto-acquire anything within R.

*For the implementer (port symptom: idle survivors next to fogged enemies):*
1. Check the perception grid: for AI players (`AIUsesFogOfWarLand` False) the "fogged" byte must stay 0, so
   `EnemyUnitConcentration` / `EnemyForce` count fogged enemies; keep `Find_Nearest`, unit targeting, `Applies`,
   `DistanceToNearest*` and `TimeLastSeen` on real fog (force 1).
2. Check the free store: `Service_Attack` must fall back to `Attack_Move(enemy_location)` in skirmish
   (`aggressive_mode` 1), `enemy_location` must come from the hint-zone targets, and `Has_Active_Orders` must follow
   the rule above (log `enemy_location` and `Has_Active_Orders` for the stuck M1 / TIE Crawler).
3. Make sure the units are free-store units (not held by a plan that has stalled; a plan's units are not serviced).
4. Implement the firesale `Reveal_All` (Lua `FogOfWar.Reveal_All`) and the Burn plan's loop as shipped.
5. Do not add a unit-level hunt or a victory reveal: retail has neither for these units.

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
| `Project_By_Unit_Range(a, b)` (ProjectByUnitRangeClass, 0x69f6f0) | kiting position away from `a` by `a`'s weapon range: AI_TACTICAL_PERCEPTION §5 (formula); `b` through the position extractor 0x4990b0, so an **empty task force counts as (0, 0, 0)** (§7.3.2) | H |
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
- **Land skirmish purchases** become entries in the player's reinforcement pool (player +0x2c8 table), so a bought land unit is not on the map until `Reinforce` drops it (§7.4). This includes hero companies without a team (e.g. `Boba_Fett_Team_Land_MP`); SKIRMISH_SETUP §3.1 has the full chain [H]. **Space skirmish** units are built at the space station (Tactical_Multiplayer_Build_Space_Units_Generic → `Produce_Force` of station-buildable types). Station upgrades are `Produce_Force` of `*_Starbase_Upgrade` objects (AI_Plan_ExpansionGeneric_SkirmishUpgradeSpaceStation). [M]
- Unit build time for the AI = normal build time × Land/Space_Build_Time_Multiplier (§4.5).

### 7.3 Orders (all build a blocking status and return it to Lua) [H unless tagged]
| Method | Land (LandTaskForceClass vt 0x8c36a8) | Space (SpaceTaskForceClass vt 0x8c1b88) |
|---|---|---|
| `Move_To(dest [, threatTolerance [, useZonePath]])` | 0x6ec410: dest = AITargetLocation, object, task force or position. Creates LandMovementBlockStatus (0x73baf0; mode 0 move). With `useZonePath` (3rd arg, default false) and source/dest in different hint zones it routes zone by zone through the land path finder (0x73bc50); otherwise one group move to the point (0x73bf40 / 0x73c140 → formation manager 0x426010 … 0x4f8200, the same path as a player's move). Shipped plans pass only the threat number, so they always use the direct move. | 0x6e7940: SpaceMovementBlockStatus (0x738870); optional speed / space-collision-avoidance string. |
| `Attack_Move(dest [, threat])` | 0x6e9440: same, mode 1 (attack-move; object dest uses 0x73c4f0) | 0x6e4fc0 |
| `Attack_Target(unit [, …])` | 0x6e99c0: unit targets only ("use Move_To instead"); block id 0x819 | 0x6e54d0: optional hard-point type; the string `"Random"` picks a hard-point type with **Get_Int(0, 13)**, 13 meaning "any" — one synced draw |
| `Guard_Target(target [, …])` | 0x6ebdc0: mode 2; a task-force target guards **one random unit of it** chosen with **Get_Int(0, n−1)** (no draw when n = 1) | 0x6e72c0: same rule |
| `Explore_Area(AITarget)`, `Fire_Special_Weapon`, `Fire_Orbital_Bombardment`, `Bombing_Run`, `Prepare_Ambush`, `Find_Closest_Enemy` | 0x6eaba0, 0x6eb7f0 (§7.3.4), 0x6eb730, 0x6e9d90, 0x6ec8b0, 0x6ead30 — bodies not traced except `Prepare_Ambush` (§7.3.1), `Fire_Special_Weapon` (§7.3.4) and `Explore_Area` (L1_S4_REMAINING) | 0x6e66a0, 0x6e6ad0, —, —, 0x6e7e00 (§7.3.1), 0x6e68a0 |
| `Activate_Ability(name, on \| target)` | generic 0x5f9890: calls each unit's `Activate_Ability` (0x570a50), returns an AbilityBlockStatus | same |

Object-level orders from Lua (`unit.Move_To`, `Attack_Move`, `Guard_Target`, `Divert`, `Garrison`, `Leave_Garrison`, `Activate_Ability`) are the L1 objects bindings and execute directly (COMMANDS.md).

### 7.3.1 `Prepare_Ambush(target, direction, minDist, threat)` (S7 t18, 2026-11-02)

**Engine code, not Lua.** The only shipped caller is `FlankPlan.lua` (SpaceMode, FoC), FlankForce thread:
`Prepare_Ambush(AITarget, "back", flank_dist = 1000, 500)`, then `"right"`, then `"left"` when the previous call
returned nil. [H]

**Space: `SpaceTaskForceClass` 0x6e7e00 → `SpaceAmbushBlockStatus`** (vt 0x8cef58, 0x70 bytes, ctor 0x739f90, Lua id
0x81d). [H unless tagged]
- **Arguments** (exactly 4, else error "Invalid number of parameters", nil):
  1. a game object, or an AI target that represents one (AI target vtable +0x30). Dead object / AI target with no
     location → error "Target object is already dead", nil. AI target with no object (a region) → error "Target %s
     is invalid: AI targets must represent game objects", nil.
  2. a string, case-insensitive (`_stricmp`): `front` → 0, `left` → 1, `right` → 2, `back` → 3 [M: the `left` /
     `back` literals are short .rdata strings at 0x1408c26f4 / 0x1408c270c; `front` / `right` are read directly].
     Anything else → error "Unrecognized direction %s", nil.
  3. a number > 0 (minDist), else error "Parameter 3 is not a positive number", nil.
  4. a number (threat tolerance), else error, nil.
- **The return value is the block in every other case**, including when no ambush point exists: the block is then
  already finished with result false. So in space FlankPlan's `if not flank_block` fall-backs to `right` / `left`
  run **only after an argument error**; a failed `back` does not try the other sides. [H]
- **Fields:** +0x40 frame of the last plan (int), +0x44 result (byte), +0x45 finished (byte), +0x48 target object,
  +0x50 direction (ctor −1), +0x54 minDist (float), +0x58 threat (float, ctor 1e18), +0x60 moving list, +0x68 member
  list (both `MultiLinkedListClass<GameObjectClass>`, created on the first plan). [H]
- **Init 0x73a5a0** (task force must be a SpaceTaskForce and the target non-null, else nothing is set): store the
  fields, clear +0x44 / +0x45, then **plan(first = true)** (0x73b180).

**Ambush point P (0x73a140)** [H]:
1. θ = target yaw in radians (obj +0x8c degrees × 0.017453292); s = sin θ, c = cos θ (for |θ| below the tiny-angle
   threshold: s = θ, c = 1). Direction u:
   - front (0): (c, s, 0) — ahead of the target;
   - left (1): (−s, c, 0);
   - right (2): (s, −c, 0);
   - back (3): (−c, −s, −0) — behind the target;
   - any other value: no point.
2. step = the target type's `Space_FOW_Reveal_Range` (type +0x1d6c); if ≤ 0, `AI_SpaceEvaluatorRegionSize`
   (GameConstants 0x140b168d0, FoC 2000).
3. P = target position + minDist × u (all three axes).
4. Only when a space mode exists (0x28a950(GMM, 2)); otherwise no point. Self = the task force's owner player.
   Loop:
   - box = mode vf +0x1f8 (centre xyz, half extents xyz) [M: the map box]; if |P − centre| > half extent on **any**
     axis → no point (the loop ends; P never wraps back);
   - threat = enemy force of Self in the square rect (P.x − step/2, P.y − step/2, step, step): 0x654290 on the
     threat grid 0x140b31900 with category mask all (−1), ally flag 0 (enemy), A = 0, no exclusion, no faction, ×
     `AI_SpaceAreaThreatScaleFactor` (AI_TACTICAL_PERCEPTION §3.3, §4.5);
   - threat ≤ tolerance (+0x58) → **P is the ambush point**;
   - else P += step × u (walk further out along the same ray) and repeat.
   "No point" is the sentinel (1e18, 1e18, 1e18).
- So the force goes to **at least minDist from the target, on the chosen side**, further out in steps of the
  target's reveal range until the spot is safe enough. FlankPlan: 1000 behind the target's facing, then 1000 + k ×
  reveal range, threat ≤ 500.

**plan(first) (0x73b180)** [H]:
1. +0x40 = mode frame (mode +0x10). Compute P. No point → **result 0, finished 1** (no move order).
2. Coordinator system = mode vf +0x228; a new coordinator (0x426010).
3. first only (0x73a650): create both lists; for each task-force member in member order (TF +0x78 vector) whose
   object exists, whose type byte +0x1e is clear, that is **not movement-locked** (0x39ad40) and that the
   coordinator admits (0x425e30): subscribe the block to its signals 9, 10, 0xb, 0x10 and one more [L: 0x1a, the
   argument is lost in the decompile], and add it to the member list (+0x68). Then subscribe to the target's signal
   0x10.
4. For each object of the member list (list order) that the coordinator still admits: add it to the moving list
   (+0x60), make it leave every coordinator (0x426d50) and join the new one (coordinator vf +0x18).
5. No unit joined → release the coordinator (0x4260d0); the block stays **unfinished** (the next re-plan tries
   again). Otherwise `Coordinator::Move_To` (0x4f8200) with {kind 1 = position P, threat tolerance = +0x58,
   avoidance mask 0x3f, flags word 0x1000001}: a plain group move (no attack flag), the same block a TF `Move_To`
   builds (§7.3).

**IsFinished (0x73a8d0, [M] vf19)**: when not finished and mode frame − (+0x40) > **150** (0x96), **re-plan with
first = false** (new P from the target's current position and facing, re-order every member-list unit, including
those that already arrived). Returns +0x45. The re-plan therefore happens only when Lua polls `IsFinished`
(FlankPlan polls once per `Sleep(5)`), at most every 150 frames. **Result (0x73ade0, [M] vf20)** returns +0x44.

**Signals (0x73ab80)** [H]:
- 9: ignored.
- 10 (the unit's movement finished, also sent when its formation fails, MOVEMENT §4.3): remove it from the moving
  list, raise TF Lua event 0x12 `Unit_Move_Finished(unit)` (0x6006b0); moving list empty → **result 1, finished 1**.
- 0xb / 0x10 / 0x1a on an object: unsubscribe its 10 / 0xb / 0x10 / 0x1a. The target → target := null, **result 1,
  finished 1** (target death counts as success). A member → remove it from both lists; when either list is empty →
  finished 1, result = (member list not empty).
- A signal while the lists do not exist → result 0, finished 1.

**FlankPlan with these rules** (shipped Lua, [H]): FlankForce flies a plain move to P (≥ 1000 behind the target),
re-planned every ≥ 150 frames while units are still flying; when the block is finished, every 5 s: if the target is
valid and the task force is closer than 1000 to it, `Move_To(Project_By_Unit_Range(target, FlankForce))` (back out
to the target's weapon range on the far side); when MainForce raises `Target_In_Range`, `ExecuteFlank` → 
`FlankForce.Attack_Target(AITarget)`.

**Land: `LandTaskForceClass` 0x6ec8b0** [H unless tagged]: same arguments plus an optional 5th boolean (4 or 5
arguments; the 5th is `useZonePath` of `Move_To`). The same ray walk is done **once, inside the call**, with step =
`Land_FOW_Reveal_Range` × terrain factor (0x3722a0), or the max attack range (0x3749d0) when that is ≤ 0; if both are
≤ 0 there is no walk and the first point is used. A point found → an ordinary `LandMovementBlockStatus` (id 0x819,
0x73c360) moving there with the threat tolerance; nil when no point is found or the new block is already finished
(0x73cab0). No re-plan, no ambush block. Not used by shipped scripts.

**Port check (2026-11-02):** `tf_tactical_order` (`engine/ai/tactical/src/tactical_ai.cpp`) maps `PrepareAmbush` to a
`MoveTo` **onto the target position**. That is wrong: retail moves to P on the chosen side (≥ minDist away), returns
a finished-false block (not a move onto the target) when no P exists, and re-plans while unfinished.

### 7.3.2 Orders on an empty task force; `Project_By_Unit_Range` with a task force (S7 t17, RE part, 2026-11-02)

**Task-force position** (TF vf46 0x5fcc60, used by the position extractor 0x4990b0, `Move_To`, `Reinforce` and
everything that takes a "position-convertible" argument) [H]: sum of the positions of the members (TF +0x78 vector,
member order) whose object resolves (0x5791e0 ≠ null), summed as floats per axis; if n > 0, multiply each axis by
f = 1.0f / (float)n (a reciprocal multiply, not a divide). **With no live member the result is (0, 0, 0), and the
extractor still reports success.** The extractor never fails for a task force.

**`Project_By_Unit_Range(enemy, emptyTF)`** (0x69f6f0) therefore projects from (0, 0, 0): d = normalise((0,0,0) −
enemy position) (fast inverse square root), result = d × R, R = the enemy's max weapon range (AI_TACTICAL_PERCEPTION
§5); x and y are clamped into the mode box (vf +0x1f8, centre ± half extents), **z is not clamped**. It returns a
position, not nil. Only an enemy standing exactly at (0, 0, 0) gives the warning "unable to project: positions are
identical" and no value. [H]

**`SpaceTaskForceClass::Move_To`** (0x6e7940) [H]:
- No argument → error "No destination set for TaskForce %s", nil.
- Argument not an object, AI target, task force or position (Lua nil included) → error "Parameter 1 is not a valid
  destination for TaskForce %s", nil. A dead object or an AI target with no location → **nil with no message**.
- A valid destination on an **empty** task force: the `SpaceMovementBlockStatus` is created (0x738ee0 / 0x7389a0:
  the member collect finds nobody, the empty coordinator is released, the block is not marked finished) and
  returned; its `IsFinished` (0x7394f0) is **true at once** because the task force has 0 members (0x5fca10). No
  message. `BlockOnCommand(block, 5)` returns after one or two polls.
- Retail log for a nil destination is a script error with that text; the port's "invalid target parameter" text
  differs [cosmetic].

**Can retail reach Weave with an empty task force?** Yes, but only in plans that override the empty event without
exiting. [H, shipped Lua + engine]
- `No_Units_Remaining` (plan event 7) is raised synchronously from the task force's own signal handler (0x5fe5d0)
  when a member dies or is removed (signals 0x10 / 0x1a) and no remaining entry of the task force's entry vector
  (TF +0x60; each entry's type via entry +0x28) has a type with bytes +0x13, +0x14, +0x1e clear and type bit 0x37
  clear [M: meaning of those type flags]; also from `Release_Unit` (0x5ff2d0) and the production-failed path when
  that vector becomes empty. It is queued to the plan and dispatched in `PumpEvents` right after the next yield,
  before the thread runs any further Lua.
- `Default_No_Units_Remaining` (PGEvents.lua) calls `ScriptExit()`, which sets the C exit flag and yields false
  from the pumping thread, so a plan with the default handler (EscortPlan, FlankPlan, …) ends **before** `Weave`
  continues to line 218.
- `MoveToLocationRush.lua`, `HideSurpriseUnits.lua` and `HideTransports.lua` override it with
  `EscortForce_No_Units_Remaining() EscortAlive = false end`. Their EscortForce thread runs
  `while EscortAlive do Escort(EscortForce, MainForce) end`, so when the last escort dies during the first
  `BlockOnCommand(tf.Attack_Target(enemy), 10)` of `Weave`, the handler clears the flag and the thread continues:
  line 218 runs `Move_To(Project_By_Unit_Range(enemy, EscortForce))` on the empty task force (a position near the
  map origin, see above; an immediately finished block), then the remaining `Attack_Target` / `Move_To` of `Weave`
  and the `Escort` tail run the same way, and the loop ends because `EscortAlive` is false.
- The escort team is `"Fighter | Corvette | Frigate = 0,4"`. Whether a plan can start with 0 escorts (and so never
  raise event 7) depends on the plan solve [open].

**Port check (2026-11-02):** `arg_position` (`tactical_ai.cpp`) returns false for a task force with no live unit, so
`Project_By_Unit_Range` returns nil and `Move_To(nil)` fails. Retail returns (0, 0, 0) for the task force. The port
also posts `No_Units_Remaining` from a per-service poll (`service_statuses`) and only when `pending` is empty, while
retail posts it from the death / removal signal (before the plan runs again).

### 7.3.3 `Activate_Ability` blocks, signals 0x29 / 0xb / 0x10, DEPLOY, `No_Units_Remaining` (S7 t26 and t27, RE, 2026-11-02)

**1. When the Lua call returns a block** (object `GameObjectWrapper.Activate_Ability` 0x570580 → 0x570a50) [H]:
- 0x570580 makes an `AbilityBlockStatus` (Lua id 0x825, pending count +0x40) **only when 0x570a50 returns true**;
  otherwise it returns **nil**.
- 0x570a50 returns false (so nil, no block) for every gate refusal: no object, argument count ≠ 2, name not a
  string / unknown, the unit lacks the ability (0x3986b0), not ready (0x39bbd0), `Can_Activate` (0x39b480(obj, t, 1))
  fails, wrong argument type, "not hooked up to special ability logic", dead target object. Of these, the count,
  name, argument-type and "not hooked up" refusals are script errors that also end the script (cu8k t10,
  CU8K_T04_COORDINATOR_TRACES "Which check ends the script"); a dead target object is a warning.
- After the gates it builds and executes the command at once. Its return value then depends on the ability family
  (case lists from the second switch, decimal ids of ABILITIES_HARDPOINTS §2.1):

| ids | command | returns a block? |
|---|---|---|
| 2 DEPLOY | 43 (mode toggle), only when the bool argument ≠ the current deployed state (0x39b950(obj, 2)); equal → nil | **always** once executed, whether or not the deploy starts |
| 13 15 16 18 20 21 22 27 30 31 34 37 41 45 51 53 55 58 59 64 66 (object target) | 40 (`TacticalSpecialAbility`, 0x429da0 / 0x429960) | only when, after the execute, the unit's pending-ability name CRC (CD pack obj +0xf0, +0x150) equals the ability's `GUI_Activated_Ability_Name` CRC (0x371ba0), i.e. `Issue_Auto_Target` accepted and stored it |
| 17 JET_PACK (position) | 43 with the position | only when the jump started: the unit has a locomotor and its vf +0x300 is false (= `Is_Unit_Ability_Active(17)`, 0x39b950) |
| 50 67 71 73 74 (position) | 57-style: spawns `Dummy_Barrage_Target` at the position (0x29f810), then 0x429da0 / 0x429960 | **always** once executed |
| 1 3 7 9 11 29 32 33 39 40 43 48 52 54 56 60 62 63 65 68 69 70 (none / bool) | 43 | **never** (nil even on success) |
| 14 23 24 25 35 36 38 44 46 49 57 61 72 | 43 with **no** position (zeroed) | never |
| 5 26 28 42 (position), 6 10 12 (a bool, value ignored; corrected cu8k t10), 19 (object) | 43 | never |

  Correction for ABILITIES_HARDPOINTS §2.9 (applied there 2026-11-02): the "target position (command 57,
  `Dummy_Barrage_Target`)" row is ids 50, 67, 71, 73, 74, not 14, 23, … 72; those take command 43 with no
  position.
- Before executing, ability ids 2 3 5 6 13 15 16 17 18 20 21 22 27 31 32 34 37 38 39 41 45 46 50 51 52 53 55 58 59
  64 66 67 69 71 73 74 raise **signal 0xb on the unit and on each member of its TEAM** (0x220ed0). The block subscribes
  afterwards, so it never sees this 0xb.
- **TaskForce `Activate_Ability`** (0x5f9890) calls 0x570a50 for each member (TF +0x78 order); a true result adds
  that unit to the one block (count + 1, subscribe); a false result queues plan event 0x13
  `Unit_Ability_Finished(unit)` at once. With no true result the block's count is 0 and `IsFinished` (0x57bab0,
  count == 0) is true on the first poll. So a TF block on an "always nil" family finishes at once and every member
  gets `Unit_Ability_Finished`.

**2. What finishes a block** [H]. 0x6ae570 subscribes the block's listener (+0x30) to the unit's signals **0x29, 0xb
and 0x10**. The listener (0x6ae4f0) does not look at the signal number: the first of the three on that unit
decrements the count and unsubscribes all three. There is **no timeout** in the block (only `BlockOnCommand`'s
optional `max_duration`).
- **0x29** = "unit ability finished" (MOVEMENT §3.1.1). It is raised on the unit by any ability end, so an unrelated
  ability's expiry (countdown 0x42f460, every ability id) or a stealth break (0x592d00 / 0x5931b0) also finishes it.
  Raisers: 0x3a9910 (jetpack swap), 0x42f460, 0x592d00, 0x5931b0, 0x5b07c0, **0x61e930 (Walk locomotor deploy /
  undeploy, below)**, **0x61d300**, 0x63fae0, 0x63ffd0, 0x640060, 0x6cc8d0 and the special-ability removes
  0x6f2c20 … 0x71bcc0.
- **0xb** = "a new order replaces this unit's order" (A1_READINGS §33.3 lists the raisers: task-force movement
  collectors, unit `Move_To` / `Attack_Move`, `Activate_Ability` itself, `Turn_To_Face`, MoveToObject, garrison
  enter / exit, ConcentrateFire, …). Auto targeting does not raise it.
- **0x10** = destroyed / removed: raised only by `Detach` 0x3a8380 (death and every `Destroy_Object`) and on a fresh
  death clone (0x29f270). It is not owner change (0x1a) and not the store removal (0x28).

So a block that the ability **refused after the gates** (only possible for DEPLOY and ids 50 67 71 73 74, which return
a block regardless) **stays pending until the unit gets a new order (0xb), any ability on it ends (0x29) or it
dies (0x10)**. An object-target block whose pending ability never fires (target died before the unit got in range)
likewise waits for one of the three [M: the pending-ability clear path on target death was not traced].

**3. DEPLOY and 0x29** [H]. Only the Walk locomotor implements deploy (locomotor vf +0x118 = 0x61d300 in
`WalkLocomotorBehaviorClass`; every other locomotor class has the empty 0x1fa90). FoC deployers (the three Mobile
Defense Units, `Deploys` yes, `WALK_LOCOMOTOR`) therefore:
- `Set_Unit_Ability_Active(2, on)` → 0x3a1ce0: refuses (no deploy request, no signal) when the unit has a
  garrison / +0x3a7 / transport, a running ability countdown, a member that cannot activate, or its locomotor's
  vf +0x2b0 is true [M: "moving"]. Otherwise it drops the target (TARGETING vf +0xd8(1)), leaves the coordinator and
  calls 0x391260 → Walk vf +0x118(1).
- 0x61d300(1): pack state 0x18/0x19 → nothing; obj +0x3a1 bit 0x40 or pack +0x2b3 set → nothing; otherwise state
  **0x18** and play animation 25 (`Deployment_Anim_Rate`). From state 0x1a (undeploying) with the turret test
  failing it goes straight to state 0x19 and **raises 0x29 at once**.
- Walk vf6 0x61e930, state 0x18: when animation 25 has finished (0x39a180), state **0x19 (deployed), raise 0x29**,
  then 0x39fec0. State 0x1a: when animation 26 has finished, state 0 and **raise 0x29**.
- So `BuildMDUItem`'s `BlockOnCommand(MainForce.Activate_Ability("DEPLOY", true))` (no timeout) **returns when the
  deploy animation ends**. It waits forever only when the deploy was refused (0x3a1ce0 / 0x61d300 gates) and nothing
  else orders, kills or ends an ability on the MDU. A non-walking unit with DEPLOY would always wait (none in FoC).

**4. `No_Units_Remaining` entry flags** (TF signal handler 0x5fe5d0, signals 0x10 / 0x1a) [H]:
- First the unit is removed from the task force (vf +0x188 → base 0x5ff610: unsubscribe, drop it from the member
  vector +0x78; if it was a member, erase the **first entry of TF +0x60 whose type equals the unit's company type**
  (0x3973b0: the type of its transport / company / team container, else its own type) and the parallel int at +0x90).
  A removal raises plan event 2.
- The entry vector TF +0x60 is filled when the task force is made from the plan solve (0x66fb30 → 0x5f9fd0, one
  entry {type wrapper, int} per planned unit of that team) and by the reinforcement drops (0x6ee010, 0x6ed9f0). It
  therefore also holds **planned units that have not arrived yet**.
- Then event 7 `No_Units_Remaining` is queued unless some remaining entry's type (entry +0x28) has all of:
  `Is_Named_Hero` (type +0x13) false, `Is_Generic_Hero` (+0x14) false, `Is_Special_Weapon_In_Space` (+0x1e) false, and
  no `LOBBING_SUPERWEAPON` (id 0x37) in the **Land** behaviour bitset (0x370320(type, 0x37, 1) reads type +0x760).
  An empty vector raises it too. The test runs on every 0x10 / 0x1a of a member, not only when the removed unit was
  counted.
- For the implementer: count "real" entries = planned-or-present unit types that are not heroes, not space special
  weapons and not lobbing superweapons; raise the event from the death / removal handler when that count is 0.

**5. Land `Prepare_Ambush` step and threat (S7 t27)** [H]:
- 0x3722a0(type, obj) is **not a terrain factor**: it is the land sight range = `Land_FOW_Reveal_Range` (type
  +0x1d68) × the weather `Sight_Range_Modifier` for the type's `Weather_Category` (0x468510 on the weather system
  0x140b30b40, FOG_OF_WAR §3.1.1; 1.0 when there is no strong weather or no entry). In 0x6ec8b0 it is called with
  the **target's** company type (0x3973b0) and the target object (its owner decides the weather exemption). Step ≤ 0
  → the target type's base max attack range (0x3749d0).
- Step ≤ 0 after both: one `LandMovementBlockStatus` to the first point (target + minDist × u) with **no threat and
  no map-box test**, returned even if it is already finished. Otherwise the ray walk: leave when P is outside the
  mode box (vf +0x1f8; returns nil); threat = 0x654290 on the threat grid with rect (P.x − step/2, P.y − step/2,
  step, step), all categories, enemies of the task force's player (0x5f7020(TF +0xf0)), **no
  `AI_SpaceAreaThreatScaleFactor`** [M: the space walk multiplies by it, the land call passes none]; threat ≤
  tolerance → block; else P += step × u.
- **The threat tolerance is passed to the group move** in both modes: land 0x73c360 stores it at block +0x50 and
  0x73bf40 writes it to the move parameter +0x54 when ≠ 1e18 (plus flags +0x48 = block +0x88, 0 here); space 0x73b180
  writes the ambush block's +0x58 to parameter +0x54. Parameter +0x54 is the order threshold T of the space threat
  path mode (REMAINING_GAPS §4.2: honoured only for `Space_Layer` 0 units) and the land zone-path threat tolerance
  (A1_READINGS §3).
- For the implementer: replace the port's terrain factor 1.0 by Land_FOW_Reveal_Range × weather sight modifier of the
  target type; pass the tolerance as the move's threat threshold.

**6. Family dispatch the port needs (RE re-check of 0x570a50, 2026-11-02)** [H]. Argument rules: ABILITIES_HARDPOINTS
§2.9 "Argument handling per family".

| ids | argument | command | block |
|---|---|---|---|
| 50 67 71 73 74 | position (0x4990b0; [M] an object gives its position) | `Dummy_Barrage_Target` at the point, then the command-40 path with the dummy as target | always |
| 14 23 24 25 35 36 38 44 46 49 57 61 72 | ignored | 43, mode **on**, no position, no target | never |
| 5 26 28 42 | position | 43, mode on, position | never |
| 17 JET_PACK | position | 43, mode on, position (no dummy) | when the jump started |
| 19 TARGETED_INVULNERABILITY | object | 43, mode on, the object's position + id (not command 40) | never |
| 13 15 … 66 (object list) | object | 40 | when the pending ability was stored |
| toggle list, 2 DEPLOY | bool | 43, mode = the bool | DEPLOY always; others never |

The port's `engine/lua/src/bind_objects.cpp` `ability_arg_family` (read 2026-11-02) still uses the old §2.9 lists.
Its block rule `ability_block_rule` already matches. What to change:
- **Position family:** {14, 17, 23, …, 72} → **{50, 67, 71, 73, 74}**. Today these five fall into Toggle and never
  spawn the dummy.
- **14, 23, … 72:** a toggle-on that ignores the argument. Today they need a point and then take the dummy path.
- **17:** command 43 with the position.
- **19:** leave the object family. Use a mode-on with the target's position and id instead.

Nothing else in the dispatch is unclear. The open item is who removes the dummy of ids 50 / 67 / 71 / 73 / 74
[L]. The port's `dummy_lifetime` 900 frames is a [port] stand-in.

**In-game probe of the DEPLOY block (s78f t01, 2026-11-05)** [H for what was seen]. Source:
`tools/probe_mod/s7s8_probes/out/20261105-051443` and `-055142` (SPMAT). An AI-owned `SPMAT_Walker` on Geonosis was
driven from the free-store script:
- `Activate_Ability("DEPLOY", true)` on an undeployed walker returned a block, which finished **45 frames** later.
- In memory the locomotor went 0x18 at once (clip 25 = Deploy, at rate 2.0) and 0x19 45 frames later, the frame
  the block finished.
- `("DEPLOY", false)` from deployed returned a block that finished after **240 frames**. In the first run the
  locomotor stayed at 0x19 for most of that time.
- `("DEPLOY", true)` again right after (state already wanted) returned **nil** (no block), as the "equal → nil" row
  above says.
- `("DEPLOY", false)` 12 frames later returned a block that finished in **12 frames**.
- An `Attack_Target` on a deployable SPMAT (`Auto_Deploys`) started the Deploy clip 3 frames after the order. It
  reached 0x19 and targeting state 3 at +48, state 4 and the first grenade at +66 (LOB_SOLVER §9).

The free-store script stopped producing records at the AT-AT scenario's first DEPLOY in both land runs (`ret=nil`,
then silence for the rest of the battle). [M] Something in that scenario blocks the free-store thread for good,
not an error: the event handlers run in pcall and recorded nothing. Run the AT-AT scenario last.
Third run (s78f t12, 2026-11-05, `out/20261105-062322`, AT-AT last) [M]. `("DEPLOY", true)` on the AT-AT returned
nil (ready 0 → 0, not active). The script then falls through to the `DEPLOY_TROOPERS` fallback, and the thread went
silent for good while the game ran on for about 9600 frames. No later record arrived, not even the ACTC record
written between `Is_Ability_Ready("DEPLOY_TROOPERS")` and the no-argument `Activate_Ability("DEPLOY_TROOPERS")`. So
the block is in one of those two calls on an AT-AT that has not deployed. The reader also missed the ACTC of the
first DEPLOY call in this run, so a lost record cannot be ruled out. Either way, the original's free-store thread
never resumes after that call.

### 7.3.4 `LandTaskForce.Fire_Special_Weapon(type_name, target)` with a position target (s78f t04, 2026-11-03)

Body 0x6eb7f0 [H unless tagged]. It extends SCRIPT_GAPS §1.4. It returns no block status.

**Arguments.** Exactly 2, else the script error "invalid number of parameters: expected 2 got %d", which returns
nothing (nil).
1. **type_name:** a Lua string (else error "invalid type for parameter 1. Expected string"). It is looked up by name
   (0x407c00); the type must have LOBBING_SUPERWEAPON (0x37) in its **Land** behaviour set (0x370320(type, 0x37, 1)),
   else error "%s is not a land special weapon!".
2. **target:** exactly one of two Lua classes:
   - **AITargetLocationWrapper** (class tag 0x140a44280). Its location (wrapper +0x38) null → warning "target is
     already dead.  Aborting.", nil. Otherwise point = the location's position (location vf +0x68). For a **region
     target** (`TacticalRegionAITargetLocationClass`, 0x7254e0) that is the stored centre at +0x1c, i.e. the hint
     zone's centre (§3.2). The location is **not** asked for a game object, so **a region target with no object is
     accepted**. For a unit target it is the unit target's position getter [M: the unit class's vf +0x68 was not
     located; expected the object's position].
   - **GameObjectWrapper** (0x140a43b18). Object (wrapper +0x60) null → the same warning, nil. Otherwise point = the
     object's raw position obj +0x78 (no `Ranged_Target_Z_Adjust`).
   - Anything else, **a plain position value included** (the position class 0x140a44290 that 0x4990b0 accepts
     elsewhere is not tested here), → error "Invalid type for parameter 1.  Expected game object or target location"
     (the message says parameter 1), nil.

**Dispatch.**
- Walk the task-force member list (TF +0x78, wrappers → objects, list order).
- Take the **first** unit whose representative type (0x3973b0) is the named type, that has the LOBBING_SUPERWEAPON
  behaviour, and whose readiness (0x4914e0 on obj +0x158) is ≥ 1.0.
- `Fire_Lobbing_At_Position` 0x4ea3f0(that behaviour, point). The point is stored as given, z included, with no
  ground snap. 0x4ea3f0 returns nothing and can silently do nothing: no tactical player data for the owner, or
  `Requires_Base_Power` without base power (0x527520). The caller does not check that.
- Then the destroy-after sweep: every object of behaviour 0x67 owned by the unit's owner is killed (0x391310(obj, 1))
  in the list order of 0x2a9ff0.
- Return the unit's GameObjectWrapper (0x5751d0 with the task force's player). Later matching units are **not**
  tried, even when the order was silently refused.
- No ready matching unit → returns **false** (a `LuaValue<bool>`).

**What happens next** (TACTICAL_COMBAT §3.1.1 "Fire-at-position order"; LOB_SOLVER §3.6 "Charge-up aim"): the
turret slews to the point, the 90-frame charge-up runs, and the WEAPON service fires a ground-point shot (no target
object, no lead, no bone pick). A region centre whose z is not the ground height is still the solver's target
point; the grenade detonates on the terrain hit (LOB_SOLVER §6) [M: the hint-zone centre's z].

**Port** (engine/lua/src/bind_script_gaps.cpp `task_force_fire_special_weapon`, read 2026-11-03). Four differences
from retail:
- it errors "invalid target" for an AI target without an object; retail fires at the region's position;
- it compares `st->type(u)` with the type; retail compares the representative type (0x3973b0);
- when the fire hook refuses, it tries the next unit; retail returns the first ready unit whatever 0x4ea3f0 did;
- its error texts differ slightly.

### 7.4 Reinforcement drops (`TF.Reinforce(target [, maxWait])`) [H unless tagged]
- **Land** (0x6ed5f0): requires the TF to have pending reinforcement types (else "taskforce has no reinforcements", nil). If the player currently cannot reinforce (0x2b03e0 false) it returns an immediately-failing ReinforceBlockStatus (id 0x81a). The target may be an object, an AITargetLocation or a task force (its position). Then:
  - **campaign battle** (galactic parent) 0x6ed9f0: draws the units from the player's galactic reinforcement pool (persistent units in orbit, 0x28aaa0 → +0x40);
  - **skirmish** 0x6ee010: draws from the player's tactical reinforcement pool (player +0x2c8 by type).
  - Both compute landing positions near the target (0x6eaf60, which honours reinforcement-point zones and `maxWait`), then, **from the last pending unit to the first**, spawn each unit whose population cost (0x373500) fits the remaining cap (0x2acb10), subscribing the block status to its arrival signal 0x19, and add the spawned unit (or its team members / ceil(members/teamSize) copies for `Create_Team` types) to the task force. [H order; M position rules]
- **Space** (0x6e8620) [H unless tagged; refixf t06]: the target is **not** a drop point that must be valid. The engine searches rings around it, one attempt per Lua poll, until a point passes the space drop test.
  - Arguments: a target convertible to a position (object, AITargetLocation, task force, position; 0x4990b0), optional max wait in seconds. No pending reinforcements for the TF → "taskforce %s has no reinforcements", nil.
  - Skirmish (0x28afb0 true) only (corrected q6 t18: earlier text said "campaign battle"; 0x6e8620 applies the rule when 0x28afb0 returns true, and 0x28afb0 is the skirmish test, SKIRMISH_SETUP §0): unless the flag at 0x140b155c4 is set [L: debug override], the AI player needs a live allied (0x39a2c0) `DUMMY_STAR_BASE` (behaviour 0x5c) object, else "Rule Change: AI players cannot reinforce after their starbase has been destroyed", nil. The pending ids are mapped to galactic objects (+0x11c).
  - Returns a `SpaceReinforceBlockStatus` (ctor 0x722d50, Lua id 0x81f; init 0x7235c0): pending list copy (+0x50..+0x58), target T (+0x68; **z := 0**), timeout frame = now + int(FPS × maxWait) when given (+0x74, else −1), player (+0x80), validate flag +0x84 = 1, ring radius R (+0x7c) = **1000** when T lies inside a non-allied object's `Reinforcement_Prevention_Radius` (0x4d8d10: GOM list +0x2f0, owner not allied (0x39a230), 2-D distance² < radius², radius read as float at type +0x1df8), else **0**.
  - **Placement** runs inside `IsFinished` (vf19 0x7236f0 → 0x722e80), i.e. once per poll; `BlockOnCommand` polls once per script service (`PumpEvents`). Per call, space mode only:
    1. Take the **last** pending entry and resolve its type (0x2acfb0); none → pop it, stop for this call.
    2. With +0x84 set, the player needs population room for that type (0x2b03e0); no room → nothing this call (R unchanged).
    3. θ = the player's reinforcement entry facing in degrees (mode +0x288, 12-byte records indexed by player +0x48, field +8). Start direction u = (−cos θ, −sin θ).
    4. Up to 10 tries: P = T + R·u, x and y each clamped into the playable box (mode vf 0x208: centre ± half extents), z = 0. Test `SpaceMode` vf80 0x4d6fd0(player, P, type, force 0) (TACTICAL_GAPS §4.2: rejected when P is fogged for the player (0x35f3b0, force 1, so the AI's own fog counts), inside a non-allied prevention radius, or outside the playable box; then the occupancy sweep from P − `Space_Reinforcement_Collision_Check_Distance` (FoC 200) × (cos θ, sin θ) to P over frames [now, now + 115] on the type's `Space_Layer` (default 4) and layer 8). Pass → spawn. With R = 0 only this one try is made. Otherwise rotate u by **+36°** (x' = x·cos36 − y·sin36, y' = x·sin36 + y·cos36) and retry.
    5. All tries failed → R += **500** (the next poll tries the next ring).
    6. Spawn: 0x2bf2d0(GOM, unit, P, player) (hyperspace arrival), subscribe to its arrival signal 0x19, +0x78 += 1, pop the entry, then reset R to 1000 (T inside a prevention radius) or 0. **At most one unit per call.**
  - Arrival (signal 0x19, 0x723ac0): +0x78 −= 1, and the unit joins the TF (TF Lua event).
  - `IsFinished` = timed out (0 ≤ +0x74 ≤ now) **or** (no pending entry left and +0x78 < 1). `Result` (vf20 0x723d00) = true when every pending entry was placed.
  - Lua side: `QuickReinforce` (PGTaskForce.lua) in space calls `BlockOnCommand(tf.Reinforce(tf), 3, Anti_Idle_Taskforce)` (then the escort TF, then fall-backs), so each attempt has 3 s of polls: rings 0, 500, 1000, … around the target until a ring point is visible to the AI, outside enemy prevention radii and free of reservations.
  - **Why a port rejects many AI space drops:** issuing the drop exactly at T (the land model) fails whenever T is fogged for the AI, inside an enemy station's or other non-allied object's `Reinforcement_Prevention_Radius`, outside the playable box, or on reserved space. Vanilla never issues an invalid drop: it tries the ring points in order and only spawns at a point that passes vf80. Space has no reinforcement points (`Use_Reinforcement_Points` is the land rule).
- `QuickReinforce` (Lua) is the only AI caller pattern; there is no separate engine "AI reinforce" logic.

### 7.5 Building structures (land) [H]
- Build plans name under-construction types (`UC_*`) in their task force; the PotentialPlan's TacticalBuild marshaller reserves (pad, type) pairs (AI_GALACTIC §1.9.3).
- `Build_All()` (0x6ea620) / `Build(typeName [, pad])` (0x6ea030): for each reservation, refuse with a log if the pad's owner is no longer allied with the AI player (0x39a2c0, relation 0; "location is no longer friendly"; there is no visibility test here, corrected q6 t19), if game rules block it ("pad may be occupied or enemies may be too close", 0x42e400), if the rebuild cooldown after a destroyed building is active (0x42daf0), or if the (type, pad) pair is not one of the plan's reservations (0x6c1e80); otherwise issue the pad build through the player's build path 0x42edf0 (for an AI player it builds at once **without a credit charge**, §7.5.1) and return a TacticalBuildBlockStatus (id 0x81e). If no under-construction object appeared, signal 0x22 is raised on the pad and the build cost is credited back to the player (0x27f370(player, +cost, 0)) [H code; M why a refund is due]. `Get_Reserved_Build_Pads` returns the reserved pads.
- `Sell_Tactical_Structure`, `Fund_Tactical_Structures`, `Purchase_*_Upgrades` are plans over the same primitives.

#### 7.5.1 Build pads: who may build, who owns the result, how pad ownership changes (q6 t19 RE, 2026-11-04) [H unless tagged]

Sources: 0x42edf0 (player build), 0x42dbd0 (start build), 0x42e890 (completion), 0x42e6d0 (child died), 0x42ecd0
(child sold), 0x42e400 (AI / Lua rule), 0x5739c0 (Lua `pad.Build`), 0x6ea030 (TF `Build`), 0x4d29a0 / 0x4d3830
(AI pad tables), 0x500a00 (capture controller), 0x39a230 / 0x39a2c0 / 0x2823e0 (alliance), TACTICAL_COMBAT §2.10
(relation table). Rules already in L1_S4_REMAINING §3.6 / §4.2 and REMAINING_GAPS §6.5 are not repeated.

**Alliance tests.** 0x39a230(obj, playerId) and 0x39a2c0(obj, player) both read the object's owner (planets and
capture points their controller) and return relation(player → owner) == 0. Relation 2 (neutral) is **not** allied.
In skirmish the Neutral player's own entry is 2 as well (TACTICAL_COMBAT §2.10), so the Neutral player is not even
allied with itself there; in campaign the self entry is 0.

**Can anyone build on a neutral, uncaptured pad? No.**
- Player build (command 33 → 0x42edf0, humans and AI): after the "UC present", "built present" and "pad under the
  player's FOW" refusals, 0x39a230(pad, player) must be true, else the log "TacticalBuild: Player %d cannot build on
  %s(%d) because it is not owned by an ally (current owner id=%d)" and the local error SFX. A neutral pad fails.
- Start build 0x42dbd0 (every path ends here) repeats the test with 0x39a2c0(pad, builder) and returns false.
- AI: the TacticalBuild marshaller takes its pads only from the free store's pad table (free store +0xe0 map, listed
  through +0xe8 by 0x4d3830, AI_GALACTIC_3 §1.7). 0x4d29a0 enters a pad there only when its owner is allied with
  the AI player (0x39a2c0) and subscribes to the pad's 0x10 / 0x1a signals, so **a neutral pad is never reserved**.
  TF `Build` re-tests 0x39a2c0 before building. To build on a neutral pad the AI must capture it first (the
  `Secure_Build_Pad` / `SecureBuildPad` plans stand units on it, A1_READINGS §33.3).
- Lua `pad.Build(type [, true])` (0x5739c0) builds for the **pad owner** (0x42dbd0(…, pad owner id, charge 0, sfx 0)).
  In skirmish a Neutral-owned pad fails (Neutral is not allied with itself) and the call raises "unknown error
  starting build". In campaign the self relation is 0, so a story script can build on a Neutral pad and the Neutral
  player owns the result [H from the relation rules; no shipped script found doing it].

**Who owns the result.** The under-construction object is created for the **builder** (0x29f810 owner = the builder
id; pad pack +0x20 := builder id), and the finished object for the id stored in +0x20 (then +0x20 := −1). The pad's
own owner does not change at build start or completion (0x3912c0, called at build start, only clears a local-player
UI flag). An allied player can therefore build on its ally's pad and own the structure while the pad stays the
ally's.

**Rule 0x42e400 (AI and Lua `Build(…, true)` only; the player build 0x42edf0 does not test nearby units).** A nearby
object blocks when it is alive, has `Influences_Capture_Point`, has neither BASE_SHIELD (slot 0x39) nor
DUMMY_GROUND_STRUCTURE (slot 0x5F) and its owner is **not allied** with the pad owner (0x39a230 false). Relation-2
objects (the Neutral player's, map props owned by Neutral) therefore block exactly like enemies; there is no neutral
exemption [H]. (Corrects the port note in SKIRMISH_SETUP "Port status", which exempts a Neutral owner [M].)

**How pad ownership changes during a battle.**
1. A pad is a CAPTURE_POINT (L1_S4_REMAINING §4.2). Its controller each service (0x500a00) is the pad owner itself
   whenever the pad pack holds an under-construction **or** a finished object (rule a). So **an occupied pad never
   changes owner by capture**, whoever stands on it, and an under-construction pad is as safe as a built one.
2. An empty pad follows the capture rules: a Neutral-owned pad goes to the first in-radius candidate's owner after one
   transition; an owned empty pad with an enemy goes owner → neutral → capturer (two transitions).
3. **Child dies** (0x42e6d0, from the death path 0x39bdb0): the pad slot is cleared (the lock +0x2c too when it was
   the finished object; galactic persistence removed). Then:
   - pad type `Destroy_When_Child_Dies` (+0x99): the pad itself is killed;
   - otherwise 0x501050(capture behaviour, Neutral id, −1): **the pad's owner becomes Neutral at once** (no
     transition time), the capture record's pending controller := Neutral, skin and reveal refreshed; then the
     `Minimum_Time_Before_Pad_Can_Build_Again` cooldown (+0xa0c > 0) starts; pad anim 0x75.
4. **Child sold** (0x42ecd0, from `TacticalSellBehaviorClass` 0x5248b0): the slot is cleared and persistence removed,
   but **no ownership reset and no cooldown**: the seller keeps the empty pad until someone captures it by rule 2.
5. **Does a pad that turns neutral take its structure with it? No.** The structure is owned by its builder, not
   through the pad. Owner changes are per object (0x38e5a0 walks only the object's own behaviours and, for a team,
   its members / container); the pad's behaviour pack has no owner-change handler that touches its child [H for the
   pad functions read; M that no other listener does]. In practice the pad cannot turn neutral while it has a child
   (rule 1), and it turns neutral exactly when the child is gone (item 3). A structure whose owner changes by other
   means (Lua `Change_Owner`) leaves the pad's owner unchanged.

**Port notes**
- Player build, TF `Build` / `Build_All`, the TacticalBuild pad list and Lua `pad.Build` must all require
  relation(builder → pad owner) == 0; a Neutral-owned pad is never buildable in skirmish. The AI's open-pad list holds
  only allied-owned pads (re-enter on the pad's owner-change signal).
- Create the UC and the finished object for the builder id, never for the pad owner; keep the pad owner unchanged.
- Capture controller: an occupied pad (UC or finished) keeps its owner. On child death set the pad owner to Neutral
  immediately (or kill the pad with `Destroy_When_Child_Dies`) and start the cooldown; on sale keep the owner and start
  no cooldown.
- 0x42e400: do not exempt Neutral-owned objects; only allied (relation 0) objects are ignored. Human builds skip the
  proximity test.
- Iterate the AI pad table in a defined order (it is a hash map in the original, as the free store, §5.3).
- Port status (q6 t19, 2026-11-04): done in `tactical_battle_pads.cpp` / `tactical_battle_bt.cpp` / `battle_ai.cpp` /
  `engine/tactical/garrison` (capture rule a): builds need relation 0 to the pad owner (`bt_allied`, so a Neutral pad
  refuses every build in skirmish), the UC and the result belong to the builder, an occupied pad keeps its owner,
  child death resets the pad (or kills it), a sale keeps the owner and starts no cooldown; the AI pad list is in
  ascending id order. 0x42e400 has no Neutral exemption, but its candidates are the battle's live units only [port,
  open]: Neutral map props (`Prop_Urban_HalfWall_B`, `Prop_Bushes_Tan04`, `Influences_Capture_Point` unset = true)
  stand within the radius of three Utapau pads, and counting them stopped every AI build there, so the vanilla land
  skirmish no longer ended. Whether retail's query reaches props is not read.

### 7.6 Garrisons [H, Lua]
`Try_Garrison` (PGCommands): nearest own `GarrisonCanFire` container within range that `Can_Garrison(unit)`, else (if not offensive-only) any `CanContainGarrison`; `SPREAD_OUT` off, `unit.Garrison(container)`, and if the unit was in a TF, `Release_Unit` + `Lock_Current_Orders`. `Try_Deploy_Garrison` ejects occupants above a health threshold that are good against the target. `Garrison_Bunker` / `Garrison_Transport` goals run the GarrisonTarget plan (move next to the target, guard until it is ours/empty, then `Garrison`). `TaskForce.Can_Garrison` / `Garrison` (0x5fbcb0 / 0x5fc440) are the TF-wide versions.

### 7.7 Base-attack follow-through: Destroy_Structure sizing, release, re-tasking, and how a land skirmish ends (q6 RE, 2026-11-04) [H unless tagged]

Port symptom: in a TR land skirmish (`_MP_LAND_BLOOD_GULCH`, Empire AI vs Rebel AI) the Empire kills everything but
`Rebel_Command_Center` (and a barracks). The CC self-repairs (TACTICAL_GAPS §2.3.1a: it heals under fire), the
Empire's `Destroy_Structure` task forces have 1-5 units and get released, and for 13 minutes no Empire unit came
within 2884 of the CC. Vanilla FoC land skirmish in the port also had no winner at 40000 ticks. This section adds what
the docs lacked. Sources: 0x6c5de0, 0x6c47c0, 0x5f1d00, 0x5f2820, 0x5b43d0 (token names), 0x72ac70, 0x6bff00,
0x73ce80, 0x6e9440; FoC and TR `DestroyStructure.lua`, `PGTaskForce.lua`, `PGEvents.lua`, `BurnUnitsLand.lua`,
FoC `BusyTacticalFreeStore.lua`, TR `LandTacticalFreeStore.lua`, `LandGoals.xml`, `BasicLandEquations.xml`,
`AI_Equations_Skirmish.xml`, the structure XML.

#### 7.7.1 The tactical contrast lists (0x6c5de0 / 0x6c47c0 → 0x5f1d00 → 0x5f2820) [H]

AI_GALACTIC §1.9.3 and AI_GALACTIC_2 §1.4 "Contrast" describe the galactic lists (0x5f1990). Tactical modes (PP
+0x198 ≠ 0) build them differently:
- **No target, or `IgnoreTarget`:** both lists empty, tolerance 0.
- **Tolerance** T = 1 − (f·PFCA + `MinContrastScale`)/(f·PFCA + `MaxContrastScale`), f = recent failures of this
  plan at this target (as galactic).
- **Main list** (0x5f1d00 with flag 0) and, only when some TF of the plan is an `EscortForce`, an **escort list**
  (flag 1). Layer byte of every entry = (mode == land), so a land list is all "ground".
  - entry 0: (no type, `S(target, all categories)`), **not scaled**;
  - then one row per `EnemyContrastTypes` key of the plan (plan +0x68, map order): `S(target, that key's category)` ×
    (`MaxContrastScale` + f·PFCA) × `Land_AI_Contrast_Multiplier` (difficulty +0x3c; space +0x38);
  - when entry 0 is ≤ 0, every typed row is 0.
- **S(target, mask)** (0x5f2820) evaluates one token path in the AI's perception system for the current mode, with
  `Parameter_Category` = mask when a mask is given (no attenuator, so full power regardless of health). The path
  (token ids from 0x5b43d0) depends on the target and the flag:

  | Target | Main list (flag 0) | Escort list (flag 1) |
  |---|---|---|
  | unit target (has an object) | `Target.ForceUnnormalized`: the target object's own `AI_Combat_Power` if its category matches (AI_TACTICAL_PERCEPTION §3.1) | `Target.Location.EnemyForceUnnormalized`: the enemies of Self around the target, the target itself excluded (§3.3 there) |
  | region (hint zone) | `Target.EnemyForceUnnormalized` | the same |

  The leaf's failure flag is ignored (a failing leaf counts as 0.0). Unlike galactic 0x5f2430, nothing else is added:
  no own-structure power term.
- **Satisfied test** 0x72ac70 (as AI_GALACTIC_2 §1.4): only typed rows (key ≠ 0) with target > 0 and remaining > 0
  add to the residual. A list with no positive typed row is satisfied at once (residual 0 ≤ T).
- So against a unit target the **main force is sized against the target object's own power** and the escort against
  what defends it.

#### 7.7.2 Worked example: `Destroy_Structure` against a command centre [H data, M consequence]

| | FoC `DestroyStructure.lua` | TR `DestroyStructure.lua` |
|---|---|---|
| Contrast scales | library default 1.1 / 1.5 → T = 0.267 | `MinContrastScale 0.2`, `MaxContrastScale 2.0` → **T = 0.9** (a 10 % cover is enough) |
| MainForce | `Infantry \| Vehicle \| Air = 1,10`, `LandHero = 0,2`, `DenyHeroAttach`, minus 4 transport / AA types | `Infantry \| Vehicle \| AirGunship \| AirSpeeder = 1,20`, `InfantryHero \| VehicleHero = 0,2` |
| GuardForce (EscortForce) | `Squad_Rebel_Trooper \| Squad_Stormtrooper = 0,2` | `Infantry = 0,4` |
| Goal (LandGoals.xml, same in both) | Enemy_Structure, Tactical_Targeted, Low_Threat, Time_Limit 30, Activation_Tracking_Duration 20, Per_Activation_Failure_Desire_Adjust −15 | same |

- `Rebel_Command_Center` (and `Imperial_Command_Center`) sets **no `AI_Combat_Power`** in FoC or TR, so its power is 0.
  The main list's entry 0 is 0, all typed rows are 0, and main contrast is satisfied before any unit is chosen.
  (FoC barracks / factories have 300-800; TR's barracks template has 1.)
- The solver (AI_GALACTIC_2 §1.2) then fills only the team minimums: Contrast scores 0 for a candidate while a slot is
  below its min and vetoes it afterwards. Result with no defenders near the CC: **one MainForce unit, no escort**.
  The pick is the best sum of FreeStore (nearest free unit, 1 − key/maxKey), Plan (team fill), TechTree (tech/5 × 2)
  and Hero (0): the nearest high-tech unit of any listed category, chosen without regard to its damage against
  structures (TR infantry do about 0.2 per hit after `ArmourG_Structure`).
- With defenders near the CC the escort list (their power) is positive. Escort-team candidates fill GuardForce. A
  candidate that only fits MainForce can also score on the escort list (main contrast met). The first placement pass
  skips its MainForce slot (main met, escort not), so the second pass puts it there. Corrected q6 t18
  (AI_GALACTIC_TOKENS §9.10 [H]): the commit 0x6c4b20 subtracts a non-escort unit from the **escort** list once the
  main contrast is met, so these MainForce units do reduce the escort residual. MainForce and GuardForce together
  grow until the defenders' power is covered (or the teams are full). This is the only way a `Destroy_Structure` task
  force against a zero-power structure gets large.
- Every ground structure without `AI_Combat_Power` behaves like the CC. `Applies` keeps the CC a target while fogged:
  both CCs set `Last_State_Visible_Under_FOW` (TR also `Initial_State_Visible_Under_FOW`), §3.3 step 1.

#### 7.7.3 When the task force is released [H]

`MainForce_Thread` is `Produce_Force` → `QuickReinforce` → `Set_Land_AI_Targeting_Priorities` →
`BlockOnCommand(MainForce.Attack_Move(AITarget, MainForce.Get_Self_Threat_Max()))` (no time limit) → `ScriptExit()`.
GuardForce loops `Escort(GuardForce, MainForce)` forever. The plan ends, and every unit returns to the free store
(plan end 0x5f6a50, `Release_Forces(1.0)`), on the first of:
1. **The Attack_Move block finishes** (A1_READINGS §33.2, handler 0x73ce80). For an object target it does **not**
   finish on arrival. It finishes when the target dies (signal 0x10), when the target turns non-hostile (0x1a), when
   every unit of the block is gone (died, changed owner, or taken by a newer order, signal 0xb), when the TF is empty,
   or, after all units reported arrival, when the target is no longer hostile, fails 0x5680b0 (alive, not garrisoned,
   not in a transport, not in a hero clash, …; A1_READINGS §33.2.1), or is fogged for the AI **and** its type has
   neither `*_State_Visible_Under_FOW` flag. Its Result is always nil (A1_READINGS §33.2.2). The
   CC has the flag, so its block cannot finish by fog. While the CC lives, the plan holds its units there.
2. **Reinforcement path.** In land `QuickReinforce` proceeds only when `Are_All_Units_On_Free_Store()`. If any unit had
   to be bought (reinforcement pool), it calls `WaitForAllReinforcements(tf, target)` (drops near the target) and then
   `ScriptExit()` ("Abandonning plan for reproposal if still relevant"). The dropped units are released near the
   target and the goal is proposed again.
3. **The goal is dropped by the goal system** (AI_GALACTIC §1.6). `DestroyStructure.lua` never calls
   `Set_As_Goal_System_Removable(false)`, so its goal is replaceable: maintenance drops it when its re-evaluated desire
   is ≤ 0 or the PP re-check 0x6c47c0 (contrast lists rebuilt, Finalize re-run) fails; Resolve_Category drops it when
   higher-effective-desire Tactical_Targeted proposals take the category's slots. `Time_Limit` (30 s) does **not** end
   a running tactical plan: it is only an activation test (`0 < AllowedTime < ETA`), and the AllowedTime < ETA
   maintenance drop is galactic-only.
4. **Plan events.** `Default_No_Units_Remaining` and `Default_Original_Target_Owner_Changed` call `ScriptExit()`;
   `Default_Original_Target_Destroyed` does not (the block's signal ends the thread).
5. **Other plans.** `Purge_Goals` from `BurnUnitsLand` (and the tech plans) ends every other running plan.
6. **Single units** leave without ending the plan: `Default_Unit_Damaged` (PGEvents) releases a unit it sends to kite
   or heal when the attacker is good against it, it is a hero, or its hull or shield is < 0.33 (`GoHeal(…, release)`
   = `Move_To(healer)`, `Lock_Current_Orders`, `Release_Unit`); `Try_Garrison` does the same. Damage from the plan's
   own `AITarget` is ignored by that handler, so a CC without weapons never triggers it.

**Re-tasking.** A released unit is re-admitted to the free store (TF vf49 0x5ff610: free-store admission, then its
targeting priorities reset to its type's, A1_READINGS §39.2). No stop order is issued, so a unit keeps shooting its
current target [M: no order call on that path]. The goal is dropped at the next Commit and the same
(Destroy_Structure, CC) pair is proposed again within one or two passes on Normal / Hard (A1_READINGS §39.1). If its
activation fails (no free unit, PP fails), each failure in the last 20 s adds −15 to the desire. That silences the CC
goal for up to 20 s (FoC desire ≈ 15, TR ≈ 5), then it comes back.

#### 7.7.4 What the free store does with idle units near the end [H, shipped Lua]

The two games differ here, and this is where the end game is decided.
- **FoC `BusyTacticalFreeStore`** (§5.5): `Service_Attack` takes `Find_Nearest(unit, owner, false)`, the nearest
  enemy UNIT_AI object **visible** to the AI (real fog; structures count: both CCs have UNIT_AI). If it is not good
  against the unit (a CC without weapons never is), the unit `Attack_Move`s to it when within 600, **and at any
  distance when `aggressive_mode`**, which is always on in skirmish. Only with no visible enemy does it use
  `enemy_location` (hint-zone `EnemyUnitConcentration`, power-weighted, A1_READINGS §16, so a zero-power CC adds 0),
  then `Service_Guard` (guard the nearest own structure).
- **TR `LandTacticalFreeStore`** (TR's AI players declare `LandFreeStoreScript LandTacticalFreeStore`;
  `BusyTacticalFreeStore` is their space script): `Service_Attack` needs `aggressive_mode`
  (`Allowed_As_Defender_Land` > 0, or an `ATTACKER ENTRY POSITION` hint) and a visible nearest enemy. It
  `Attack_Move`s to that enemy only when it is within `free_store_attack_range` (600; 6000 once an `AirGunship` /
  `AirSpeeder` was serviced, a global that is never reset), else to `enemy_location`, else it gives up. Then
  `Service_Kite`, `Service_Heal(1.0)` (an idle unit with any damage walks to the nearest own healer), and
  `Service_Guard`: `Attack_Move(friendly_location)` (the zone with the highest own unit concentration) plus
  `Try_Garrison`, or guard the nearest own structure. **An idle TR unit more than 600 from the CC walks home.**
- TR's land goal set is `BasicLandGoalSet` only (no Expansion / faction land sets) and its land template is
  `Generic_Land` (Tactical_Targeted on).

#### 7.7.5 Goal proposal for the last structure: no special end-game rule [H]

- No engine code proposes "finish the base". There is no desire ramp over battle time and no cheat or aggression
  ramp for land targets. FoC and TR land data have no "close to victory" token (`Am_I_In_Winning_Position` is
  galactic). `Destroy_Structure` is proposed for the CC on every pass with its normal desire.
- **FoC `Should_Destroy_Structure`** for a CC: `Allowed_As_Defender_Land` (1 in skirmish) × 1.0 (faction owner) ×
  (0.001 + 15 × (1 − `Target.Location.EnemyForce`)) × (1 − `May_Lose_Land_Control_Game`). With no defenders: ≈ 15.
- **TR `Should_Destroy_Structure`**: `Allowed_As_Defender_Land` × (1 − neutral owner) × (0.001 + turret / bacta /
  repair term + `Priority_Structures` + 20 × mines / reinforcement points + 5 × (1 − `Location.EnemyForce`)) ×
  (1 − turbolaser tower) × (1 − `May_Lose_Land_Control_Game`) × (1 − `IsContestable`) ×
  **`Is_Initial_Skirmish_State_Established`**. For a CC that is ≈ 5. The last factor is about **Self** (the
  attacker): it is 1 only while Self owns a barracks **and** a command centre **and** has non-structure force > 0
  **and** more than one resource structure (`TacticalBuiltStructureCount` of `Mineral_Processor`,
  `Skirmish_Ground_Mining_Facility`, the two GroundWar reinforcement-point upgrades), or while Self owns any
  `Ground_Level_*_Tech_Upgrade` (the "+" term; the sum can exceed 1). An Empire with at most one mine and no tech
  upgrade proposes **no** `Destroy_Structure` at all.
- **Burn_Units_Land.** FoC: as §5.6 item 5. TR: the campaign branch, or in skirmish `UnitSpaceAvailable` < 1 and
  reinforcements > 0.25 × (or >) the enemy's half-infantry force. The plan collects every free unit and
  `Attack_Move`s the nearest **visible** enemy with `InBase` (CCs have it), else the nearest visible enemy, until
  it dies. With nothing visible it does nothing (one pass, `Sleep(5)`, release). TR's `Should_Firesale_Land` is
  campaign-only, so TR skirmish never reveals the map. FoC's firesale reveals only for the **losing** AI (no command
  centre) and the local human, never for the winner.

#### 7.7.6 How a vanilla AI-vs-AI land skirmish ends [M: chain of [H] rules, not probed]

1. `Destroy_Structure` for the enemy CC (desire ≈ 15, applicable while fogged) sends a 1-unit task force (§7.7.2). Its
   Attack_Move block holds that unit at the CC until the CC dies or the unit dies (§7.7.3 item 1).
2. Its arrival reveals the CC to the AI. From then on every idle free-store unit, map-wide, `Attack_Move`s to it
   within about 2 s of its next service (FoC aggressive `Service_Attack`, §7.7.4). Burn_Units_Land does the same for
   a capped AI. The combined damage beats the 14 hp/s self-repair.
3. When the last `Victory_Relevant` enemy object dies, 0x2c0340 declares the winner after the 210-frame countdown
   (SKIRMISH_SETUP §4).
- If the 1-unit task force dies on the way, the goal is re-proposed (step 1 again). The goal-level pressure is one
  unit at a time; the end comes from the free store's reaction to a visible target.
- **TR on the original engine** [M]: step 2 reaches only idle units within 600 (air 6000) of the CC, and step 1
  depends on `Is_Initial_Skirmish_State_Established` for the attacker. A slow end against a self-repairing CC is
  therefore what TR's scripts produce. The port should match these rules first and judge TR by them.

**Port notes**
- Contrast (engine/ai tactical PP): build the tactical lists as §7.7.1: main list `Target.ForceUnnormalized` (unit) /
  `Target.EnemyForceUnnormalized` (region), escort list `Target.Location.EnemyForceUnnormalized` only when an
  EscortForce exists, typed rows × (Max + f·PFCA) × `Land_AI_Contrast_Multiplier`, no own-structure term. A 1-unit
  `Destroy_Structure` task force against a zero-power CC is correct; do not enlarge it.
- Attack_Move on an object target must stay open after arrival while the target lives and is hostile and not
  (fogged without the visible-under-FOW flags). If the port's TFs "get released" while the CC lives, check this first:
  log block finish reasons per plan.
- Make sure `Applies` honours `Last_State_Visible_Under_FOW` / `Initial_State_Visible_Under_FOW` for the CC, so the
  goal is proposed while the CC is fogged.
- TR: log `Is_Initial_Skirmish_State_Established` and `Number_Resource_Structures_Land` for the Empire in the stuck
  run. If the factor is 0, no `Destroy_Structure` goal exists, which is the 13-minute "no attacker within 2884"
  picture. `TacticalBuiltStructureCount` and `HasUnit` for tech-upgrade objects must match the original
  (AI_TACTICAL_PERCEPTION).
- Free store: run TR's `LandTacticalFreeStore` for land and `BusyTacticalFreeStore` for space, as the AI player XML
  says; FoC uses `BusyTacticalFreeStore` for both. `Find_Nearest` must include UNIT_AI structures and use real fog
  for AI players.
- Do not add an end-game cheat, a reveal for the winner, or a Force_Healing combat gate: the original has none.

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
3. LandMovementBlockStatus / SpaceMovementBlockStatus completion rules (arrival radius, what counts as finished for attack-move and guard), and `Explore_Area`, `Prepare_Ambush`, `Bombing_Run`, `Fire_Special_Weapon`, `Find_Closest_Enemy` bodies. **`Prepare_Ambush` done (S7 t18, §7.3.1); empty-task-force `Move_To` / position done (S7 t17, §7.3.2); land `Fire_Special_Weapon` done (s78f t04, 2026-11-03, §7.3.4; [M] left: the unit AI target's position getter and the hint-zone centre z).** Still open from §7.3.1/§7.3.2: the mode box vf +0x1f8 vs the playable box vf +0x208; the lost 5th signal id of the ambush member subscription [L: 0x1a]; ~~the meaning of the type flags +0x13 / +0x14 / bit 0x37 in the No_Units_Remaining test~~ (done 2026-11-02, §7.3.3: named hero, generic hero, Land LOBBING_SUPERWEAPON; +0x1e = Is_Special_Weapon_In_Space); whether a plan can start with a 0-unit optional team. Still open from §7.3.3: whether an object-target ability's pending entry is cleared when its target dies (else its block waits for 0xb / 0x10 / any 0x29); the meaning of locomotor vf +0x2b0 in the DEPLOY gate.
4. Reinforcement landing positions (0x6eaf60) and the land ReinforceBlockStatus finish/fail rules. (Space resolved by refixf t06, §7.4: ring search in SpaceReinforceBlockStatus; 0x2b03e0 is the population-room test.)
5. Free-store admission bit meanings (type bitfield +0x790 bits 0x20, 0x5b, 0x5f; obj +0x2b0/+0xf8; mode +0x38 set) and the +0x30 lock field writer; `Lua_Is_Object_On_Free_Store` / `Get_Object_Count`. **+0x30 writers/readers and the two Lua methods: done, A1_READINGS §28** (+0x30 = owning PP; `Lock_Current_Orders` is obj +0x3a8).
6. `Project_By_Unit_Range`, `Get_Self_Threat_Max/Sum`, `Get_AI_Power_Vs_Unit`, `Should_Switch_Weapons`, `Is_Good_Against`, `Get_Time_Till_Dead` (used by every land plan and PGEvents).
7. AIBuildTask tactical path (FREESTORE_ALLOC vs purchase vs HIJACK_UNIT ordering) — confirm with a probe. ~~Skirmish purchase into the reinforcement pool vs spawn at the factory~~: pool, never at the factory (SKIRMISH_SETUP §3.1, q6 t19) [H code].
8. Event code ↔ name table (shared with AI_GALACTIC open item 8) — needed to dispatch `Unit_Damaged`, `Target_In_Range`, etc. in the right order relative to combat.
9. Confirm `Has_Space_Evaluator` on own type at space init vs representative type at runtime (§3.1) and the conflict-record attacker/defender offsets (§1.2).
10. Probe: a skirmish AI-vs-AI land and space run logging per-frame synced-RNG counts and AI orders would validate §2, §5.3 and §10 (Gabriel launches; kit under `tools/probe_mod/`).
11. From §7.7 (q6, 2026-11-04): confirm on the original that a 1-unit `Destroy_Structure` task force is what the AI sends against a command centre with no defenders, and that MainForce grows only while the escort list is unmet [M]; whether a released unit keeps its current attack order (no order call was found on the release path) [M]; `Number_Resource_Structures_Land` / `TacticalBuiltStructureCount` semantics for structures under construction (they gate TR's `Is_Initial_Skirmish_State_Established`): **answered 2026-11-12, they count from the build start** (SKAI_T06_LAND_RUNS §7).
12. Probe (Gabriel): a TR AI-vs-AI `_MP_LAND_BLOOD_GULCH` run on the original, logging how long the last CC survives, would show whether TR's slow end (§7.7.6) is retail behaviour.

---

## 13. Summary for the implementer

**Key rules**
- One context per AI player per battle; first service picks the opponent (attacker/defender preference), runs the six subsystems' begin hooks, and (land campaign battles only) swaps the AI's credits for the template tactical allowance, returning leftovers × Credit_Multiplier at the end.
- Cadence is frame-driven in T7, player index order: Goal and Template every frame, Planning and Execution every 3 frames, Perception and Learning every 300 frames (no work). Easy sleeps 15 s after each tactical goal commit.
- Targets: unit targets for objects whose type has `Has_Land/Space_Evaluator`, appended on creation and erased in place on death; land regions = map AI hint zones; space regions = a 2000-unit grid named `CELL_x_y`. `Applies` classifies by behaviour slots (unit / build pad / cash point / reinforcement point / structure) and owner alliance, and hides fully fogged objects unless their type has `Last_State_Visible_Under_FOW` / `Initial_State_Visible_Under_FOW` (command centres do). Base attacks: §7.7.
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

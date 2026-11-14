# Tactical AI perception: evaluators, tokens and the threat grid (roadmap A1)

Clean-room behavioural spec from the Ghidra 12.2 decompilation of the 64-bit Steam `StarWarsG.exe` (FoC v1.121, image base 0x140000000; all addresses are RVAs), vtables read from the same exe, and shipped FoC data (`Data/XML/AI/PerceptualEquations/*`, `Data/XML/GameConstants.xml`, `Data/Scripts/{AI,FreeStore,Library}/*.lua`). No decompiled code is reproduced.

Confidence tags: **[H]** read directly from code or data; **[M]** one inferred link (a field meaning, a callee's purpose, a name); **[L]** plausible, not verified.

**Scope.** Closes open items 1 and 2 of AI_TACTICAL.md §12 (tactical perceptual evaluators; `TacticalPerceptionGridClass`) and parts of item 6 (`Get_Self_Threat_Max/Sum`, `Project_By_Unit_Range`, `Is_Good_Against`). It builds on, and does not repeat:
- AI_TACTICAL.md §3 (target lists, `Applies`, region targets, reachability), §6 (FindTarget, Reachable_Target, FindDeadlyEnemy, the first description of Get_Most_Defended_Position).
- AI_GALACTIC.md §1 (framework), HOTSPOTS.md §2 (PerceptionContext, equation evaluation in PerceptionFunctionG.dll), GALACTIC_ECONOMY.md §6.1 (token paths evaluated through 0x4dd1d0), PHASE2_ANSWERS Q7.
- The engine already has the equation language, the token registry (`engine/ai/src/token_registry.inc`, ids from 0x5b43d0) and the galactic tokens (PHASE4 S2). This document specifies the **tactical token leaves** that the language resolves.

Notation: FPS = 30 (0x140b0a340); F = current mode frame (mode +0x10); GC+x = GameConstants offset (base 0x140b15800); "player list order" = PlayerList index order (0x140a16fd0).

---

## 1. Evaluation machinery (shared with galactic)

### 1.1 Evaluator nodes [H]
Every evaluator class has a registration virtual (slot 8, +0x40). It builds a map `token id → child node` (0x64cbd0). A child is either:
- a **leaf node** (0xB0 bytes, `VisiblePerceptualEvaluatorClass<…>`): a pointer-to-member function at +0x88 (adjustor at +0x90) and a **cache lifetime** at +0x98; or
- a **sub-evaluator** (another evaluator object), reached by the next token of the path (`Location`, `Owner`, `Hints`, `Type`).

Leaf signature: `bool f(evaluator, bindings, double* out)`; false = evaluation failure (the whole equation fails, AI_TACTICAL §6.1 skips the candidate). Several leaves are shared stubs that set a flag in r9 (friendly = 1 / enemy = 0) and tail-jump to one body; this document lists the body RVA.

Token ids are the 225 globals filled by 0x5b43d0 (names and order are already in `token_registry.inc`).

### 1.2 Leaf cache (0x64d740 lookup, 0x64d990 store) [H]
- Each leaf keeps a sorted map keyed by a hash of the binding set (0x5f3e60) with an equality test (0x5f3990). Corrected q6 t18 (AI_GALACTIC_TOKENS §9.9 [H]): the key covers the **parameter bindings only**; Self / Enemy / Human / Target are context variables and are not in it. Each leaf node exists once per (perception system, target evaluator), so the cache is per AI player, per mode and per target object. An entry stores the value and the frame `stamp` it was computed.
- **Lookup** only when the lifetime `L` (+0x98) ≠ 0: hit if `stamp ≠ 0` and (`L = −1` or `NOW ≤ stamp + L × 30`). `L` is in **seconds**; −1 means "forever". A value computed on frame 0 is never a hit (stamp 0).
- **Store** always happens on a successful computation (also for `L = 0`); entries are never pruned during the battle.
- Consequence for parity: a token cached for L seconds keeps returning the old value even if the world changed. The port must cache the same way (same key, same lifetime, same frame) — this is part of the deterministic behaviour, not an optimisation.

The lifetimes are given per token in §3.

### 1.3 Parameters and defaults [H]
Leaves read their parameters from the bindings: `Parameter_Category` (category bitmask, default **all bits**), `Parameter_Attenuator` (default **0.0**), `Parameter_Type` (list of type names, default empty = no filter), `Parameter_Faction` (string), `Parameter_Time`, `Parameter_Range`, `Parameter_Count` (default 1), `Parameter_Only_Consider_Complete` (default 0), `Parameter_Difficulty_Level_Type`, `Parameter_Hard_Point_Type`. A token that ignores a parameter simply never reads it.

The **attenuator** appears in every force sum as the factor `1 − (1 − health) × A`, where `health` is the object's hull fraction (0x396df0: current / `Get_Max_Tactical_Health`, teams via 0x405230). A = 0 counts full combat power; A = 1 scales power by health.

### 1.4 Normalised tokens (0x4de270) [H]
`Force`, `FriendlyForce`, `EnemyForce`, `*NBTD`, `*NBTP`, `*UnitConcentration`, `*UnitConcentrationNBTD` do not compute anything themselves. They evaluate two other **token paths** through the same perception system entry used by galactic code (0x4dd1d0, GALACTIC_ECONOMY §6.1):
1. numerator path, evaluated **with the caller's parameters** (category, attenuator, …);
2. if the numerator failed → fail; if it is exactly 0 → result 0;
3. denominator path, evaluated **without** the caller's parameters [M: a fresh binding set carrying only the variables];
4. result = `num ≤ den ? num / den : 1.0` (so the result is in [0, 1] and saturates at 1).

The paths are listed in §3. This is the only place where tactical evaluation recurses into the evaluator tree; port it as a call back into the S2 evaluator with a constructed path.

### 1.5 Which machinery is reused from S2 [H]
- Same equation language, linker, `#` random, `Function_`/`Script_` references and token ids (S2 `perception_equations.*`): no tactical changes.
- Same evaluator protocol as galactic (root → path → leaf, failure propagation). Tactical evaluators are all `Visible…` wrappers; there are **no** `Fogged…` tactical evaluator classes (the galactic planet evaluators have them). Fog in tactical is handled inside the threat grid and per token (§4.6).
- The galactic `Node_Weight`/danger consumers and the tactical normalised tokens share the path-evaluation entry 0x4dd1d0.
- New for A1: the tactical leaf bodies (§3), the cache (§1.2; galactic leaves use the same cache code, but S2 did not need it because galactic evaluation sites are serial per call), and the threat grid (§4).

---

## 2. The tactical evaluator tree

| Root / path | Evaluator object | Created by | Conf |
|---|---|---|---|
| `Game` (token 0x140a43c70) | `LandGamePerceptualEvaluatorClass` (vt 0x8c0e68) / `SpaceGamePerceptualEvaluatorClass` (vt 0x8c0c98); tokens = Game base (0x6e2170) + TacticalGame (0x738010) | perception slot 6 (0x6b9140 land, 0x6ba570 space) | H |
| `Variable_Self` / `Variable_Enemy` / `Variable_Human` (player name id, player +0x478) | `LandPlayerPerceptualEvaluatorClass` (vt 0x8c0d08) / `SpacePlayerPerceptualEvaluatorClass` (vt 0x8c0bd0); tokens = Player base (0x6e09c0) + TacticalPlayer (0x736790) [+ LandPlayer (0x6d4460)] | slot 6, one per player in player list order | H |
| `Variable_Target` = unit target (`OBJECT_<id>`) | **land:** `LandUnitPerceptualEvaluatorClass` (vt 0x8c1048, ctor 0x6d70d0) for **every** object with `Has_Land_Evaluator`, structures included; **space:** `SpaceShipPerceptualEvaluatorClass` (vt 0x8c09f0, ctor 0x6d27f0) for every object with `Has_Space_Evaluator`. Tokens = TacticalObject (0x731490) + TacticalUnit (0x733960) + LandUnit (0x6d7370) or SpaceShip (0x6d2990) | perception slot 11 (AI_TACTICAL §3.1) | H |
| `Variable_Target` = land region (hint zone) | `LandLocationPerceptualEvaluatorClass` (vt 0x8c0ed8, 0x6d55a0) bound to the zone; tokens = TacticalLocation (0x72ef80) + LandLocation (0x6d5b70) | 0x6b8d80 | H |
| `Variable_Target` = space cell `CELL_x_y` | `SpaceLocationPerceptualEvaluatorClass` (vt 0x8c0918, 0x6d1c90) with the cell rect; tokens = TacticalLocation | 0x6b9da0 | H |
| `….Location` (unit target) | land: a `LandLocationPerceptualEvaluatorClass` bound to the **unit** (0x6d5620, linked by 0x7304d0); space: a `SpaceLocationPerceptualEvaluatorClass` bound to the unit (0x6d1d00) | unit registration | H |
| `….Owner` (unit target) | `RootLookupPerceptualEvaluatorClass` (0x6df790) that forwards to the player evaluator of the object's owner **at registration time** (owner id obj +0x58 → player +0x478, 0x6dfaa0) | 0x731490 | H; M: not re-pointed on owner change |
| `….Hints` (land location) | `HintPerceptualEvaluatorClass` (vt 0x8c16e0) over the zone's hint list (zone +0xd0), e.g. `Hints.High_Ground` | 0x6deb90 | M |
| `….Type` (unit target) | the object-type evaluator shared with galactic (`GameObjectTypePerceptualEvaluatorClass`, `IsType`) | not traced | L |

`LandStructure…`/`SpaceStructure…`/`TacticalStructure…` evaluator classes exist but are only built by the save-load factory 0x64d610; at runtime every land object is a LandUnit evaluator [H for the creation sites, M for "load only"]. TacticalStructure's registration (0x6d33e0 → 0x734770 → 0x731490) has only the TacticalObject tokens.

A path whose token is not registered on that evaluator fails (e.g. `Variable_Target.EnemyUnitConcentration` on a unit target: units only have it under `.Location`). [M: failure comes from the DLL's lookup; the exe returns no node]

### 2.1 Location geometry [H]
A tactical location evaluator has: rect `(x, y, w, h)` at +0x98..+0xa4, attached object +0xa8, zone +0xf0 (land), the zone's threat-cell list +0xb0, zone centre +0xf8/+0xfc.
- **Space cell:** fixed rect = the cell clipped to the map (AI_TACTICAL §3.2).
- **Space unit location:** before every force/concentration/bitfield/time evaluation the rect is rebuilt as a square of side `2 × R` centred on the unit, `R` = the unit's max attack range (0x3749d0 on its representative type) [M: "max attack range"].
- **Land region:** the hint zone; queries use the zone's threat-cell list.
- **Land unit location:** every evaluation first looks up the unit's current hint zone (land mode slot +0x180 at the unit position). If it changed, the evaluator re-links (slot 13 0x6d65d0: new zone and cell list via 0x654210). **If the unit is in no zone, every force / concentration / time token returns 0** (success) [H].
- Centre for distances: unit position if attached, else zone centre, else rect centre.

---

## 3. Token reference

"Cache" is the lifetime L of §1.2 (s; −1 forever; 0 none). Unit tokens fail (or return 0 where noted) when the evaluator has no object.

### 3.1 Unit target tokens (TacticalUnit 0x733960, TacticalObject 0x731490) — land and space

| Token | Body | Cache | Value | Conf |
|---|---|---|---|---|
| `Health` | 0x7337f0 | 0 | hull fraction 0..1 (0x396df0) | H |
| `Shield` | 0x733920 | 0 | shield fraction (0x3976f0) | M |
| `Energy` | 0x732cc0 | 0 | energy fraction (0x396560) | M |
| `ForceUnnormalized {Category, Attenuator}` | 0x733400 | 0 | `AI_Combat_Power(type) × (1 − (1 − health) × A)` if the type's category bits (type +0x1648) intersect Category, else 0. Type = 0x396380 (the object's own / container type). `AI_Combat_Power` = 0x370e60 (squad fill scaling, GALACTIC_ECONOMY §6.2) | H |
| `Force` | 0x732d00 | 0 | §1.4: `Target.ForceUnnormalized / Game.ForceUnnormalized` | H |
| `ForceNBTP` | 0x7331a0 | 0 | `Target.ForceUnnormalized / <owner player>.ForceUnnormalized` | H |
| `ForceNBTD` | 0x732f40 | 0 | `Target.ForceUnnormalized / <owner player>.FriendlyForceUnnormalized` | H |
| `HasTakenDamage {Time t}` | 0x733730 | 0 | 1 if the DAMAGE_TRACKING behaviour (0x3e) last-damage frame (0x58bb40) ≠ −1 and `F ≤ last + t × 30`, else 0 (also 0 without the behaviour) | H |
| `IsBuildPad` | 0x733830 | **−1** | 1 if the object has TACTICAL_BUILD_OBJECTS (behaviour 0x29) | M (body not decompiled; reads obj +0x35b) |
| `HasBuiltObject {Type…}` | 0x733500 | 0 | 0 if not a build pad or nothing built / under construction on it (0x42e3b0, 0x42e3d0); with no Type list → 1; else 1 iff the built object's type name equals one of the listed names | H |
| `NearbyOpenBuildPadCount {Range r}` | 0x733870 | 0 | free-store side table: visible open pads within r of the object (0x4d3770); fails without Range | H call, M table |
| `ContainsHero {Type…}` | 0x732900 | **−1** | 1 if any of: the object's type, the types of its garrison/contained objects (obj +0xf8 list), or its team members (TRANSPORT/team list 0x397bf0) is a hero (type +0x13) — or, with a Type list, has one of the listed names. Cached forever per binding set | H |
| `DistanceToNearestFriendly / Enemy {Category, Type…}` | 0x730df0 | 2 | §3.3 (object variant: 3-D distance between the two objects minus both radii) | H |
| `IsContestable` | 0x731400 | **−1** | 1 if the object has behaviour 0x3D (obj +0x36f) and 0x3751a0(type, special player 0x331cc0) is true | M (behaviour 0x3D and the test not named) |
| `GarrisonSlotsAvailable` | 0x731390 | 0 | garrison capacity (0x3954b0) − occupants (obj +0x210 → +0x34) | M |
| `AdditionalPopulationCapacity` | 0x730dc0 | **−1** | pop-cap the object grants [L: body not decompiled] | L |
| `Owner.*` | sub-evaluator | — | player tokens of the owner (§3.4); shipped use: `Owner.IsFaction` | H |

### 3.2 Land unit (0x6d7370) and space ship (0x6d2990) extras

| Token | Body | Cache | Value | Conf |
|---|---|---|---|---|
| `IsCashPoint` | 0x6d71b0 | −1 | 1 if CASH_POINT behaviour (obj +0x36e) | M |
| `CashPointValue` | 0x6d7170 | −1 | cash value of the point, 0 without CASH_POINT | L |
| `IsInsideEnemyShield` / `IsInsideFriendlyShield` | 0x6d7210 (flag enemy=1) | 5 | land mode 0x3b8790(unit position, Self, flag): inside a base shield of an enemy / allied player | M |
| `IsValidBombingTarget` | 0x6d72c0 | 5 | [L: body not decompiled] | L |
| `Location.*` | land 0x6d5620 / space 0x6d1d00 | — | §3.3 on the unit's location | H |
| `HardPointHealth {Hard_Point_Type}` (space) | 0x6d28b0 | 0 | health of hard points of that type | L |
| `AreEnginesOnline` (space) | 0x6d2860 | 0 | engines hard point alive | L |

### 3.3 Location tokens (TacticalLocation 0x72ef80; LandLocation 0x6d5b70 adds the shield/bombing/hint tokens)

All grid queries take Self = the evaluation's `Variable_Self` player (bindings context +0x50) [M: field], a category mask, an **ally flag** (friendly = allied with Self including Self, relation value 0; enemy = every other non-neutral-faction player, §4.5), and exclude the location's own attached unit.

| Token | Body | Cache | Value | Conf |
|---|---|---|---|---|
| `FriendlyForceUnnormalized` / `EnemyForceUnnormalized {Category, Attenuator, Faction}` | slot 15: 0x72db60 (land 0x6d5690 adds the zone check of §2.1) | **10** / **9** | **space / rect:** `Σ_cells CellForce` over the threat cells overlapping the rect (0x654290); **zone:** over the zone's cell list (0x6544a0); × `AI_{Space,Land}AreaThreatScaleFactor` (GC+0x10f0 / +0x10f4, shipped 1.0). CellForce = §4.5. `Faction` (if given) keeps only objects whose owner's faction is that faction | H |
| `FriendlyForce` / `EnemyForce` | 0x72d620 | 0 | `[Target(.Location if the target is a unit)].F/EForceUnnormalized / Game.ForceUnnormalized` | H |
| `FriendlyForceNBTD` / `EnemyForceNBTD` | 0x72d8c0 | 0 | same numerator / `Self.F/EForceUnnormalized` | H |
| `ForceUnnormalized` | 0x72cf10 | 0 | point danger at the location centre (unit position or rect centre; zone → zone total) — §4.6 (0x653ff0 / 0x653e90) | H |
| `FriendlyUnitConcentrationUnnormalized` / `EnemyUnitConcentrationUnnormalized {Category}` | slot 16: 0x72ea70 (land 0x6d5a10) | **11** / **8** | `Σ_cells` of 0x656800: AI_Combat_Power of objects whose **centre cell** is the cell (no spreading, no health factor); rect 0x6565e0 / zone 0x656750. No area-scale factor | H |
| `F/EUnitConcentration` | 0x72e530 | 0 | `[Target(.Location)].F/EUnitConcentrationUnnormalized / Game.ForceUnnormalized` | H |
| `F/EUnitConcentrationNBTD` | 0x72e7d0 | 0 | same numerator / `Self.F/EForceUnnormalized` | H |
| `FriendlyUnitsBitfield` / `EnemyUnitsBitfield {Type…, Category…}` | slot 17: 0x72ebc0 | 10 | builds a list from the `Parameter_Type` names then the `Parameter_Category` masks; bit i set if some matching object (exact type or category intersect) is in the area (0x654b70 rect, 0x654ce0 zone, per cell 0x654d90). Value is an integer bitfield | H |
| `DistanceToNearestFriendly / Enemy {Category, Type…}` | 0x72d000 | 2 | scan all UNIT_AI objects of qualifying players (grid per-player lists, player list order, list order): alive (obj +0x3a0 bit 0x40 clear), category match, type match on the representative type (0x3973b0) if a Type list is given, **not fogged for Self under real fog** (0x35f470 with force = 1: the `AIUsesFogOfWar*` switches are ignored; only the global FOW-active flag 0x140a284c4 matters). Distance = 2-D centre distance (location centre) − the location's unit radius (if any) − the found object's radius (0x397780); first minimum wins. None found → **9.999999843067494e17** (1e18 as a float) | H |
| `TimeLastSeenUnnormalized` | slot 10: 0x72e410 (land 0x6d5950) | 7 | mean over the area's threat cells of `fogged ? (F − lastSeenFrame[Self][cell]) : 0`, divided by 30 → seconds. "Fogged" = the cell centre is fogged for Self under real fog (0x35f3b0, force = 1). Land skips cells whose terrain flags & 0x22 are set (corrected in A1_READINGS §26: base passability Steep | Water, left out of both sum and count) [H]. 0 if no cell is fogged | H (from the instruction stream; the decompiler lost the return) |
| `TimeLastSeen` | slot 9: 0x72e3c0 | 7 | `TimeLastSeenUnnormalized × 30 / F` (fraction of the battle) | H |
| `IsFriendlyStartLocation` | slot 12: 0x72e0f0 | −1 | skirmish: marker `Player_%d_Position_Marker` (Self's player id); campaign battle: `DEFENDING FORCES POSITION` if 0x28aec0(Self) [M: "is defender"] else `ATTACKER ENTRY POSITION`. Value = max over markers of that type of `1 − d / (2 × D)`, d = 2-D distance marker → location centre, D = map half-diagonal [M: D from mode slot +0x1f8 extents]. Fails if the marker type is missing | H |
| `IsEnemyStartLocation` | slot 11: 0x72de80 | −1 | campaign: the opposite marker; **skirmish: `1 − IsFriendlyStartLocation`** | H |
| `IsInsideEnemyShield` / `IsInsideFriendlyShield` (land) | 0x6d5760 | 5 | the zone centre inside an enemy / allied base shield (0x3b8790) | M |
| `IsValidBombingTarget` (land) | registered, body not traced | 5 | — | L |
| `Hints.<name>` (land) | 0x6deb90 | — | hint evaluator of the zone | M |

### 3.4 Player tokens (Player base 0x6e09c0, TacticalPlayer 0x736790, LandPlayer 0x6d4460)

P = the player the evaluator belongs to (`Variable_Self`, `Variable_Enemy`, `Variable_Human`, or `Target.Owner`).

| Token | Body | Cache | Value | Conf |
|---|---|---|---|---|
| `ForceUnnormalized {Category, Attenuator}` | vcall +0x70 → 0x6d4450 → slot 13 with friendly = 1 | 1 | same as `FriendlyForceUnnormalized` | H |
| `FriendlyForceUnnormalized` / `EnemyForceUnnormalized` | slot 13: 0x7356f0 (land 0x6d4180) | 2 / 2 | 0x656400: for every player Q in list order with `relation(P→Q) == 0` (friendly) or `≠ 0` (enemy) — nothing if P's faction is neutral (0x294e10) [M] — sum over Q's UNIT_AI objects (grid list +0x88[Q]) with category match of `AI_Combat_Power × (1 − (1 − health) × A)`. **Omniscient**: no fog filter. Land adds objects with TRANSPORT_LANDING (behaviour 0x36: reinforcements still in the air) whose owner relation matches, under a campaign/visibility condition [M] | H |
| `LandedForceUnnormalized` (land) | 0x6d4450 | 1 | identical to `FriendlyForceUnnormalized` (same body) | H |
| `Force` | 0x735290 | 1 | `P.ForceUnnormalized / Game.ForceUnnormalized` | H |
| `FriendlyForce` / `EnemyForce` | 0x735040 | 3 / 3 | `P.F/EForceUnnormalized / Game.ForceUnnormalized` | H |
| `ForceNBTD` | 0x7354c0 | 2 | `P.ForceUnnormalized / P.FriendlyForceUnnormalized` — the two are the same sum, so the value is **1 whenever P has force, else 0** (vanilla quirk) | H |
| `IsDefender` | 0x735e10 | −1 | 0 when there is no campaign conflict (0x28afb0: skirmish); else 1 if P is not the attacker (0x28adf0) | M |
| `CanRetreat` | 0x734fc0 | 0 | 1 if the mode's retreat coordinator (mode +0x30, slot +0x48) returns 0 for P | H |
| `IsRetreating` | 0x735ea0 | 0 | [L: not decompiled] | L |
| `BaseLevel` | slot 15: land 0x6d3ff0 / space 0x6d3550 | 0 | **land skirmish:** count of P's alive objects with behaviour 0x5F (DUMMY_GROUND_STRUCTURE, obj +0x391), clamped 0..5; **land campaign:** count of the conflict planet's ground structures (planet +0x158 list) without flag 4; **space:** the starbase level (0x397540) of the first object with behaviour 0x5C (DUMMY_STAR_BASE) and without 0x3D among players allied with P, else 0 | H (behaviour names M) |
| `OpenBuildPadCount {Type…}` | 0x735ef0 | 0 | **Self only** (else fail): pads in the free-store visible-pad table (0x4d3830); with types, count pads whose type is listed or accepted (0x374330 with P's faction) | H |
| `TacticalBuiltStructureCount {Type…}` | 0x7364d0 | 0 | **Self only**: free-store count of built structures (0x4d3630), summed over the listed types (all if none). A structure under construction counts, as the type it becomes (trace, SKAI_T06_LAND_RUNS §7) | H |
| `ReinforcementsUnnormalized {Category}` | 0x736320 | 0 | campaign battle: Σ AI_Combat_Power of P's persistent units in orbit (galactic reinforcement pool, 0x28aaa0 → +0x40) by category; skirmish: Σ over P's tactical reinforcement pool types (player +0x2c8, count +0x2d0) | H |
| `UnitSpaceAvailable` | 0x736700 | 0 | `clamp(popCap − popUsed, 0, popCap)` (0x2ac5c0 / 0x2aba70) | H |
| `IsBombingRunAvailable` / `IsOrbitalBombardmentAvailable` (land) | 0x6d4350 / 0x6d43d0 | 0 | mode managers +0x38 / +0x40 report availability for P | H |
| `HasUnit {Type…, Count n = 1, Only_Consider_Complete c = 0}` | slot 9: 0x7357d0 | 8 | counts, until n reached: P's objects of a listed type (or, when c ≤ 0, whose type +0x9f0 "builds into" a listed type); for listed types with `Is_Community_Property` (+0x27) also objects of players allied with P; P's reinforcement pool; and (c ≤ 0) P's production queue (player +0x3a0). 1 if n reached | H |
| `IsFaction {Faction…}` | 0x6e0700 | −1 | 1 if P's faction name equals one of the strings (exact byte compare after the parser's normalisation [M]) | H |
| `IsDifficulty {Difficulty_Level_Type}` | 0x6e0620 | 0 | 0 for a human P or when 0x2814d0 is false; else AI difficulty (AIPlayer +0x58) == value | H |
| `CreditsUnnormalized` | slot 10: 0x6e05f0 | 0 | P's credits (float +0x70) if P is Self, else 0 | H |
| `TechLevel`, `BudgetAllocation`, `StoryTrigger`, `ActiveGoals` | 0x6e09a0, 0x6e0560, 0x6e08e0, 0x6e0490 | 0 / 2 / 0 / 0 | galactic-side meanings (S2); `BudgetAllocation` in battle = the category's weight in the tactical template's `<Budget>` block, Self only (AI_GALACTIC_TOKENS §9.3, H) | M |

### 3.5 Game tokens (Game base 0x6e2170, TacticalGame 0x738010)

| Token | Body | Cache | Value | Conf |
|---|---|---|---|---|
| `Age` | 0x6e1f20 | 0 | `frame × (1/30)` seconds; frame = the **AI context's mode** frame when available, else the current mode | H |
| `IsCampaignGame` | 0x6e1ff0 | −1 | tactical: 1 iff the battle has a galactic conflict parent (0x28aa00) | H |
| `IsLandControlGame` | 0x6e2060 | −1 | [L: body not decompiled] | L |
| `IsSetupPhase` | 0x6e2090 | 0 | 0x35f790 | M |
| `ForceUnnormalized {Category, Attenuator}` | 0x737ee0 | 3 | 0x656400 over **all** players (P = null) — the denominator of every normalised token | H |
| `ForceVisibility {Category}` | 0x737f80 | 2 | for Self: among other players' UNIT_AI objects in the category, fraction whose UnitAI reports visible to Self (unit-AI slot +0xa0) [M: slot meaning]. 0/0 → NaN when there are none (vanilla) | H |
| `Timeline`, `InterventionsEnabled`, `TimeSinceStoryPopup`, `ActiveStoryGoalCount`, `IsStoryCampaign` | 0x6e2140, 0x6e1fb0, 0x6e2110, 0x6e1f00, 0x6e20e0 | 0/0/0/0/−1 | story/galactic flags | L |

### 3.6 Where the shipped tactical equations use them [H]
Token paths used by BasicLand, OffensiveSpace, Hero and the FoC `*_Land*`/`*_Space` equation files (count of uses): `Variable_Self.FriendlyForceUnnormalized` 36, `Variable_Self.IsDefender` 30, `Variable_Target.Location.EnemyForce` 31, `Variable_Target.Type.IsType` 35, `Variable_Target.EnemyForce` 20, `Variable_Target.Force` 19, `Variable_Target.FriendlyForce` 19, `Variable_Self.TacticalBuiltStructureCount` 17, `Variable_Self.BaseLevel` 16, `Variable_Target.DistanceToNearestFriendly` 16, `Variable_Target.HasBuiltObject` 14, `Variable_Target.Health` 11, `Variable_Target.DistanceToNearestEnemy` 11, `Variable_Self.HasUnit` 11, `Variable_Self.FriendlyForce` 11, `Variable_Enemy.LandedForceUnnormalized` 10, `Variable_Target.Location.FriendlyForce` 10, plus `Game.IsCampaignGame` 46, `Game.Age` 36, `Game.IsLandControlGame` 3, `Game.IsSetupPhase` 1 and ~40 rarer paths (all covered by §3.1–3.5).

Lua calls equations by name, never tokens: the tactical scripts use `Space_Area_Is_Hidden`, `Friendly_*_Unit_Raw_Total`, `Current_Enemy_Location`, `Current_Friendly_Location`, `Is_Friendly_Start`, `Is_Build_Pad`, `Allowed_As_Defender_Land`, `Needs_*_Shot`, `Should_Firesale_*`, `Should_Clear_*`, `*_Defense_Score`, `Game_Age`, `Is_Skirmish_Mode`, … (about 50 names, all through EvaluatePerception / FindTarget / Reachable_Target, AI_TACTICAL §6).

---

## 4. `TacticalPerceptionGridClass` — the threat grid

One global instance (0x140b31900), vt 0x8b4470, 0xD0 bytes, ctor 0x653340. Created at tactical mode start by the mode's FOW setup (GameMode slot 73 0x35eb40 → 0x4dcfd0 → init 0x657fc0); recreated from a save chunk by 0x4df890; deleted by 0x4dd180. Space region targets exist only when it exists (AI_TACTICAL §3.2). [H]

### 4.1 Geometry [H]
- Map extents from mode slot +0x1f8 (centre c, half-size h): `left = cx − hx`, `top = cy + hy`, `right = cx + hx`, `bottom = cy − hy` (+0xa0 left, +0xa4 top, +0xa8 right, +0xac bottom).
- Per-player FOW grids (0x4c0890) have `nFow = round(extent / DesiredFOWCellSize)` cells per axis [M: rounding by 0x22f9f0], clamped to 0..512; `DesiredLandFOWCellSize` 15, `DesiredSpaceFOWCellSize` 100 (GC+0x11c0 / +0x11c4).
- Threat grid: `nx = ceil(nFowX / k)`, `ny = ceil(nFowY / k)`, k = `AI_FogCellsPerThreatCell` (GC+0x10f8, shipped 4). `cellW = (right − left) / nx` (+0xb8), `cellH = (top − bottom) / ny` (+0xbc), half diagonal `√(cellW² + cellH²) / 2` (+0xc0). Cell index = `row × nx + col`; **row 0 is the top**: `row = (top − y) / cellH`, `col = (x − left) / cellW` (truncation toward 0, then clamp).
- So a land threat cell is about 60 units and a space cell about 400 units on a typical map (k × FOW cell).

### 4.2 Contents [H]
| Field | Shape | Meaning |
|---|---|---|
| +0x10 | cell → list of entries | **live** entries overlapping the cell |
| +0x28 | player → float[cells] | **threat layer**: projected hostile power per cell (§4.4) |
| +0x40 | player → cell → list of entries | **fog memory**: entries the player last saw in that cell (§4.7) |
| +0x58 | player → float[cells] | threat snapshot at refog (§4.7) |
| +0x70 | player → int[cells] | frame the cell last re-fogged for that player = "last seen" stamp |
| +0x88 | player → list of objects | every object registered with the grid, by owner |
| +0xc8 | uint64 | OR of the category masks of every registered type; any area query whose mask misses it returns 0 at once |

### 4.3 Entries (`PerceptionGridEntryClass`, 0xB8, ctor 0x653100) [H]
Registered by the object's **UNIT_AI behaviour (0x12)** on its first service (0x4f6070 → 0x6537d0) and on the GOM add hook (0x2b3670 → 0x4f5950). Only objects with UNIT_AI are in the grid.
- One entry per **hard point** of the object whose hard-point type is 5..10 and whose flag +0x4c is set; if there is none, one entry for the whole object (hard-point index −1).
- Fields: object, hard-point index, radius r (+0x1c), grid-relative position (x − left, top − y) (+0x20/+0x24), velocity/facing (+0x28..+0x30), area `4r²` (+0x34), centre cell (+0x38); per-player snapshots of radius, position, vector, area and centre cell (+0x40..+0xa0).
- **Radius** (refresh 0x653a50): whole object → max attack range (0x3749d0); hard point → its weapon range (hard-point type +0x238), position = hard-point world position when it is a turret type. Mobile objects add `speed × AI_{Space,Land}ThreatLookAheadTime` (GC+0x10e8 / +0x10ec, shipped 0). Then `r = max(r, half cell diagonal)`, `r = min(r, AI_{Space,Land}ThreatRangeCap)` (GC+0x1104 / +0x1108; not in the shipped XML, constructor defaults **1000 space / 500 land**, 0x2c91c0). A destroyed destroyable hard point gets r = −1 and is not projected.
- **Power**: whole object → `AI_Combat_Power` (0x370e60) of its type; hard point → hard-point value +0x88 [M: per-hard-point combat power].

### 4.4 Projection and update cadence [H]
- **Add** (0x656a10): cover the square of half-side r around the entry: columns `max(0,(x − r)/cellW) .. min(nx−1,(x + r)/cellW)`, rows likewise with cellH. Each covered cell gets `v = power × cellW × cellH / (4r²)` (the power spread uniformly over the square). For every player p for which the object is **hostile** (0x39a4e0 → relation table value 1, 0x2824f0) [M: "hostile"]: `threat[p][cell] += v`, and if the object has neither TEAM (0x16) nor HIDE_WHEN_FOGGED (0x19) also `snapshot[p][cell] −= v` [H; purpose unknown, see open item 3]. The entry is appended to the cell's live list.
- **Remove** (0x656d10): the same cells; `threat −= v` and (same condition) `snapshot −= v`, each clamped at 0; unlinked from the live lists.
- **Re-projection cadence**: the UNIT_AI behaviour service checks the object every service, but the grid work is gated by a per-object next-frame stamp that advances by **300 frames (10 s)**. At that check, if the object's 3×4 transform (obj +0x248..+0x274) differs from the stored copy, every entry is removed, refreshed and re-added (0x659760); otherwise nothing happens. **Units can therefore be up to 10 s away from their grid footprint**, and the power used is the one at projection time. [H]
- Object destroyed (grid slot 1 0x6585f0, signal): removed from the live lists and from the owner list; for every player that currently has the object marked fogged, removed from that player's memory lists. Owner change: 0x658930.
- **No decay**: values change only by remove/add. `AI_SpaceThreatDecayStep` belongs to DAMAGE_TRACKING (AI_TACTICAL §6.3), not to the grid.

### 4.5 Cell force (0x6545d0) — used by force tokens and Get_Most_Defended_Position [H]
`CellForce(cell, mask, Self, allyFlag, A, excludeObj, factionFilter)` = Σ over the cell's **live** entries, then over Self's **memory** entries of the cell, of `power / area × cellW × cellH × (1 − (1 − health) × A)` for entries whose object:
- is not `excludeObj`; matches `factionFilter` (owner's faction) if given; has a type category intersecting the mask;
- has an owner, whose faction is not `Is_Neutral` (faction +0x108);
- has `relation(owner → Self) == 0` (allied, including Self) when allyFlag = 1, `≠ 0` (enemy or neutral relation) when 0;
- live entries: skipped if the object has UNIT_AI and is currently marked fogged for Self (§4.7);
- memory entries: the decompiled condition is the **same** as for live entries (counted when **not** fogged) [M], unlike the concentration and bitfield queries which count memory entries only while fogged. See open item 3.

Power is re-read now (type combat power or hard-point value) but the area is the entry's current 4r². Area queries sum CellForce over the rect's cells (rows top→bottom, columns left→right) or over the zone cell list (list order), then multiply by the area-threat scale factor (§3.3). Summation is in float in that order.

### 4.6 Point / zone danger (pathing and `Location.ForceUnnormalized`) [H]
- **Point** (0x653ff0, player p): `threatLayer × S / (cellW × cellH)` at the point's cell, S = 4000² (space) or 800² (land) — the hostile power density scaled to a 4000- / 800-unit square. Layer = live `threat[p]` if the cell counts as visible to p (0x656fe0), else `snapshot[p]`. Outside the grid → 0.
- **Zone** (0x653e90): Σ over the zone's cells of the same layer choice, unscaled. Used by `ZoneReachabilityClass` (0x5a7390, 0x5a7880), the land path finder (0x6940e0) and LandMovementBlockStatus (0x5a6940) — this is the threat that reachability tolerances (AI_TACTICAL §3.4) compare against.
- Visible test 0x656fe0: for a **non-human** p with `AIUsesFogOfWarSpace/Land` = False (shipped, GC+0x2ed / +0x2ee) the cell is always "visible" → live layer. Otherwise the cell is visible iff its live list holds an object with UNIT_AI that is not marked fogged for p.

### 4.7 Fog memory [H unless tagged]
- Per object and player, UNIT_AI keeps a "fogged for p" byte (data pack +0x58). It is updated on each UNIT_AI service only when the global FOW-active flag (0x140a284c4: set by the land mode when fog is on, cleared by the space mode, toggled by `FOW.Disable_Rendering`) is set and the object has TEAM or HIDE_WHEN_FOGGED; otherwise all bytes are 0.
- Fogged test = 0x35f470(p, object, force = 0): false for a non-human p while `AIUsesFogOfWar{Space,Land}` is False. **So with shipped data the AI players never have fogged objects**, and humans do.
- Not fogged → fogged: copy the entry's live radius / position / vector / area / centre cell into p's snapshot (0x657ce0). Fogged → not fogged: remove the entry from p's memory lists using that snapshot (0x657db0).
- **Refog event** (grid slot 3 0x657bb0, called from the FOW decay 0x4c1670 when one of p's FOW cells goes from seen to fully fogged): for the threat cell containing it: `lastSeen[p][cell] = F`; clear p's memory list of the cell; copy every live entry of the cell into it; `snapshot[p][cell] = threat[p][cell]`. The FOW decay runs for every player's FOW grid, AI players included [M].

### 4.8 Determinism notes [H]
- Entry lists are intrusive multi-linked lists in insertion order; the order of `Σ` in §4.5 is that order. Keep the same order (append on add, unlink on remove) to get bit-identical float sums.
- Per-player loops are in player list order. Object lists +0x88 are in registration order.
- Everything here runs on the sim thread inside object service (UNIT_AI) or AI evaluation; queries are pure reads and may run in parallel only if no projection happens concurrently.

---

## 5. Lua functions that read the grid or combat power

| Function | Behaviour | Conf |
|---|---|---|
| `Get_Most_Defended_Position(src, player)` (0x69f3a0 → 0x655400 space / 0x6556c0 land) | cell score = `CellForce(cell, mask, player, allyFlag = 1, A = 0, no exclusion, no faction)` (§4.5) — the **allied** force of `player`, no area-scale factor. Max over the scanned cells; exact ties → one `Get_Float01` draw, switch if > 0.5 (AI_TACTICAL §6.4). Cell centre = `(left + (col + 0.5) cellW, top − (row + 0.5) cellH, 0)` | H |
| `TaskForce.Get_Self_Threat_Max()` (0x5fcda0) | max over the task force's units (TF +0x78 list, alive, with a type) of `AI_Combat_Power(type)` (0x370e60); 0 if empty | H |
| `TaskForce.Get_Self_Threat_Sum()` (0x5fcec0) | the sum of the same values | H |
| `Project_By_Unit_Range(unit, pos)` (0x69f6f0) | tactical only. `d = normalise(pos − unitPos)` using the **fast inverse square root** (magic 0x5f375a86, two Newton steps; bit-exact port required); `R = max(max attack range of the unit (team: its leader 0x4ec820), weapon range of every live hard point of type 5..10)`; result = `pos + d × R`, each axis clamped to the map extents. Identical positions → warning, no result | H |
| `unit.Is_Good_Against(x)` (0x57bb60) | for every category bit of x's type, look up the **contrast table** (PGAICommands `EnemyContrastTypes`/`FriendlyContrastTypes`, 0x5f1f90, BATTLE_TRANSITIONS §contrast) value of the caller's type against that category; true iff the maximum is **> 1.0** | H (M: the per-bit key argument was dropped by the decompiler) |
| `FindDeadlyEnemy`, `FindTarget`, `Reachable_Target` | AI_TACTICAL §6 | — |

---

## 6. Partial findings for the secondary items

- **Event names** [H]: global name strings are built by static initialisers at 0x1b180–0x1b320 (`Current_Target_Owner_Changed`, `Hardpoint_Opportunity_Target_Acquired`, `Hardpoint_Target_In_Range`, `No_Units_Remaining`, `Original_Target_Owner_Changed`, `Production_Failed`, `Special_Weapon_Online`, `Unit_Ability_Cancelled`, `Unit_Ability_Finished`, `Unit_Ability_Ready`, `Unit_Destroyed`, `Unit_Diversion_Finished`, `Unit_Move_Finished`, `Unit_No_Threat`), 0x1c5f0–0x1cc00 (`Space_Conflict_Begin/End`, `Fleet_Destoyed` [sic], `Reached_System_On_Path`) and 0x1ccf0–0x1ce60 (`Current_Target_Destroyed`, `Original_Target_Destroyed`, `Target_In_Range`, `Unit_Damaged`, twice). Event factories are registered by code (e.g. 0x1cc20 registers code 0x815 in the registry 0x140b09e08). The code ↔ name table itself is not resolved (open item 5).
- Movement block-status completion and reinforcement landing positions: not reached in this pass (open items 6, 7). The zone-threat function they use is §4.6.

---

## 7. Open items

1. Bodies not decompiled (Ghidra did not create functions): `IsBuildPad` 0x733830, `IsCashPoint` 0x6d71b0, `CashPointValue` 0x6d7170, `IsValidBombingTarget` 0x6d72c0, `AdditionalPopulationCapacity` 0x730dc0, `IsRetreating` 0x735ea0, `IsLandControlGame` 0x6e2060. Define functions there and decompile, or probe.
2. (Settled in A1_READINGS §20-§23, 2026-10-31.) Hint evaluator (`Hints.<name>`, 0x6deb90 / 0x8c16e0) value rule; `Type.IsType` sub-evaluator wiring for tactical unit targets; `HardPointHealth`, `AreEnginesOnline`.
3. (Settled in A1_READINGS §10-§11, 2026-10-31: (a) confirmed [H]; (b) loop order and gates recorded.) **Fog-memory counting.** (a) In CellForce (0x6545d0) memory entries are counted under the same "not fogged" test as live entries; concentration (0x656800) and bitfield (0x654d90) count them only while fogged. With shipped data AI players never have fogged objects, so every entry copied into an AI player's memory at a refog is counted a **second time** in that player's force sums until the cell refogs again or the object dies. Confirm the branch at the instruction level and with a probe (AI force token vs visible units after walking a unit out of view). (b) The meaning of `snapshot −= v` for non-hiding objects in add/remove.
4. `relation ≠ 0` treated as "enemy" everywhere (includes neutral-relation players with a non-neutral faction); confirm relation values (PHASE2_ANSWERS Q5).
5. Event code ↔ name table (shared with AI_GALACTIC open item 8): the registry 0x140b09e08 and the initialisers near 0x1cc20 / 0x1cd70.
6. LandMovementBlockStatus / SpaceMovementBlockStatus completion rules (AI_TACTICAL open item 3).
7. Reinforcement landing positions 0x6eaf60 and the space reinforcement FOW sampler (AI_TACTICAL open item 4).
8. (First-service frame settled in A1_READINGS §14, 2026-10-31.) UNIT_AI behaviour service rate (whether the fogged-byte update runs every frame) and the first-service frame of the 300-frame gate.
9. Probe (Gabriel launches; kit under `tools/probe_mod/`): a land and a space skirmish logging, every 30 frames, `EvaluatePerception` of mod equations wrapping `Variable_Target.EnemyForceUnnormalized`, `FriendlyUnitConcentrationUnnormalized`, `TimeLastSeenUnnormalized`, `DistanceToNearestEnemy` for a few targets, plus `Variable_Self.EnemyForceUnnormalized` and `Game.ForceUnnormalized`, with unit positions — validates §3.3, the 10 s re-projection lag, the cache lifetimes and item 3.

---

## 8. What A1 needs (summary)

1. **Leaf cache** (§1.2) inside the S2 evaluator: per leaf, keyed by the binding set, lifetime in seconds (table in §3), stamp-0 rule. Without it values and decisions diverge.
2. **Normalised tokens** (§1.4) as recursive path evaluations: `num == 0 → 0`, `num ≤ den ? num/den : 1`.
3. **Evaluator tree** (§2): Game, one player evaluator per player, LandUnit/SpaceShip for every target object (structures included), LandLocation per hint zone, SpaceLocation per cell, `.Location` bound to the unit (land: follows the unit's hint zone, 0 outside zones), `.Owner` frozen at registration.
4. **Threat grid** (§4): build it from the FOW grid size (k = 4), register UNIT_AI objects (per hard point of type 5..10 or whole object), project power over a (2r)² square into the per-player hostile threat layer, re-project at most every 300 frames and only when the transform changed, keep the live / memory lists in insertion order, implement the refog snapshot. AI evaluations always use an AI player as Self, and with shipped data AI players are never fogged, so the "fogged object" branches can come later; the refog memory lists cannot, because of the counting quirk in open item 3.
5. **Tokens first** (by shipped usage): Location `F/EForceUnnormalized` and the normalised Force family, player `F/EForceUnnormalized` (omniscient sums over the grid's object lists), `Game.ForceUnnormalized`, `IsDefender`, `BaseLevel`, `TacticalBuiltStructureCount`, `HasBuiltObject`, `DistanceToNearest*`, `Health`, `HasUnit`, `IsFaction`, `Age`, `IsCampaignGame`; then concentration, bitfields, time-last-seen, start locations.
6. **Lua**: Get_Most_Defended_Position cell score = allied CellForce; Get_Self_Threat_Max/Sum = max/sum of AI_Combat_Power; Project_By_Unit_Range with the fast inverse square root; Is_Good_Against via the contrast table.
7. Reuse S2 unchanged for parsing, `#`, Function_/Script_ references and token ids; the zone-threat function (§4.6) also feeds the existing `zone_reachability` and land path-finder threat costs.

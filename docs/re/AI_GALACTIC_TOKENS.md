# Galactic perception tokens: leaf semantics (StarWarsG.exe FoC x64, all RVAs)

Tags: [H] read from code/data, [M] one inferred link, [L] unverified. No decompiled code reproduced. Shared machinery
(leaf cache 0x64d740/0x64d990, parameters/defaults, 0x4de270, path entry 0x4dd1d0) is in AI_TACTICAL_PERCEPTION §1 and
GALACTIC_ECONOMY §6 and is not repeated. Notation: D = planet data pack (planet object +0xB8); GC+x = GameConstants
(base 0x140b15800); "power(T)" = `AI_Combat_Power` 0x370e60; FPS = 30; "Self" = the AI player that owns the perception
system (bindings → context +0x50) [M]; "target list" = the system's one-PlanetAITargetLocation-per-planet list (system
vf+0x40, AI_GALACTIC §2.2). GC fields by XML order [M]: +0xF0 `MaximumPoliticalControl`=5, +0xF4
`MaximumStarbaseLevel`=5, +0xF8 `MaximumGroundbaseLevel`=5, +0xFC `MaximumSpecialStructures`=5, +0x108
`MaximumFleetMovementDistance`=100.0, 0x140b168c8 `Max_Ground_Forces_On_Planet`=10. Behaviour ids decoded from the
behaviour registry table at 0xa2b330 [H]: 0x5D DUMMY_GROUND_COMPANY, 0x5E DUMMY_SPACE_FIGHTER_SQUADRON, 0x5F
DUMMY_GROUND_STRUCTURE, 0x60 DUMMY_ORBITAL_STRUCTURE, 0x61 DUMMY_STARSHIP, 4 FLEET, 5 TRANSPORT, 0x16 TEAM (type
bitset +0x790 tested by 0x370320).

## 0. New shared mechanics

**Leaf kinds** [H]
- *Visible leaf* (0xB0 B; built inline or by 0x659bd0 / 0x665ec0(node,&pmf,L)): cache with lifetime L seconds, then compute.
- *Fogged leaf* (0xE0 B; 0x659ab0 planet, 0x6d83c0 planet-force; vt 0x8b4638 / 0x8c1608, evaluate 0x65aa20). Extra
  fields: visibility mask M (+0xD0), visible flag (+0xC0), last-seen frame (+0xC8, double). It listens to signal 0xE
  (visibility, listener 0x661a50): if `(event mask & M) == M` → recompute all cached entries (0x65ac40) and visible=1;
  else if it was visible → recompute, visible=0, last-seen=now. The planet evaluator forwards 0xE only when the event's
  player is the evaluator's player (0x661b00).
- `IsVisible` (fogged vf+0x48 = 0x660f80): true if visible flag set **or** the mode's fog option is off: mode 0 →
  GC+0x2EC `AIUsesFogOfWarGalactic`, 1 → +0x2EE Land, 2 → +0x2ED Space [H for code, M for the field names]. Shipped
  GameConstants: all three **False** [H] → every fogged leaf behaves as live.
- Fogged evaluate 0x65aa20 [H]: if the next path token is `TimeLastSeen`: visible → 0.0; else (now − lastSeen)/now
  (now = global frame 0x140b0a320). `TimeLastSeenUnnormalized`: visible → 0.0; else (now − lastSeen)/FPS seconds. Any
  other continuation: visible → normal cache/compute and, on success, lastSeen = now; not visible → fogged path
  0x65e380 = **last cached value for this binding set regardless of age**, or a live compute if nothing was ever
  cached (also stored). `TimeLastSeen*` is only reachable as a suffix of a fogged leaf (e.g.
  `EnemyForce.SpaceTotal.TimeLastSeen`); `Target.TimeLastSeen` alone is not registered (fails).
- **Fog answer for the port:** with `AIUsesFogOfWarGalactic=False` the AI uses **live values**, every
  `*.TimeLastSeen*` = 0.0, and `AverageAgeOf*Intelligence` = 0.0 [H]. Visibility masks (only relevant with fog on):
  planet: 0x400 political control, 0x200 structures, 8 income/routes, 0x80 garrison slots, 1 owner/TargetPlayerID/
  IsFaction/TimeSince*; force: 0x80 ground units, 0x20 space units, 0x10 space presence, 0x40 maintenance, 4 ground
  base, 2 starbase [H masks, M meaning].

**Planet event stamps** (planet evaluator listener 0x661b00) [H]: signal 0x12 → +0xD8 = frame (space conflict), 0x14 →
+0xDC (ground conflict), 0x1A → +0xE0 (conversion; also rebinds Owner). Initialised to −1 (ctor 0x659cf0).

**Neighbour lists** [H]: planet evaluator +0xC8/+0xD0 (built 0x661da0) = every other planet whose 3-D distance ≤
GC+0x108 **or** that is connected by a route (0x3f7b30), self excluded. The planet-force evaluator's own list
(+0xB0/+0xB8, 0x6ddfb0) is built the same way but **includes the planet itself**.

**Force side** (PlanetEnemy vt 0x8c1390 / PlanetFriendly vt 0x8c1458) [H]:
- object filter vf+0x58: Enemy = 0x39a4e0 (owner's relation to Self == 1, hostile); Friendly = 0x39a2c0 (relation == 0:
  self or ally). Unowned objects match neither. For the planet itself the owner is D+0x6C.
- vf+0x68 own-side token (EnemyForce / FriendlyForce), vf+0x70 opposite side, vf+0x80 "add planet-type list"
  (Enemy true, Friendly false).

**Normalisation suffixes** (all on PlanetForce, X = GroundTotalUnnormalized or SpaceTotalUnnormalized) [H].
Numerator = `Target.<side>.X` with the caller's parameters (0x6dba00). Result: num = 0 → 0; num ≤ den → clamp(num/den,
0, 1); num > den → 1.0. Fails if any path fails.

| suffix | denominator path | meaning |
|---|---|---|
| (none) `GroundTotal`/`SpaceTotal` (0x6dc250) | `Game.<side>.X` + `Game.<opposite>.X`, caller params copied | share of all forces in the galaxy |
| `NBTD` (0x6dc6b0) | `Game.<side>.X`, params copied | share of that side's galactic total |
| `NBPTM` (0x6dc6b0) | `Game.<side>.PerTarget­Max{Ground,Space}Force`, params copied | relative to the strongest planet of that side |
| `NBTT` (0x6dc940) | `Target.<side>.X` **without** caller params | fraction of the force here that matches the category filter |

`Game.<side>.X` (GameForce 0x6e3a10) = Σ over the target list of `Target(p).<side>.X` evaluated **with fresh bindings
(caller params dropped)**; `PerTargetMax*` (0x6e36a0) = max of the same. Units in transit between planets are not
counted. Lifetime 30 s.

Player-side normalisers: `NBTP` = by the max over planets with the same TargetPlayerID; `NBTF`/`NBPTM` on income: see §3.

## 1. Target root = planet (Planet evaluator 0x65f7b0; base 0x6d8100 → 0x64dc60 Markup + 0x6d8040 Type)

Life = cache seconds (−1 forever). Fog = fogged leaf mask, "vis" = visible leaf. Fail = leaf returns false.
Every leaf fails when the evaluator has no planet object.

| token | leaf | life | fog | computation | params / failure |
|---|---|---|---|---|---|
| TargetPoliticalControl | 0x65f000 | 0 | 0x400 | influence record of behaviour 3 (vf+0x98): v = (int)\|control\| / GC+0xF0; negated when the record's player ≠ Self [H; M Self] | none |
| TargetPlayerID | 0x65e980 | 0 | 1 | player id of the same influence record (−1 if none) [H] | — |
| StarbaseLevel / …Unnormalized | 0x65ea30 / 0x65eab0 | 0 / 0 | vis | D+0x148 / GC+0xF4; unnormalised = D+0x148 | — |
| GroundbaseLevel / …Unnormalized | 0x65c1a0 / 0x65c220 | 0 | vis | D+0x130 / GC+0xF8 | — |
| MaxStarbaseLevel / …Unn | 0x65e490 / 0x65e4f0 | 0 | vis | 0x372780: D+0x238 (type +0x668 without D) / GC+0xF4 | — |
| MaxGroundbaseLevel / …Unn | 0x65e3e0 / 0x65e440 | 0 | vis | 0x372400 (D: 0x4b7eb0; else min(5, type+0x92C)) / GC+0xF8 | — |
| MaxStructureSlots / …Unn | 0x65e540 / 0x65e5a0 | 0 / 0 | vis / 0x200 | D+0x230 (type+0x92C) / GC+0xFC | normalised fails if GC+0xFC = 0 |
| OpenStructureSlots | 0x65e840 | 15 | 0x200 | 1 − used/total; used = (D+0x130>0 ? D+0x160 : 0) + (D+0x148>0 ? D+0x178 : 0); total = D+0x230 + D+0x234 (ground + space); total 0 → 0 | — |
| OpenStructureSlotsUnnormalized | 0x65e900 | 0 | 0x200 | D+0x230 − (D+0x130>0 ? D+0x160 : 0) (ground only) | — |
| OpenGroundCompanySlots | 0x65e800 | 13 | 0x80 | `Max_Ground_Forces_On_Planet` − count of D+0xD8 list (surface forces) [M const] | — |
| BaseIncome | 0x65aec0 | 15 | vis | base(P)/max base over the target list, clamped [0,1]; base = 0x3733c0 (GALACTIC_ECONOMY §1.4); max 0 → 0 | fails if no target list |
| BaseIncomeNBTP | 0x65b450 | 15 | vis | base(P) / max base over planets whose `TargetPlayerID` equals this planet's, clamped | fails if any TargetPlayerID fails |
| BaseIncomeNBTD | 0x65b000 | 17 | vis | not read (rare) | — |
| IncomeUnnormalized | 0x65d6e0 | 15 | 8 | Planet_Income(P, gross=0) 0x3f68b0 (float) | — |
| Income / IncomeNBPTM / IncomeNBTP / IncomeNBTD | 0x65c8e0 / 0x65cb60 / 0x65d240 / 0x65cd80 | 15/15/15/17 | 8 | not read (unused by shipped galactic eqs) | — |
| TradeRoutes | 0x65f200 | −1 | vis | routes(P) (D+0x60) / max routes over the target list; 0 routes → 0 | fails without target list |
| ActiveTradeRoutes | 0x65ae20 | 15 | 8 | routes passing 0x39a2c0(route, planet owner) / routes(P) (0 if none) | — |
| ConnectsIsolatedPlanetsByForce / …ByLength | 0x65bb30 / 0x65bbd0 | 0 | vis | flag +0x205 / +0x204 of the planet's AI target data; set by island analysis 0x4e4850 (planets on the force-cost path, 0x3ef6a0 mode 0, resp. length path mode 1, from each owned island to the others; only when Self has ≥ 2 islands) | — |
| ConnectsLargestIslands | 0x65bc70 | 0 | vis | flag +0x206: planets on the path between the two largest islands (0x4e4850) | — |
| IsConnectedTo | 0x65d830 | 5 | vis | over the neighbour list n: match if n's **planet type name** ∈ `Parameter_Type` **or** (`Parameter_Faction` given and `Target(n).IsFaction{same params}` > 0). A = `Parameter_Attenuator` (default **0.0**): A ≤ 0 → 1.0 on first match; else max over matches of 1 − (d/GC+0x108)·A, d = distance, 0 if a direct route exists | fails if a nested IsFaction fails; no type and no faction → 0 |
| IsFaction | 0x65e090 | 0 | 1 | 1.0 if the political owner's (0x3956c0 +0xC) faction name ∈ `Parameter_Faction` | fails if `Parameter_Faction` absent |
| HasStarbaseOfLevel / HasGroundbaseOfLevel | 0x65c820 / 0x65c420 | 0 | vis | L = (int)(`Parameter_Level` (default 1) + 0.5); 1.0 if D+0x148 (resp. D+0x130) ≥ L | fails if L ≤ 0 |
| HasStructure | 0x65f3b0 | 13 | 0x200 | 1.0 if any object in D+0x168 or D+0x150 lists has a type named in `Parameter_Type`, or (unless `Parameter_Only_Consider_Complete` = 1) the production queue's current item (behaviour 6, 0x401960) does. **No owner filter** | fails if `Parameter_Type` absent, or (complete≠1) no production behaviour |
| StructureCount | 0x65ebd0 | 2 | vis | count of D+0x168 + D+0x150 objects whose type ∈ list, + (unless complete-only) queued count per type 0x401ac0 | as HasStructure |
| HasCreditSiphon | 0x65f350 | 0 | vis | D+0x200 ≠ −1 | — |
| IsSurfaceAccessible | 0x65e320 | 0 | vis | type test 0x375220 | — |
| IsCorrupted | 0x65e010 | 3 | vis | 1.0 if D+0x2F4 ≠ −1; otherwise the output is **not written** (stays at the caller's initial value, 0.0 at all known sites [M]) | — |
| IsConnectedToCorruption | 0x65df70 | 4 | vis | 1.0 if any planet-evaluator neighbour is corrupted, else 0.0 | — |
| IsCorruptionTransitionActive | 0x65e050 | 2 | vis | 1.0 if D+0x2E2 or D+0x2E3; else output not written | fails if no planet |
| TimeSinceCorruptionChange | 0x65f110 | 0 | vis | (mode frame − D+0x2F0)/FPS | fails if no D |
| RetainsResidualInfluence | 0x65e9f0 | 0 | vis | type byte +0xBA ? 1.0 : 0.0 | — |
| TimeSinceSpaceConflict / Ground / Conversion | 0x65f1b0 / 0x65f160 / 0x65f0c0 | 0 | 1 | stamp < 0 → **1e18** (float 1e18 as double); else (now − stamp)/FPS seconds | — |
| BlackMarketAbilitiesAvailable | 0x65c280 | 5 | vis | if D+0x2F4 == 6 (Black_Market): 1.0 if some ability (table 0x140a2aa78) owned by the planet owner's faction is not yet used by the `Parameter_Faction` player (0x282070); else 0 | fails if faction param absent/unknown |
| BlackMarketMinimumAbilityPrice | 0x65e5e0 | 6 | vis | min price of those abilities (×(+0x28) when above tech) + type float +0x800; none → 1e18 + modifier | as above |
| BlackMarketPriceModifier | 0x65b7d0 | −1 | vis | type float +0x800 | — |
| IsHumanControlled | 0x65e290 | 0 | vis | owner player (planet +0x58) human flag (+0x62) | — |
| HasIndigenousUnits, StoryTrigger, DistanceToNearestFriendly, connectivity, BuildQueue*, IsBuilding | various | — | — | not used by shipped galactic eqs; not read | — |
| **Markup** | 0x64dc50 (base 0x64dc60, every evaluator) | 0 | vis | evaluator double +0x80, accumulated by Lua `Apply_Markup` (0x69d110, re-apply subtracts the old value) [H leaf, M writer]; default 0 | — |
| **Hints.X** | sub-evaluator 0x6deca0 (HintPerceptualEvaluator, evaluate 0x6dee80) | none | — | value of hint X for this planet from the campaign's `Markup_Filename` file (Data/XML/AI/GalacticMarkup/*), else the hint-set default (`Galactic_Hints` in HintSets: Chokepoint, OffenseDelayGround/Space, PriorityTarget = 0.0) via 0x4dd6d0 | **fails** if X is in neither (e.g. `Hints.High_Ground` in galactic) |
| Owner.X | sub 0x6df710 / 0x6df790 | — | — | Owner → the owning player's evaluator, rebound on 0x1A [M] | — |
| Type.Token | sub from 0x6d8040 (0x4dd450(type-name token)) | — | — | per-type evaluator; only `Is_Home_Planet` uses it [L] | — |
| FriendlyForce / EnemyForce | sub 0x6d8ef0 / 0x6d8e60 | — | — | §2; the bare token (no leaf) fails (0x64d890) [H] | — |

## 2. Target.FriendlyForce / Target.EnemyForce (PlanetForce 0x6dcb70)

Fogged leaves unless "vis". Category = `Parameter_Category` (default all bits) against type mask +0x1648 (structures:
mask +0x1658, defaulting to the `Capital` category mask 0x140b3a9d0). `per-tech members` = the type's per-tech-level unit
list (0x373f40 count / 0x373e30 type / 0x373d30 count, tech = owner player +0x84), each adding power × count.

| token | leaf | life | mask | computation | fails |
|---|---|---|---|---|---|
| GroundTotalUnnormalized | 0x6d95d0 | 15 | 0x80 | Σ over surface list D+0xD8 (skip obj +0x3A0 bit 2) passing the side filter: power(0x3973b0(obj)) if category; + if the planet itself passes the side filter: per-tech members of every ground structure in D+0x158; + if total > 0 and side = Enemy: Σ int field (+0x10) of planet-type list +0x20C8 entries whose type is affiliated with any player hostile to Self (0x3751a0) [H; M "garrison/indigenous" meaning] | no planet / no D+0xD8 |
| SpaceTotalUnnormalized | 0x6dbd50 | 15 | 0x20 | for each orbit object (0x395900) that is a FLEET whose first unit passes the side filter: each unit's type power (category) + its per-tech members; + if the planet passes the side filter: starbase type (D+0x140, owner D+0x74) per-tech members (not the starbase's own power), + ground structures of D+0x158 whose type byte +0x1E is set, + orbital structures D+0x170 (count +0x178), structure masks per +0x1658 | no planet / orbit list / D |
| GroundTotal, SpaceTotal | 0x6d9560, 0x6dbce0 → 0x6dc250 | 15 | 0x80/0x20 | §0 table | path failures |
| *NBTD / *NBPTM / *NBTT | 0x6d95a0 / 0x6d9570 / 0x6d95c0 (space 0x6dbd20 / 0x6dbcf0 / 0x6dbd40) | 15 | 0x80/0x20 | §0 table | path failures |
| NearbyGroundTotal / NearbySpaceTotal | 0x6db240 / 0x6db250 → 0x6db260 | 30 | vis | S = Σ over force neighbours n (incl. self) of (1 − d_n/GC+0x108·(1 − A))·`Target(n).<side>.X{params}`, A = `Parameter_Attenuator` default **1.0** (no attenuation); result = S / (`Game.<side>.X` + `Game.<opp>.X`), no clamp; den 0 → 0 if S = 0 else fail | any path fails |
| HasSpaceForce | 0x6d9f80 | 7 | 0x10 | 1.0 if any orbit object (skip +0x3A0 bit 2 and 0x3a9760) passes the side filter and (if `Parameter_Faction` given) its owner's faction name matches | no planet / orbit list |
| HasGroundForce | 0x6d9930 | 7 | 0x80 | same on D+0xD8 | no planet / list |
| HasNearbyGroundForce / Space | 0x6d9f60 / 0x6d9f70 → 0x6d9bd0 | 11 | vis | 1.0 if any force neighbour n (incl. self) has `Target(n).<side>.Has…Force{params}` > 0 | a nested failure before a hit |
| StarbaseTotalUnnormalized | 0x6dc1c0 | 0 | 2 | if the planet passes the side filter and D+0x140 ≠ 0: power(starbase type) | no D |
| GroundbaseTotalUnnormalized | 0x6d9470 | 0 | 4 | if the planet passes the side filter: Σ power(type) of D+0x158 structures + garrison power 0x4b8000(D, all) | [M] no D |
| HasSpaceUnitsBitfield / HasGroundUnitsBitfield | 0x6da230 / 0x6da7d0 | 13 | 0x10/0x80 | bit i set if a side unit (orbit, resp. surface) has the i-th `Parameter_Type` type; value = bitmask as double [M] | — |
| AntiStealthEffectiveness | 0x6d9180 | 13 | 0x80 | Σ per-unit value 0x395830 over side units in orbit and on the surface [M] | — |
| Maintenance(…Unnormalized) | 0x6dabc0 / 0x6daf20 | 15 | 0x40 | not read | — |

The Node_Weight / danger consumers of GALACTIC_ECONOMY §6.2 read these same leaves (and caches).

## 3. Player roots (GalacticPlayer 0x66b9f0, base Player 0x6e09c0)

Each player has its own evaluator in Self's system. "Owned planets" = target-list planets whose `TargetPlayerID`
(evaluated through each planet's evaluator, so fogged/cached as in §1) equals the player id (+0x4C); every such
nested evaluation that fails fails the token.

| token | leaf | life | computation | params / failure |
|---|---|---|---|---|
| PlanetsControlledUnnormalized | 0x66af70 | 3 | number of owned planets ((int)(id+0.5) compare) | fails on a TargetPlayerID failure |
| PlanetsControlled | 0x66aef0 | 3 | above / size of target list (all planets) | — |
| SpaceTotalUnnormalized | 0x66b1c0 | 10 | over the player's GOM objects (0x2acb70): T = 0x3973b0(obj); if T is DUMMY_STARSHIP/DUMMY_SPACE_FIGHTER_SQUADRON: + power(T) when mask +0x1648 matches; else + power(T) when mask +0x1658 (default Capital) matches and (T+0x1E set or T is not DUMMY_ORBITAL_STRUCTURE); + per-tech members (0x373fd0/0x373ee0) matching +0x1648. Includes units in transit [M] | `Parameter_Category`; fails without player |
| GroundTotalUnnormalized | 0x6682c0 | 10 | same object walk; + power(T) only for DUMMY_GROUND_COMPANY types with matching +0x1648; + per-tech members | same |
| SpaceDefendedPlanets / GroundDefendedPlanets | 0x66b1b0 / 0x6682b0 → 0x667bf0 | 5 | average over owned planets of `Target.<F|E>.Has{Space|Ground}Force` (FriendlyForce if this player is Self, else EnemyForce): fraction of the player's planets with that presence; 0 owned → 0 | nested failure |
| MaxedStarbases / MaxedGroundbases | 0x66a3c0 / 0x669f40 → 0x669f60 | 5 | fraction of owned planets where `StarbaseLevel` == `MaxStarbaseLevel` (ground: Groundbase pair). A planet with max 0 counts as maxed | nested failure |
| MaxStarbaseLevel / …Unn, MaxGroundbaseLevel / …Unn | 0x669f20 / 0x669f30, 0x669f00 / 0x669f10 → 0x66aa80 | 4 | **max current level** over owned planets of `Target.StarbaseLevel` (normalised) / `StarbaseLevelUnnormalized` (ground same) | fails without target list |
| HasStarbaseOfLevel / HasGroundbaseOfLevel / HasStructure | 0x668a70 / 0x6689f0 / 0x668a30 | 3 | NumX > 0 ? 1.0 : 0.0 (see next) | — |
| NumStarbaseOfLevel / NumGroundbaseOfLevel / NumStructure | 0x66aa70 / 0x66aa50 / 0x66aa60 → 0x668490 | 3 | n = owned planets where `Target.<HasStarbaseOfLevel / HasGroundbaseOfLevel / HasStructure>{caller params}` > 0; result = max(0, n − (int)(`Parameter_Count`(default 1) + 0.5) + 1) | nested failure (e.g. HasStructure without `Parameter_Type`) |
| StructureCount | 0x66b3c0 | 4 | Σ over owned planets of `Target.StructureCount{caller params}` | nested failure |
| WorstIslandFractionOfLargest | 0x66b9a0 | 0 | only for Self (others 0.0): **second-largest / largest** island size from 0x4e4850 (system +0xEC / +0xE8); fewer than 2 islands → 0.0 (0x4e1e10) | — |
| AnyCurrentThreats | 0x666900 | 15 | human player → 0.0. Else 1.0 if some **human-owned** planet has a **human-owned** object in orbit and one of that planet's evaluator neighbours is owned by this player | — |
| AverageAgeOfSpaceIntelligence / …Ground… | 0x666af0 / 0x666de0 → 0x666b00 | 6 | Self only: average over planets not held by Self or an ally (0x4ad9f0) of `Target.EnemyForce.<X>.TimeLastSeen`. Note the swap: **Space uses GroundTotal, Ground uses SpaceTotal** [H]. With fog off → 0.0 | **fails** if the player is not Self; nested failure |
| BudgetFractionToBuild | 0x667120 | 5 | Self only (others 0.0). `Parameter_Type` (one type), `Parameter_Category` = budget category (default all). R = type cost 0x3711c0, reduced for an item of this type already in progress for Self ((1 − progress)·cost, minus spent 0x692ca0); D = build time 0x371230 (min'd with remaining) × category income rate (0x6134c0, or budget +0x68 for all) + (nothing in progress) category available credits (0x6135e0, or player +0x70). Result R/D; R < 0.0001 → 0; D ≤ 0 → 1e18 [H structure, M field names] | fails without goal system / unknown type |
| CanAdvanceTech | 0x6679a0 | 6 | T = next tech-level upgrade type (0x408040(tech+1, faction)); 0 if none; 1.0 if T is queued by this player at any planet (first 2 queue slots) or 0x2804d0(player, T, planet) [M "can build here"] holds for any planet | fails without target list |
| HasTechToProduce | 0x668ab0 | 8 | 1.0 if **any** `Parameter_Type` type exists, is affiliated with the player's faction (type +0xEF0 list), has byte +0xC1 = 0 and (tech req +0x89C ≤ player tech +0x84 or 0x282580 [M special unlock]) | fails if `Parameter_Type` absent |
| GrossIncomeUnnormalized | 0x667fb0 | 7 | Σ over owned planets of `Target.IncomeUnnormalized` [M tail] | nested failure |
| GrossIncomeNBPTM / NBTF; NetIncome NBPTM / NBTF | 0x667f70 / 0x667f90; 0x66a3e0 / 0x66a400 → 0x66a730 | 7 | Gross (resp. Net) / `MaxAllPlanetsIncomeUnnormalized` (NBPTM) or `MaxOwnedPlanetsIncomeUnnormalized` (NBTF) [H args, M body] | — |
| NetIncomeUnnormalized | 0x66a420 | 7 | Gross − maintenance via player-rooted paths [M] | — |
| Maintenance / MaintenanceUnnormalized | 0x669110 / 0x669420 | 7 | Unn: neutral faction (+0x108) → 0; Self only: Σ over the player's fleets' units of upkeep (type +0x798 × faction +0x3AC) × cost/build-time/FPS [M]; Maintenance = Unn / Gross, Gross 0 → 0 [M] | — |
| TechLevel | 0x6e09a0 | 0 | player +0x84 (no null check) | — |
| IsFaction | 0x6e0700 | −1 | faction name (+0x68) ∈ `Parameter_Faction` | fails if param absent |
| IsDifficulty | 0x6e0620 | 0 | human or no AI system → 0.0; else AI system (+0x360) difficulty index (+0x58) == `Parameter_Difficulty_Level_Type` | fails if no player or param absent |
| CreditsUnnormalized | 0x254a14 → GalacticPlayer vf+0x50 = 0x667b40 | 0 | AI (has AI system, not human): galactic budget total 0x6136b0(goal system +0x190) [M "Σ pools"]; else base 0x6e05f0 (player credits) [M] | no player |
| HasUnit | 0x6dfbb8 → vf+0x48 = 0x668cd0 | 8 | n = `Parameter_Count` (int, default 1) minus owned GOM objects whose 0x3973b0 type ∈ `Parameter_Type`, minus (unless `Parameter_Only_Consider_Complete` > 0) queued counts at planets; 1.0 if n ≤ 0 | fails if `Parameter_Type` absent |
| ActiveGoals | 0x6e0490 | 0 | number of active goals whose category ∈ `Parameter_Category` mask (0x721f70 on the goal system of the current mode) | fails without AI system |
| StoryTrigger | 0x6e08e0 | 0 | 1.0 if 0x4dd530(system, `Parameter_Name`) [M story flag] | fails if name absent |
| HomePlanet | sub 0x6df790 / 0x6df650 | — | sub-evaluator of the home planet [M] | — |
| BudgetAllocation | 0x6e0560 | 2 | Self only: normalised weight of the budget category `Parameter_Category` (an AIGoalCategoryType value); §8.3 | fails if the player is not Self |
| TimeSinceSpaceDefender / …SpaceAttacker / …GroundDefender / …GroundAttacker | 0x66b950 / 0x66b900 / 0x66b8b0 / 0x66b860 | 0 | (now − stamp)/FPS seconds; never stamped → 1e18; §8.2 | — (any galactic player root) |

## 4. Game root (GalacticGame 0x66e380 → base 0x6e2170; Game.<side> = GameForce 0x6e3d40)

| token | leaf | life | computation |
|---|---|---|---|
| Age | 0x6e1f20 | 0 | frame of the mode the AI player belongs to (system → +0x28 → mode index → mode +0x10), else current mode frame; × 1/FPS → seconds |
| Timeline | 0x6e2140 | 0 | int at current mode +0x9C [M era/timeline] |
| IsSetupPhase | 0x6e2090 | 0 | mode +0x1F0 → byte +0x30; fails if no mode |
| IsCampaignGame | 0x6e1ff0 | −1 | mode type 0 (galactic) → 1.0; else 1.0 if 0x28aa00 finds a parent campaign |
| IsStoryCampaign | 0x6e20e0 | −1 | global byte 0x140b303c2 |
| IsLandControlGame | 0x6e2060 | −1 | GameModeManager +0x20 == 4 |
| PlanetsCorruptedUnnormalized / PlanetsCorrupted | 0x66e2c0 / 0x66e220 | 7 | count of target-list planets with D+0x2F4 ≠ −1; normalised by the target-list size |
| EnemyForce/FriendlyForce.{Ground,Space}TotalUnnormalized | 0x6e3680 / 0x6e3a00 → 0x6e3a10 | 30 | §0 (Σ planets, params dropped) |
| …PerTargetMax{Ground,Space}Force | 0x6e39e0 / 0x6e39f0 → 0x6e36a0 | 30 | max over planets |
| ForceVisibility | — | — | **not registered** on the galactic Game (only the tactical budget eqs use it) → fails |

## 5. Audit of kKnownPaths (engine/ai/src/galactic_ai_tokens.cpp)

**Paths the original does not register on that root (evaluation fails there):** `Target.ISCONTESTABLE`,
`Target.HASBUILTOBJECT`, `Target.TIMELASTSEEN`, `Target.TIMELASTSEENUNNORMALIZED` (only valid as a suffix),
`Target.FORCE`, bare `Target.ENEMYFORCE` / `Target.FRIENDLYFORCE`, `Target.HINTS.HIGH_GROUND` (not in
`Galactic_Hints`), `Game.FORCEVISIBILITY`. (`Target.LOCATION.*`, used by `FriendlyForceNearUnit`, is also not
registered on planets.)

**Semantic differences to fix (port → original):**
- Fog: correct to use live values; TimeLastSeen 0, AverageAge 0 (but AverageAge* fails for non-Self roots).
- `TimeSince*` never-happened value: 1e18, not 1e6; the original has real stamps (signals 0x12/0x14/0x1A).
- `PlanetsControlled*`: counted by `TargetPlayerID` (political-control record), not `owner`.
- `MaxedStarbases`: uses the planet's own max (D+0x238) and normalised equality; no default 5.
- `HasStarbaseOfLevel`/`NumStarbaseOfLevel`: n − round(Count) + 1, clamped ≥ 0; Has = (that > 0).
- `HasStructure`/`StructureCount` (player): via owned planets' structure lists + queues, not all objects.
- `HasUnit`: owned GOM objects + queued (unless complete-only), count against `Parameter_Count`. A company counts as its transport (0x3973b0 = the company type) plus each `Company_Units` member as its own object (STRATEGIC_GAPS §1.7; the port, which keeps one object per company, counts the members from the company type: q2f t01).
- `HasTechToProduce`: **any** listed type, faction-affiliated, tech ≤ player tech.
- `CanAdvanceTech`: next-tech upgrade type buildable/queued, not `tech < 5`.
- `AnyCurrentThreats`: human fleet at a human planet adjacent to ours (not hostile fleet over our planet).
- `WorstIslandFractionOfLargest`: second-largest / largest, 0 if < 2 islands.
- `BudgetFractionToBuild`: cost-remaining / (time × category rate + available), Self only.
- `SpaceTotalUnnormalized`/`GroundTotalUnnormalized` (player): per-type power incl. per-tech members, DUMMY_* rules.
- Planet force totals: orbit = fleets only (space), surface list D+0xD8 (ground) + structure garrisons, starbase members
  and orbital/space structures only when the planet's owner is on that side; enemy ground adds the planet-type list.
- Normalisations: §0 table (not "max player power"). `NearbyX`: neighbours incl. self, distance rule, both-side galactic
  denominator. `TradeRoutes`/`BaseIncome`: max over all planets; `BaseIncomeNBTP` by TargetPlayerID group.
- `IsConnectedTo`: neighbour = within GC+0x108 or routed; matches planet type or `Target(n).IsFaction`; attenuator form.
- `OpenStructureSlots(Unnormalized)`, `MaxStructureSlots`, `Max*Level`: real fields (D+0x230/0x234/0x238...).
- `Markup`: Lua `Apply_Markup` accumulator. `Hints.*`: campaign markup file + hint-set default.
- `TargetPoliticalControl`: signed (negative when controlled by another player) and divided by 5.
- `Game.TIMELINE`, `ISCAMPAIGNGAME`, `ISLANDCONTROLGAME`, `ISSETUPPHASE`: real flags (§4). `Game.<side>.X` ignores params.
- Caches: lifetimes above are part of the behaviour (e.g. force totals 15 s, Game totals 30 s).

**Registered and used by shipped galactic eqs but missing in the port:** `Self.GrossIncomeNBPTM`,
`Self.GrossIncomeNBTF`, `Self.MaxStarbaseLevelUnnormalized`, `Self.StoryTrigger`, `Self.HomePlanet.Type.Token`,
`Human.SpaceDefendedPlanets`, `Human.GroundDefendedPlanets`, `Target.OpenGroundCompanySlots`,
`Target.BlackMarketAbilitiesAvailable`, `Target.BlackMarketMinimumAbilityPrice`, `Target.BlackMarketPriceModifier`,
`Target.EnemyForce.HasSpaceUnitsBitfield`, `Target.EnemyForce.HasGroundUnitsBitfield`,
`Target.FriendlyForce.HasGroundUnitsBitfield`, `Target.EnemyForce.AntiStealthEffectiveness`, `Target.Type.Token`.

## 6. Extra questions

**(a) Variable_Human without a human** [H]
- 0x294a00(PlayerList) returns the **first player in list order with the human flag (+0x62)**, or null.
- Every galactic site binds Human only if that is non-null: budget weights 0x614590 (and init 0x6137b0), proposal
  0x6cb120 (both global and targeted branches), template trigger/untrigger 0x670be0 / 0x670cf0, FindTarget
  0x698360 / Best_Of 0x697450 / Lua `Evaluate_Perception` 0x699430. Enemy is bound only if the goal system's opponent
  index (+0x30) ≠ −1 (0x5f55c0).
- An unbound root: 0x4dd1d0 looks the first token up in the context, then the system's root map, then the global root
  map; not found → the path returns **false** (no default value).
- Consequences at the call sites (exe side [H]; a leaf failure fails the whole equation and leaves the output
  untouched, read from PerceptionFunctionG.dll in §9.1 [H, q6 2026-11-04]):
  - proposal 0x6cb120: `Evaluate` false → candidate skipped (no desire, not proposed);
  - template trigger 0x670be0: result pre-set to 0.0 and the bool ignored → trigger "> 0" is false (template not
    activated); the DLL writes no partial value (§9.1);
  - budget 0x614590: the result is pre-loaded with the **previous normalised weight** and the bool ignored → a failing
    category keeps its old weight (then negatives → 0, renormalised) [H, both sides].
- Shipped equations that need Human: `Underworld_Offensive_Budget_Allocation`, `Num_Enemy_Space/Land_Territories`,
  `Enemy_Planets_Controlled`, `Needs_More_Space_Force_Globally`, `Am_I_In_Winning_Position`,
  `Space/Ground_Force_Limit_For_Unrestricted_Grab`, `Should_Shutdown_Offense_For_Story_Campaign`.

**(b) `Script_` references** [H unless tagged]
- Load: 0x5e8220 scans `Data/Scripts/Evaluators/*.lua`; per file 0x5e7cb0 loads **one shared Lua script instance**,
  registers the token `Script_<FileName>` (0x341ce0) and stores the script at +0x88 of the global root evaluator
  0x4dd450(token) = ScriptPerceptualEvaluator (ctor 0x651fd0, vt 0x8b43e0). Shared by all AI players and modes.
- Evaluate (vf+0x8 = 0x652040, a root evaluator, **no leaf cache**; the trailing `.Evaluate` token is not checked):
  sets script globals `PlayerObject` = wrapper of Self (context +0x50) and, if the context has a target object
  (+0x48), `Target` = 0x60e0a0 wrapper; builds an argument list from `Parameter_Script_String` value(s) (a table of
  strings is expanded) followed by `Parameter_Script_Number`; calls Lua `Evaluate(args...)`; then calls
  `Evaluator_Clean_Up()` (PGBaseDefinitions.lua: Target = nil, PlayerObject = nil, then the script's `Clean_Up`).
- Result: must be a Lua number (class 0x140a157c0) → copied as double; anything else (nil, boolean) → **failure**.
- Side effects / RNG: whatever the Lua does. The only galactic one, `Script_AnyFreeLandSlots` (in
  `Allow_Blind_Ground_Production`), returns 1.0/0.0 from `FindTarget.Reachable_Target(PlayerObject,
  "Can_Park_Ground_Unit", "Friendly", "Any", 1.0)`; 0x698360 calls Weighted_Random_Pick 0x698de0 → 0x532480, so each
  evaluation **consumes synced RNG** [M: draw count per call]. Script globals persist between calls.

## 7. Open items
1. ~~PerceptionFunctionG.dll: does `Evaluate` write the output on failure, and do any operators (e.g. `*` by 0, clamp)
   swallow a failing operand?~~ No and no: §9.1 [H, q6 2026-11-04]. Not read: the Division, comparison, Clamp
   (vt 0xc7be0), Unary (vt 0xc7bb0) and Random (vt 0xc7b98) node bodies; they cannot swallow a failure either, because
   the failing node stops the run before any later operator is reached.
2. Island analysis cadence (0x4e4850 called from 0x4e2060 and 0x4e4670) and when flags +0x204..0x206 are reset (0x278404 /
   0x1104999 sites).
3. Bodies not read: GrossIncome sum tail (0x667fb0), NetIncome (0x66a420), 0x66a730, Maintenance (0x669110/0x669420),
   AverageStarbase/Groundbase (0x666df0), BaseIncomeNBTD/IncomeNBTD, bitfield/antistealth details, HomePlanet/Owner/Type
   sub-evaluators.
4. Meaning of the planet-type list +0x20C8 added to enemy ground totals, and of 0x3a9760 in HasSpaceForce.
5. 0x6136b0 (AI CreditsUnnormalized) — total of budget pools vs player credits.
6. Cache key: 0x5f3e60 hashes the binding set; whether the hash includes Self (so Human-rooted and Self-rooted leaves
   share entries) is [M] (AI_TACTICAL_PERCEPTION §1.2).
7. (q6) §8: whether any land path other than the auto-resolve start raises signal 0x13 on a planet through a
   dispatcher other than 0x220ed0. (~~whether the tactical budget map is empty~~: it is not, §9.3 [H].)

## 8. Player-rooted tokens used by Thrawn's Revenge (q6, 2026-11-03)

TR's galactic equations use three player tokens that the shipped FoC equations never use:
`GovernmentEquations.xml` (`240 > Variable_Self.TimeSinceSpaceDefender`, `480 > Variable_Self.TimeSinceGroundDefender`),
`OffensiveGalaticEquations.xml` (`40 >` and `80 > Variable_Self.TimeSinceGroundDefender`) and `BudgetingEquations.xml`
(`Variable_Self.BudgetAllocation{Parameter_Category = AIGoalCategoryType[MajorItem]} * Function_Current_Credits.Evaluate
> 10000`). This section specifies them and their attacker twins.

### 8.1 Token table and dispatch [H]

- **Name → id.** The static registration 0x5b43d0 registers every token name with the token manager (global pointer
  0x140b310a8, vf+0x20(name, &id)) and stores the id in a global. A failed registration logs "Unable to initialize
  <name> perception token!". The ids used here:

| global | token |
|---|---|
| 0x140a43d7c | `BudgetAllocation` |
| 0x140a43eac | `TimeSinceSpaceDefender` |
| 0x140a43eb0 | `TimeSinceSpaceAttacker` |
| 0x140a43eb4 | `TimeSinceGroundDefender` |
| 0x140a43eb8 | `TimeSinceGroundAttacker` |
| 0x140a43c60 | `Parameter_Category` (stored as a pointer-sized value, LuaValue class 10, by the parameter parser 0x5f3aa0) |

- **Id → leaf.** Each evaluator class has a registration function (its vf+0x40) that builds leaf nodes and inserts them
  with 0x64cbd0(evaluator, token id, node). A path walk (0x4dd1d0) finds the root evaluator, then looks each further
  token up in the current evaluator's map. A token with no entry fails the path (§5).
- **Who registers them.** The four `TimeSince*er` ids appear only in the GalacticPlayer registration 0x66b9f0 (visible
  leaves built by 0x665ec0, cache lifetime **0**). `BudgetAllocation` is registered in the base Player registration
  0x6e09c0 (visible leaf, lifetime **2** s), which the galactic and the tactical player evaluators both run.

### 8.2 TimeSinceSpaceDefender / SpaceAttacker / GroundDefender / GroundAttacker

**State.** Each GalacticPlayer evaluator (vt 0x8b4828; ctors 0x6667d0 / 0x666850, size 0xB8) keeps four int frame
stamps, all **−1** at construction [H]:

| field | stamp | leaf |
|---|---|---|
| +0xA8 | space defender | 0x66b950 |
| +0xAC | ground defender | 0x66b8b0 |
| +0xB0 | space attacker | 0x66b900 |
| +0xB4 | ground attacker | 0x66b860 |

**Value** (all four leaves; no parameters, no Self check) [H]: stamp = −1 → **1e18** (the float constant 1e18 widened
to double, 999999984306749440.0); otherwise (NOW − stamp) / FPS as a double. NOW is the global logic frame 0x140b0a320
(FrameSynchronizer +0) and FPS the int at 0x140b0a340 (30). The subtraction is done in int. Range: ≥ 0 seconds, or the
1e18 sentinel. The leaf never fails.

The clock is the global logic frame, not the galactic mode frame (mode +0x10) [H]. The global frame keeps counting
while a tactical battle runs, so time spent in battle counts towards these values [M].

**Writer: the listener 0x66d490** (the evaluator's signal-listener sub-object at +0x88) [H]:
- Signal **0x11**: take the space-conflict queue head of the galactic game-object manager (0x28a950(GMM, 0) → mode →
  +0x18 → 0x2ad0e0; record layout BATTLE_TRANSITIONS §1.1). If the evaluator's player id (player = evaluator +0x98, id
  at player +0x4C) equals the record's defender id (+8): space-defender stamp = NOW. Otherwise, if it equals the
  attacker id (+4): space-attacker stamp = NOW.
- Signal **0x13**: the same with the land-conflict queue head (0x2ac5a0): defender → ground-defender stamp, attacker →
  ground-attacker stamp.
- Any other signal is passed to the base listener 0x6e1680. A null player changes nothing.

**Subscription** (vf+0x60 = 0x66d6c0, the player re-bind) [H]: it unsubscribes the listener from signals 0x11 and 0x13
on every object of the perception system's target list (evaluator +0xA0 → vf+0x40; each entry's object via vf+0x30,
signal generator at object +0x38). It then re-binds the player (base 0x6e1690) and, if the evaluator has a player,
subscribes again to 0x11 and 0x13 on the same objects. The target list holds one entry per planet, so the evaluator
hears these two signals from **every planet** [M: list = the planet target list of §0].

**Senders.** All 183 call sites of the signal send 0x220ed0 were checked, including tail jumps [H]:
- **0x11** is raised on the planet only by the galactic space-conflict service 0x2b92e0 (GOM; space queue data +0x470):
  - state −1 (new head): after every participating unit and fleet member has been tagged (each also gets 0x11 on its
    own generator, which the player evaluators do not hear), the planet gets 0x11 and the state becomes 4. This is the
    start of a space conflict, whatever is chosen next (tactical battle, auto-resolve, retreat);
  - validation failure (0x2befe0 false: attacker == defender, or one side cannot fight): the planet gets 0x11
    **before** the head is removed, so the listener still sees the dropped record and stamps its attacker and defender
    as well.
- **0x13** is raised on the planet only by `AutoResolveClass` vf3 (0x608080) on its land path (flag +0x3D == 0), when a
  land auto-resolve begins. The land-conflict service 0x2b76a0 sends no 0x13. So **`TimeSinceGroundDefender` and
  `…GroundAttacker` change only when a land battle is auto-resolved** (AI-vs-AI invasions, or a human who picks
  auto-resolve). A land battle fought in tactical mode leaves them unchanged [H for the sender list; M that no other
  path reaches these listeners, open item 7].
- Signals 0x12 / 0x14 (the planet tokens `TimeSinceSpaceConflict` / `TimeSinceGroundConflict`, §1) are different: they
  are raised at battle **end** by 0x2a7580.

**Scope** [H unless tagged]:
- Per (perception system, player): every AI player's perception system has one GalacticPlayer evaluator per player,
  each with its own four stamps. They all hear the same planet signals, so they agree, except that an evaluator created
  or re-bound later has missed the earlier events [M].
- **Saved:** the save vfunc 0x66d5a0 writes the base evaluator data (0x6d8250), then chunk 0x20000 with micro-chunks
  0 = +0xA8, 1 = +0xB0, 2 = +0xAC, 3 = +0xB4 (4 bytes each). The load vfunc 0x66ccd0 reads the same chunk back.
- **Roots.** Registered on every galactic player evaluator, so valid on `Variable_Self`, `Variable_Enemy` (when bound),
  `Variable_Human` (when bound) and `Variable_Target.Owner`. Not registered on the planet (`Variable_Target.TimeSince
  SpaceDefender` fails), on the Game root, or on any tactical evaluator (tactical uses fail).

**Equation consequence.** `240 > Variable_Self.TimeSinceSpaceDefender` is 1.0 for 240 s of logic frames after a space
conflict in which the AI is the defender starts (or is dropped), and 0.0 before the first one (1e18).

### 8.3 BudgetAllocation (0x6e0560) [H unless tagged]

- **Self only.** Fails (returns false) unless the evaluator's player (+0x98) is the player that owns the perception
  system (bindings → system +0x58 → +0x50). So `Variable_Self.BudgetAllocation` works. On `Variable_Enemy`,
  `Variable_Human`, or `Target.Owner` (unless that owner is Self) the leaf fails and the whole equation fails.
- **Parameter.** `Parameter_Category` (global 0x140a43c60), read with 0x5f4780, default **−1** when absent. Only the low
  32 bits are used as the key. In TR it is an AIGoalCategoryType value (`AIGoalCategoryType[MajorItem]`), the same
  enum that names the categories of a budget (`<MajorItem>`, `<Infrastructure>`, `<Offensive>` … in the templates) [M
  for the key being that enum value].
- **Budget.** Goal system = evaluator +0xA0 (perception system) → +0x28 (AI context) → +0x40. Budget = goal system
  +0x190 (AIBudgetClass, AI_GALACTIC_2 §3).
- **Value.** 0x612ee0(budget, key) looks the key up in the budget's category map (hash map at budget +0x30 / +0x40 /
  +0x58, int key at node +0x10, category at node +0x18) and returns the category's float at +0x3C, widened to double.
  That float is the **normalised weight** w of AI_GALACTIC_2 §3.2: init 0x6137b0 and every reweight 0x614590 set it
  to the equation value with negatives → 0, then w / Σw, or 1/count when Σw = 0. A key that is not in the map → **0.0**
  (not a failure).
- **Range.** [0, 1]; over all categories of the player's budget the weights sum to 1. No parameter → key −1 → 0.0 [M:
  no category is stored under −1].
- **Timing.** No clock. The value changes only when the budget is reweighted (each commit, and at init), plus the 2 s
  leaf cache. During a reweight, 0x614590 writes each category's raw value to +0x3C as it goes and normalises in a
  second loop. An equation that reads `BudgetAllocation` from inside a budget weight equation can therefore see a raw
  value for categories already evaluated in that pass, unless its cached value is still fresh [M].
- **Tactical.** The same leaf is on the tactical player roots, and the tactical budget is **not** empty: it holds the
  categories of the mode template's `<Budget>` block, so the value is that category's normalised weight there (§9.3
  [H], corrects the earlier [L] reading "empty, 0.0").

### 8.4 Port notes

- Register the four `TimeSince*er` leaves on the galactic player evaluator with the stamps above, fed by "space conflict
  head started or dropped at a planet" and "land auto-resolve began at a planet"; 1e18 when never stamped; saved with
  the player evaluator.
- `BudgetAllocation`: Self-only, keyed by the AIGoalCategoryType value, returns the category's current normalised
  weight, 0.0 for an unknown category.
- Port status (q6 t10, 2026-11-04): galactic leaf done (q6 t03). Tactical player roots (`engine/ai/tactical`
  tactical_perception.cpp): Self-only as above, value 0.0 for every key (no tactical budget; §9.3 since shows the battle budget is filled from the template, open for the port); before t10
  the tactical leaf also returned 0.0 on Enemy / Human / Owner roots instead of failing.
- Port status (q6 t10): with no human player (headless `--galactic-ai` without `--human`), TR's
  `Allow_Blind_Space_Production` (via `Needs_More_Space_Force_Globally`: `Variable_Human.SpaceTotalUnnormalized`) and
  `Should_Conquer_Opponent_Planet` (via `Player_Danger_Score`: `Variable_Human.PlanetsControlled`) fail on every
  evaluation, as AI_GALACTIC_2 §4.3 requires; with `--human Empire` both evaluate. The headless `proposals` summary now
  names the first failing site (`~ eval FAIL <eq> at <token path | Function_X | Script_X | error>`).

## 9. Coordinator questions (q6, 2026-11-04)

Three questions from the q6 integration: what a failing operand does to an equation, what the contrast totals of
helper 0x5f2430 contain, and what `BudgetAllocation` reads in battle.

### 9.1 A failing operand fails the whole equation (PerceptionFunctionG.dll) [H unless tagged]

DLL RVAs (PerceptionFunctionG.dll, FoC x64):

- **`PerceptionFunctionClass::Evaluate`** (export 0x17650) runs the equation's node list in order on a stack of
  doubles (the parsed equation is postfix).
  - The first node that returns false ends the run: `Evaluate` returns false and the **output is not written**.
  - Success needs exactly one value left on the stack at the end; that value is the output.
- **Operator nodes** run only when both operands are already on the stack.
  - Multiply (0x17560) and Add (0x17070): fewer than 2 values on the stack → false; otherwise pop two, push the result.
  - There is no short-circuit. `* 0` cannot swallow a failure: the failing operand stops the run before the multiply
    node is reached.
- **Lookup node** (`PerceptualLookupNode`, vt 0xc7bf8, slot 1 = 0x17770): pops its parameters and calls the exe's path
  evaluator through the import at 0xfb440. If that fails, the node returns false and pushes nothing.
- **Function references**: `PerceptionFunctionCallClass::Evaluate` (0x20980):
  - no function → false;
  - otherwise it binds the call's parameters (import 0xfb438) and tail-calls `Evaluate` of the referenced function,
    returning its result.
  - So a `Function_X.Evaluate` operand fails when any leaf of `Function_X` fails [H for 0x20980; M for which node class
    an embedded `Function_X.Evaluate` operand goes through].
- **Not read:** the Division, comparison, Clamp (vt 0xc7be0), Unary (vt 0xc7bb0) and Random (vt 0xc7b98) bodies. They
  cannot change the outcome either: a failure stops the run first.

**The two TR cases** (no human player, so `Variable_Human` is unbound and every leaf under it fails, §6(a)):

- `Allow_Blind_Space_Production` = `0.01 + (Maxed_Shipyards > 0.5) * (1 + … + Function_Needs_More_Space_Force_Globally
  .Evaluate)`. The inner function reads `Variable_Human.SpaceTotalUnnormalized`, so it fails, and the whole equation
  fails. This happens even when `Maxed_Shipyards > 0.5` is 0 and even though the constant 0.01 is on the stack.
- `Should_Conquer_Opponent_Planet` via `Function_Player_Danger_Score` (reads `Variable_Human.PlanetsControlled`): the
  same. The whole equation fails.
- What the failure does depends on the caller (§6(a)):
  - proposal: the candidate is skipped;
  - template trigger: 0.0;
  - budget weight: the category keeps its previous weight.

**Port notes**
- The port's current rule (a failing leaf fails the whole equation, including through `Function_` references) is the
  original's. Keep it.
- On failure, leave the caller's output variable untouched. The callers already pre-load it: 0.0 for the template
  trigger, the previous weight for the budget.
- No operator short-circuits, `*` by 0 included.

### 9.2 Contrast totals of 0x5f2430: perception leaf plus the structures' own power [H unless tagged]

`0x5f2430(AI context, target context, category mask, ground flag)` returns a double:

1. **No AI context** → 0.0.
2. **Leaf.** Evaluate the path `Target.EnemyForce.SpaceTotalUnnormalized` (ground flag 0) or
   `Target.EnemyForce.GroundTotalUnnormalized` (ground flag 1). If the mask is non-zero, it is bound as
   `Parameter_Category`. The result variable starts at 0.0 and the success flag is ignored, so a failing leaf counts
   as 0.0. Per §2 the leaf sums the enemy units at the planet, including a starbase's **spawned member units**, but not
   the starbase's own power.
3. **Ownership gate.** The planet's owner is tested with 0x4ad9f0 against the AI player (context +0x28). If the owner
   is the AI or one of its allies, nothing more is added. Every other owner passes the gate, including pirates and the
   neutral player, so for a pirate planet the following steps always run.
4. **Effective mask** = the given mask, or all bits when the mask is 0.
5. **Space (flag 0).** Take the planet's starbase **type** (planet data +0xB8 → +0x140, via 0x3f8b40). If it is set and
   its category mask (+0x1648) shares a bit with the effective mask, add its `AI_Combat_Power` (0x370e60, float).
6. **Ground (flag 1).** For each structure object in planet data +0x158 (count +0x160) whose type (object +0x298)
   shares a category bit with the effective mask, add that type's `AI_Combat_Power`. Then add 0x4b8000(planet data,
   mask): the sum of `AI_Combat_Power` over the type ids in the list at planet data +0x10 (count +0x18, 0x18-byte
   records, ids resolved in type table 0x140b2f020) whose category matches the mask. GALACTIC_ECONOMY §6.2 calls these
   "garrison types" [M for the meaning of that list].
7. **Precision.** The leaf is a double. Each structure power is a float widened to double and added in list order.

**Answer.** Yes. For a pirate (or any non-allied) planet, the untyped totals (mask 0 = all categories) add each
structure's own `AI_Combat_Power` on top of the leaf, which already holds the spawned members:
- space: the starbase type's power;
- ground: every ground structure's power, plus the 0x4b8000 list.

**The typed rows call the same helper.** In the contrast list builder 0x5f1990, each per-type row is
0x5f2430(…, row category mask, flag) × scale, so the own-power additions also land in **typed rows whose category
matches**:
- every shipped starbase is `<CategoryMask> Capital </CategoryMask>` (`STARBASES.XML`, 23 types), so the starbase's
  own power is in the Capital row;
- ground structures are in the rows that match their category.

This contradicts the "spec reading" used for the `CO_SBUNTYPED` experiment in A2_CONQUER_OPPONENT_DIAG (own power
only in entries 0/1). That experiment gave better attack behaviour, so its gain must come from something else that
still differs [L: not traced].

**Layer order (correction to AI_GALACTIC_2 "Contrast", Setup A).** Flag 1 selects `GroundTotalUnnormalized` and the
ground structures, so the list 0x5f1990 builds is:

| position | content | layer byte |
|---|---|---|
| entry 0 | the **ground** total | 1 |
| entry 1 | the **space** total | 0 |
| next rows | one typed row per `EnemyContrastTypes` key, **ground** (flag 1) | 1 |
| last rows | one typed row per key again, **space** (flag 0) | 0 |

- Each typed loop is zeroed when its own layer total is ≤ 0.
- AI_GALACTIC_2 names entry 0 "space". The consumers only compare the layer byte (the satisfied test 0x72ac70 sums the
  entries whose byte equals the asked layer), so the naming matters only where the list **order** matters, for example
  the order in which Setup C builds the candidate pairs [M].

**Port notes**
- `galactic_ai_enemy_strength` (`engine/ai/src/galactic_ai_tokens.cpp`) adds the own power only when the mask is
  "all". The original adds it whenever the structure's category matches the row's mask:
  - the starbase in the Capital row;
  - ground structures in their categories' rows.
- The port has no 0x4b8000 list term ("[port: none]").
- The port's ownership gate is "owner is hostile to Self". The original's is "owner is neither the AI nor an ally",
  which also lets a neutral or unowned-but-held planet's structures count. For pirate planets the two gates agree.
- `ContrastM::setup_a` (`engine/ai/src/galactic_pp.cpp`) pushes the space total first. The original pushes ground
  first, then the typed ground rows, then the typed space rows. Match the order if candidate-pair order matters there.
- A failing leaf counts as 0.0 here, not as a failure of the plan.

### 9.3 BudgetAllocation in battle: the tactical budget is filled from the mode template [H unless tagged]

- **Leaf.** 0x6e0560 is registered by the base Player evaluator 0x6e09c0, so it is on the tactical player roots too.
  The §8.3 rules are unchanged:
  - Self only (any other root fails);
  - `Parameter_Category`, default −1;
  - value read through context goal system (+0x40) → budget (+0x190) → 0x612ee0.
- **Every mode has a budget.** Each goal system builds an AIBudgetClass (0x6109c0, called from 0x6c7970). It is reached
  from the goal-system constructors 0x6b8480 / 0x6b86e0 / 0x6b88d0 and from 0x5f5230.
- **Filled at the first service.** In battle, 0x5f5ab0 (AI_TACTICAL §1.2) → template init 0x6bbed0 picks the mode's
  template list (0x60f780(player type, mode)) and applies its first template through 0x670ed0:
  - `<Budget>` entries (template list +0x80) are added by 0x615420 as (category id, weight equation). An entry from a
    template of higher or equal priority replaces the category's equation.
  - `Tactical_Budget_Allowance` entries (list +0xC0) are added by 0x615550 as a category with **no** equation and the
    allowance at +0x5C.
- **Weights.** Init 0x6137b0 evaluates each weight equation in the tactical perception context. Self is bound, Enemy
  and Human are bound only when they exist. Then:
  - negatives → 0;
  - normalise to sum 1;
  - all zero → 1/count;
  - a category without an equation counts as 0 but is part of the count.
- **Reweight.** 0x614590 runs from the goal service 0x6cacd0 (AIGoalSystem vtable slot 2) at the end of each proposal
  pass (+0x184), as in galactic. Only its **money** part is galactic-only (mode +0x34 == 0 test), so AI_GALACTIC_2 §3
  "galactic only" applies to money, not to the weights. Confirmed on the galactic and land goal systems
  (PRIOR_PATCH.md); the space one is [M].
- **Cleared** at battle end by goal slot 7 (0x6cafb0, AI_TACTICAL §1.4).
- **Shipped data.** The FoC `Test_Land` / `Test_Space` `<Budget>` blocks:
  - Hero ReallyBig;
  - High_Priority High;
  - Map_Control Medium_High;
  - Med_Priority Medium;
  - Tactical_Targeted / Defensive / Tactical_Untargeted with `Generic*` allocations;
  - NoBudget ReallyBig (space only);
  - StoryArc ReallyBig.
- **AIGoalCategoryType values** (the key):

  | value | category | value | category |
  |---|---|---|---|
  | 0 | Offensive | 11 | MajorItem |
  | 1 | Defensive | 12 | StoryArc |
  | 2 | Information | 13 | StoryArcHuman |
  | 3 | Infrastructure | 14 | Interventions |
  | 5 | Tactical_Targeted | 15 | High_Priority |
  | 6 | Tactical_Untargeted | 16 | Sandbox_Events |
  | 7 | NoBudget | 17 | Med_Priority |
  | 8 | Hero | 18 | Map_Control |
  | 9 | Always | | |
  | 10 | Macro_Goal | | |

**Answer.** The tactical budget map is not empty. `Variable_Self.BudgetAllocation{Parameter_Category = X}` in battle
is X's normalised weight in the active tactical template's `<Budget>` block; it is 0.0 only when X is not in that
block.
- No shipped FoC or TR tactical equation reads `BudgetAllocation`. TR's only use is `Flush_MajorItem_Budget`
  (galactic), so the difference is not visible with current data.

**Port notes**
- Give every tactical AI player a budget built from its mode template's `<Budget>` (and `Tactical_Budget_Allowance`)
  entries at the first AI service.
- Weight it with the §8.3 normalisation and reweight it at each proposal-pass end, without the money step.
- Clear it at battle end.
- `BudgetAllocation` on a tactical Self root then returns that weight. Returning a fixed 0.0 in battle is wrong but
  harmless for current data.

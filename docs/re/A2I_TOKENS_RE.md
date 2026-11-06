# A2i t04 / t05 / t07: Maintenance and NetIncome for non-Self roots, planet force structures, enum literals (2026-11-06)

StarWarsG.exe FoC x64 and PerceptionFunctionG.dll (same Steam install), all RVAs. Tags as in the other `docs/re` pages: [H] read
from code / data, [M] one inferred link, [L] unverified. No decompiled code is reproduced. Notation as in `AI_GALACTIC_TOKENS.md`
(D = planet data pack, planet +0xB8; power(T) = `AI_Combat_Power` 0x370e60; FPS = 30, `DAT_140b0a340`; Self = bindings +0x58 -> +0x50;
root = the evaluator's player, +0x98). "Port" = `engine/ai/src/galactic_ai_tokens.cpp` / `galactic_ai.cpp` /
`perception_equations.cpp` at `cd13efb`. This page continues `A2H_GALACTIC_AI_RE.md` and corrects two of its statements and one row
of `AI_GALACTIC_TOKENS.md` (marked **Correction**).

Token ids used below, named from the registration 0x5b43d0 [H]: 0x140a43c4c `Variable_Self`, 0x140a43c50 `Variable_Enemy`,
0x140a43c58 `Variable_Target`, 0x140a43c7c `EnemyForce`, 0x140a43c78 `FriendlyForce`, 0x140a43d04 `Maintenance`, 0x140a43d58
`MaintenanceUnnormalized`, 0x140a43d2c `GrossIncomeUnnormalized`, 0x140a43d30 `NetIncomeUnnormalized`, 0x140a43d3c / d40
`GrossIncomeNBPTM` / `NBTF`, 0x140a43d44 / d48 `NetIncomeNBPTM` / `NBTF`, 0x140a43d4c `MaxAllPlanetsIncomeUnnormalized`,
0x140a43d50 `MaxOwnedPlanetsIncomeUnnormalized`, 0x140a43dc4 `TargetPlayerID`.

Variables (Self / Enemy / Target, and each player's perception id +0x478) are bound in the **perception context** (bindings +0x58),
not in the parameter map (bindings +0x08). A leaf that builds fresh bindings and copies only +0x58 therefore keeps every variable and
drops the caller's parameters [H: 0x659c60 ctor, 0x4dd1d0 resolves the first path token through 0x5b4300(context, token)].

## 1. t04: Maintenance and NetIncome

### 1.1 Player root `MaintenanceUnnormalized` 0x669420, root ≠ Self [H]

(The neutral-faction and Self branches are in `A2H_GALACTIC_AI_RE.md` §3.)

For a root that is not Self, the leaf does **not** look at the root player at all:
1. For every entry of the perception target list (system vf+0x40, list order), take the planet's evaluator (target vf+0x28) and
   evaluate the path `EnemyForce.MaintenanceUnnormalized` on it with fresh bindings (context copied, no parameters).
2. Any failure fails the token. Otherwise the result is the double sum of the planet values.

`EnemyForce` is relative to **Self** (the side filter compares owners with Self, §1.2). So for any non-Self root (the human, an
enemy, an ally) the value is the same: the upkeep of every fleet hostile to Self that is in orbit somewhere. Fleets in hyperspace
are not in any orbit list and add nothing. Leaf lifetime 7 s.

### 1.2 Planet force `MaintenanceUnnormalized` 0x6daf20 (`Target.<side>.MaintenanceUnnormalized`) [H]

Registered on both force sides as a fogged leaf, mask 0x40, lifetime 15 s (registration 0x6d83c0(…, 0x40, …, 0xf)).
1. No planet object: fail. Orbit list (0x395900) empty: 0.0, success.
2. `Parameter_Category` read with 0x5f4780, default all bits (−1); a value of 0 also means "all".
3. For each orbit object O, in list order, skip it unless all hold:
   - O's +0x3A0 bit value 2 is clear (the "dead / leaving" bit the other force leaves skip);
   - O passes the side filter (vf+0x58: Enemy = hostile to Self, Friendly = Self or ally);
   - O is not contained in a fleet: O's container (+0x2B0) is null, or the container has no FLEET behaviour slot (byte +0x336 = −1)
     [M: +0x336 is the behaviour-4 slot byte, by analogy with +0x335 = behaviour 3 = planet, `A2GAL_READINGS.md` §2.2].
4. For each member M returned by 0x3965f0(O, list, 1) (the fleet's contents), with T = 0x3973b0(M) (a transport resolves to its
   company type):
   - f = the `Maintenance_Cost` (+0x3AC) of **M's owner's** faction (M +0x58); u = T's `Maintenance_Cost` (+0x798);
   - skip when f × u = 0 (float), when btime(T, no planet) (0x371230) = 0, or when the category mask is non-zero and T's
     `CategoryMask` (+0x1648) does not meet it;
   - add (double)(f × u × ((float)cost(T, no planet) / btime / 30)) (float products in that order, one cast, double sum).

Differences from the Self branch of 0x669420: this one reads `Parameter_Category`; the faction factor comes from each member's owner
(the Self branch uses the fleet's owner); the float rounding order differs (here f × u × rate in float, there (double)rate ×
(double)(u × f)); in-transit fleets are absent.

### 1.3 Planet force `Maintenance` 0x6dabc0 (`Target.<side>.Maintenance`) [H]

Same registration as 1.2 (mask 0x40, 15 s).
1. num = path `Variable_Target.<side>.MaintenanceUnnormalized` (side = this evaluator's own side token, vf+0x68), fresh bindings
   (no caller parameters, so category = all). Failure: fail.
2. num = 0: 0.0, success.
3. den = path `<V>.GrossIncomeUnnormalized` with V = the side's player variable (vf+0x78): **Enemy side → `Variable_Enemy`, Friendly
   side → `Variable_Self`** (0x6dbcc0 / 0x6dbcd0 return 0x140a43c50 / 0x140a43c4c). The success flag of this evaluation is ignored.
4. den > 0: num / den, success. Otherwise (den ≤ 0, or den's path failed and left 0): **0.0 and fail**.

No shipped equation uses `Target.*Force.Maintenance*` or a non-Self `.Maintenance*` (FoC XML grep: only `Variable_Self.Maintenance`
3×, `Variable_Self.NetIncomeUnnormalized` 1×) [H data].

### 1.4 NetIncome and GrossIncome normalised variants [H]

- `NetIncomeUnnormalized` 0x66a420: g = path `<root id>.GrossIncomeUnnormalized` (fail -> fail), m = path `<root id>.MaintenanceUnnormalized`
  (fail -> fail); result g − m. So for a non-Self root it is Gross(root) − (the enemy-of-Self upkeep of §1.1). Lifetime 7.
- All four normalised tokens are one helper 0x66a730(numerator token, denominator token) [H: the wrappers 0x667f70 / 0x667f90 /
  0x66a3e0 / 0x66a400 load the two token ids, read from the disassembly]:

  | token | leaf | numerator | denominator |
  |---|---|---|---|
  | GrossIncomeNBPTM | 0x667f70 | GrossIncomeUnnormalized | MaxAllPlanetsIncomeUnnormalized |
  | GrossIncomeNBTF | 0x667f90 | GrossIncomeUnnormalized | MaxOwnedPlanetsIncomeUnnormalized |
  | NetIncomeNBPTM | 0x66a3e0 | NetIncomeUnnormalized | MaxAllPlanetsIncomeUnnormalized |
  | NetIncomeNBTF | 0x66a400 | NetIncomeUnnormalized | MaxOwnedPlanetsIncomeUnnormalized |

  Both paths are rooted at the root player's id with fresh bindings. Either failing fails. den = 0: 0.0 when num = 0, else **fail**.
  Otherwise num / den, **no clamp** (a negative net income gives a negative value). Lifetime 7 each.
- `MaxAllPlanetsIncomeUnnormalized` 0x6698f0: Σ over **every** target-list planet (any owner) of the planet's potential income
  (0x3f83b0 below), each term cast float -> double, double sum. Lifetime **−1: computed once and cached forever** (per binding set).
- `MaxOwnedPlanetsIncomeUnnormalized` 0x669ca0: the same sum over target-list planets whose `TargetPlayerID` (evaluated per planet,
  **truncated** to int, not rounded) equals the root's id (+0x4C). A `TargetPlayerID` failure fails. Lifetime 7.
- Potential income 0x3f83b0(planet behaviour, planet, player): 0 when the planet or the player is null (the player is only
  null-checked, it does not enter the value). Otherwise, with b = the planet base value (0x3733c0, GALACTIC_ECONOMY §1.4), in float,
  summed in this order:
  1. (float)b × the black-market multiplier D +0x2A0 (when D exists);
  2. the force-alignment term (0x3f4f90, as in Planet_Income);
  3. the political term at **full** control: only when the planet owner D +0x6C is not the neutral player, q =
     `Political_Income_Curve`(GC +0xF0 `MaximumPoliticalControl` = 5), and q × b − b when q > 0 (vanilla curve: q = 1, term 0);
  4. trade routes 0x3f6210 with "all links" set: b × Σ `Credit_Gain_Factor` (> 0) over the planet's links whose endpoint types match
     the route, **whatever the neighbour's owner** (0 when the owner D +0x6C resolves to no player).

  No halt check, no ability / breakdown entries, no min / max clamp, no siphon.
- No shipped equation uses any NB* income token (the two mentions, `INFRASTRUCTUREGALACTICEQUATIONS.XML` 567 and
  `OFFENSIVEGALATICEQUATIONS.XML` 242, are inside XML comments) [H data].

**Port change** (`maintenance_unnormalized`, the `GROSSINCOME*` / `NETINCOME*` / `MAINTENANCE*` branches of `resolve_player`, and
`resolve_force`):
1. `maintenance_unnormalized(pl, self)` for a non-neutral, non-Self root: Σ over planets (target-list order) of the planet-force
   enemy upkeep of 1.2 for Self, category all. Today it returns 0 [port]. The value does not depend on `pl`.
2. Add the planet-force leaves `Target.EnemyForce/FriendlyForce.MaintenanceUnnormalized` (1.2, reading `Parameter_Category`,
   orbiting non-moving fleets of that side, member owner's faction factor, float order f × u × rate) and `.Maintenance` (1.3: den =
   Enemy's / Self's GrossIncomeUnnormalized; den ≤ 0 fails) with lifetime 15. Today these paths are unregistered and fail.
3. NetIncomeUnnormalized = Gross − Unn with the non-Self Unn of step 1 (port already does Gross − Unn).
4. Normalised income: num / den with den = Σ potential income over all planets (NBPTM; cached for the whole game after the first
   evaluation) or over the root's TargetPlayerID planets (NBTF). Replace `normalise()` (clamp to [0, 1], num > den -> 1) with the
   plain division, and den = 0 with num ≠ 0 -> fail. The port's current denominators (Σ `planet_income` over all planets; max gross
   over players) are both wrong. Add a `potential_income(p)` next to `planet_income` per 0x3f83b0.

Steps 1-4 change no shipped equation's value except through `Variable_Self.NetIncomeUnnormalized`, which is unchanged (Self branch).

## 2. t05: planet-level force leaf, structures and starbase members

### 2.1 `SpaceTotalUnnormalized` on PlanetForce 0x6dbd50 [H]

After the orbiting fleets (`AI_GALACTIC_TOKENS.md` §2), if the planet itself passes the side filter (owner vs Self):
1. D null: **fail**.
2. Starbase: if D +0x140 (the starbase **type**) is set and its owner D +0x74 resolves to a player: the type's per-tech members
   with that player's tech (+0x84), through the fall-back family 0x373f40 / 0x373e30 / 0x373d30, each member power(m) × count when
   the member's `CategoryMask` meets `Parameter_Category`. **Not** the starbase's own power.
3. Ground structures D +0x158 (count D +0x160), each object S with type T = S +0x298: only when T's byte +0x1E
   (`Is_Special_Weapon_In_Space`) is set. Mask = T +0x1658 (`Garrison_Category`, also written by `Space_Or_Garrison_Category`;
   for a company it is the OR of its members', OBJECT_TYPES §6.2); **when that mask is 0 it is replaced by the `Capital` mask**
   (0x140b3a9d0). Add power(T) when `Parameter_Category` (default all bits) meets the mask.
4. Orbital structures D +0x170 (count D +0x178): every object, **no +0x1E test**, the same mask rule, + power(T).

So the default is a property of the **type mask** (zero -> Capital), not of the parameter. With no `Parameter_Category` every
qualifying structure counts; with a parameter, a type whose +0x1658 is 0 counts only when the parameter includes Capital.

Vanilla data (FoC XML, variants resolved) [H data]:
- Types with `Is_Special_Weapon_In_Space` = Yes: `Ground_Ion_Cannon`, `Ground_Empire_Hypervelocity_Gun`,
  `Space_Mode_Ground_Rebel_Ion_Cannon`, `Space_Mode_Ground_Empire_Hypervelocity_Gun`. None sets `Garrison_Category` (mask -> Capital),
  and **none sets `AI_Combat_Power` (default 0.0)**.
- All 24 `DUMMY_ORBITAL_STRUCTURE` types (sensor pods, jamming / gravity-well stations, scanners, satellites, destroyable asteroids,
  merchant dock, construction pod): `AI_Combat_Power` unset (0.0).
- So steps 3 and 4 add **0** in vanilla for every category. The ion cannon and the hypervelocity gun are in the walk but have no
  combat power.

### 2.2 `GroundTotalUnnormalized` on PlanetForce 0x6d95d0 (the sibling) [H]

If the planet passes the side filter: D null fails; for every ground structure S in D +0x158, the per-tech members of S's type with
the tech of **S's owner** (S +0x58, not the planet owner), fall-back family, member mask +0x1648. **No structure mask and no own
power** in this leaf. Then the Enemy-side planet-type list +0x20C8 (unchanged from `AI_GALACTIC_TOKENS.md` §2).

### 2.3 Per-tech members on planets [H]

- 0x373f40 / 0x373e30 / 0x373d30 take the list of the requested tech level, or, when it is empty, the highest lower level with
  entries (down to 0). Vanilla lists exist only at level 0 (`Starting_Spawned_Units_Tech_0`), so the planet-level leaves add them
  at every tech. Starbases carry them: e.g. `Empire_Star_Base_3` = TIE_Interceptor_Squadron × 3 + TIE_Bomber_Squadron × 3 +
  Tartan_Patrol_Cruiser × 1, `Rebel_Star_Base_1` = Rebel_X-Wing_Squadron × 2 + Y-Wing_Squadron × 2; ground structures too (barracks,
  vehicle factories, officer academies); fleet units (Victory_Destroyer, Acclamator_Assault_Ship, Generic_Star_Destroyer, Executor,
  Arc_Hammer, Acclamator_Underworld).
- The count is looked up **by member type** (0x373d30 returns the count of the first entry with that type), so a list that repeats a
  type adds the first entry's count each time. No vanilla list repeats a type.
- A negative count is not filtered (power × count is added). Vanilla `Starting_*` counts are all positive (only `Reserve_*` uses −1).
- The player-root tokens (0x66b1c0 / 0x6682c0) use the exact-level list (0x373fd0 / 0x373ee0), so they add 0 at tech ≥ 1
  (`A2H_T02_SPACE_TOTAL.md` §3); the planet-level leaves do not.

### 2.4 Correction: the player-root structure branch 0x66b1c0 [H, disassembly 0x66b28a-0x66b2f8]

`AI_GALACTIC_TOKENS.md` §3 row SpaceTotalUnnormalized and `A2H_T02_SPACE_TOTAL.md` §3 / §4 read the non-starship branch as "power
when the +0x1658 mask (default Capital) matches and (T +0x1E set **or T is not** DUMMY_ORBITAL_STRUCTURE)". The code is:
- for a T that is neither DUMMY_STARSHIP nor DUMMY_SPACE_FIGHTER_SQUADRON: mask = T +0x1658, or Capital when it is 0; if
  `Parameter_Category` (default **all bits**) meets it, add power(T) when T +0x1E is set **or T is** DUMMY_ORBITAL_STRUCTURE.

With the default all-bits category the mask test always passes, so the a2h reason ("Garrison_Category holds only Infantry /
Vehicle") is not what keeps companies and ground structures out. They are out because they are neither special weapons nor
orbital structures. The branch still adds 0 in vanilla (both of those type sets have power 0, §2.1), so the a2h t02 numbers and
its port change stand.

**Port change** (`galactic_ai_planet_force`, `player_total` comments):
- Vanilla: **port already matches** for both leaves (the four special weapons have power 0, orbital structures have power 0, the
  starbase and structure members with the fall-back list are what the port adds).
- For data fidelity (mods): in the space leaf, test each +0x1E ground structure with mask = its `Garrison_Category` /
  `Space_Or_Garrison_Category` bits (company: members' OR), Capital when 0, against `mask`, instead of `mask & Capital`; add the
  orbital special structures (D +0x170) by the same rule without the +0x1E test, if the port keeps them apart from ground
  structures; use the starbase owner (D +0x74) for the starbase members and each structure object's owner for the ground members.
- If `ps.structures` holds orbital structures too, the ground leaf must skip them (retail's ground leaf walks D +0x158 only). No
  vanilla orbital structure has a spawn list, so this is also 0 today.
- Fix the `player_total` comment: the branch is "+0x1E or DUMMY_ORBITAL_STRUCTURE, mask +0x1658 (0 -> Capital)", 0 in vanilla
  because those types have no combat power. If implemented: add power(T) for a walked object whose type qualifies.

## 3. t07: `Type[Name]` literals in equations

### 3.1 Where the name is resolved [H]

- PerceptionFunctionG.dll builds each equation's node tree in 0x16210 (recursive, called from `PerceptionFunctionClass::Build`
  0x15ba0). An enum literal is a parse node (kind 0xA) with exactly two children: the converter name text and the bracket text.
- It calls the exe callback passed as argument 9 of `Init_Perception_DLL` (DLL global 0x1800fb470 = exe 0x474c40 -> 0x23b030):
  converter name -> `DatabaseUIntConversionClass*` from the exe's global converter registry (0x140b09350), or null.
- Then it calls the converter's virtual slot 1, `String_To_UInt(text, uint64&)`, **once, with the whole bracket text**. The DLL
  does not split on `|`.
  - Plain enums (`DynamicEnumConversionClass<…>`, e.g. AIGoalCategoryType, vtable 0x872e18; shared slot 0x1dc300): trim leading /
    trailing whitespace (0x200350), upper-case (0x200ad0), exact lookup of the whole string. Found: the int value, sign-extended.
    Not found: false.
  - Bitfields (`DynamicBitfieldConversionClass<GameObjectCategoryType / GameObjectPropertiesType>`, vtable 0x872ef8, slot 1
    0x46e9a0 -> slot 4 0x46e680): split on `|`, space, `,`, tab, newline; each token trimmed, upper-cased and looked up; OR of the
    values. **Any unknown token: false** (and the output keeps its old value).
- On success the DLL makes a constant node whose 8 bytes are the **uint64 bit pattern** (not converted to a double). Parameter
  readers take the raw 8 bytes back (0x5f4780 reads a qword), so `Parameter_Category = AIGoalCategoryType[MajorItem]` carries the
  integer 11 exactly. Used in arithmetic the literal would be a denormal double (11 -> about 5.4e-323), not 11 [H; no shipped
  equation does this].

### 3.2 What a bad literal does [H unless tagged]

- Unknown converter (`Foo[Bar]`): node build fails ("There is no enum converter named %s!").
- Known converter, unconvertible text: node build fails ("Enum converter(%s) was unable to convert %s to a valid enum value!").
  This covers an unknown name (`AIGoalCategoryType[Foo]`) and **any `[A|B]` on a plain enum** (`AIGoalCategoryType[Offensive|
  Infrastructure]` looks up "OFFENSIVE|INFRASTRUCTURE" and fails). It also covers a bitfield list with one unknown token.
- A failed node build makes `Build` return null: the equation is **not added** to the equation manager ("Equation %s (file %s) was
  not successfully parsed!"); the file's other equations still load (the read loop continues).
- The first parse error of the process shows a **modal MessageBox** "Perception Parsing Error!" / "There were perception parsing
  errors. See PerceptionErrors.txt for details." (one-shot flag DLL 0x1800fb4bd). The log writer 0x17020 is an empty stub in this
  build, so PerceptionErrors.txt stays empty.
- So the answer is **parse error, the equation never exists**: not value 0, not a runtime failure of the token. Exe sites that look
  the equation up by name get null (e.g. template triggers log "Unrecognised perception function %s specified as trigger for AI
  template."); a goal-function set naming it has no function for that goal [M: the goal-function load site was not traced past
  the lookup].
- Shipped data has no such case: every literal converts. The odd-looking `GameObjectCategoryType[Infantry | Hero]`
  (`AI_EQUATIONS_EXPANSIONGENERIC_LANDSKIRMISH.XML`, `Tactical_Multiplayer_Should_Build_Barracks`) is valid: the enum file defines
  `Hero` = 0x06400000 (LandHero | SpaceHero | NonCombatHero) [H data]. The converter registry is assumed to cover
  `DifficultyLevelType` and `HardPointType`, which shipped equations use [M].

### 3.3 Correction to `A2H_GALACTIC_AI_RE.md` §1

"A `[A|B]` form would OR the ids (the DLL's bracket `|`)" is wrong. The OR belongs to the bitfield converter, and AIGoalCategoryType
is a plain enum, so `[A|B]` is a parse error and the equation is dropped. "Unknown name: most likely a parse error [L]" is now [H].

**Port change** (`perception_equations.cpp` parameter-value parser, equation loading, `GalacticAiWorld::Impl::active_goals`):
1. Resolve `Type[...]` literals **when the equation is built**, with the converter named by `Type`: no such converter -> the equation
   fails to build. Plain enum: the trimmed, case-insensitive bracket text must be one defined name (so any `|` fails). Bitfield
   (GameObjectCategoryType, GameObjectPropertiesType): every token (split on `|`, space, `,`, tab, newline) must be defined; the
   value is their OR.
2. An equation that fails to build is absent (not registered); its other file-mates load. Callers that name it behave as for a
   missing equation. Log it; there is no need to imitate the MessageBox.
3. Then `active_goals` receives one category value (no OR of several names, no unknown names): count the active vector of the
   category with that value (port already does this for one name). The `n_cat != 1` failure rule of A2h stays.
4. Optional: carry the literal as an integer so a literal used in arithmetic could reproduce the denormal; not needed for shipped
   data.

Result for shipped data: unchanged (the only ActiveGoals use is `AIGoalCategoryType[MajorItem]`).

## 4. Not resolved

- The goal-function set site that loads `<Function>` names (BASICOFFENSIVEGALACTICSET.XML uses `Can_Reclaim_Excess_MajorItem_Budget`):
  what it does with a null equation at evaluation (skip, or crash) [L].
- `Function_X.Evaluate` references inside other equations to a dropped equation: link-time failure or evaluation failure (not
  traced in the DLL) [L].
- Whether the converter registry name lookup (0x23b030) is case-sensitive [L].
- 0x3965f0 flag 1 nested contents and the +0x2B0 / +0x336 container test meaning [M] (as in A2h).

## 5. Confidence summary

| claim | tag |
|---|---|
| non-Self MaintenanceUnn = Σ planets of `EnemyForce.MaintenanceUnnormalized`, independent of the root | H |
| planet-force MaintenanceUnn body (filters, member-owner faction factor, category, float order) | H |
| planet-force Maintenance = Unn / Enemy's or Self's GrossIncomeUnn, den ≤ 0 fails | H |
| NB* income = num / MaxAll (lifetime −1) or MaxOwned, no clamp, den 0 rule | H |
| potential income 0x3f83b0 terms | H (field names from GALACTIC_ECONOMY) |
| planet space leaf: +0x1E ground structures and orbital structures, mask 0 -> Capital | H |
| those structures add 0 in vanilla (no AI_Combat_Power) | H data |
| player-root branch is "+0x1E or DUMMY_ORBITAL_STRUCTURE" (correction) | H |
| fall-back member lists on planets, count looked up by type | H |
| enum literal: one String_To_UInt call on the whole bracket text; plain enum exact lookup; bitfield split | H |
| bad literal -> equation not built, one-time MessageBox, log stub | H |
| the literal constant is the uint64 bit pattern | H |
| downstream effect of a missing equation at goal-function sites | L / M |

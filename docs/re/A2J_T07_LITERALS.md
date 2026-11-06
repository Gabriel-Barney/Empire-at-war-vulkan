# A2j t07: perception-equation literals, dropped-equation references, converter-name case, force-leaf fog mask (2026-11-06)

Follow-up of `A2I_TOKENS_RE.md` §3 (build-time `Type[Name]` literals) and §1.2 (planet-force Maintenance leaves). No new
binary reading was done for this page: every statement cites an existing `docs/re` page or the shipped FoC XML
(`extracted/foc_data_text.zip`). Tags as elsewhere: [H] read from code / data, [M] one inferred link, [L] unverified, [port] a
port choice where the original is unknown. "Port" = `engine/ai/src/perception_equations.cpp` and
`engine/ai/src/galactic_ai_tokens.cpp` on branch `claude/a2j-laneD`.

| item | result |
|---|---|
| 1. converters from the data enum files | **done** (port change below) |
| 2. `Function_X` reference to a dropped equation | **documented** (spec missing: port rule kept, RE question) |
| 3. converter-name case | **documented** (spec missing: case-insensitive kept, RE question) |
| 4. fog mask 0x40 in force leaves | **documented** (the rule is in the specs; not modelled, because the A2 resolver has no fogged leaves at all) |

## 1. Enum converters from `Data\XML\Enum` [H source, port change]

### 1.1 Spec

- The DLL asks the exe's global converter registry for the converter named by `Type` (`A2I_TOKENS_RE.md` §3.1, 0x23b030 ->
  registry 0x140b09350) and calls its `String_To_UInt` [H]. So a literal converts with the **same converter objects the exe
  uses for its own data**.
- Those converters are created once at engine init (0x46ea70, `OBJECT_TYPES.md` §9.1) [H]. Three kinds:
  - dynamic enum / dynamic bitfield: values from `.\Data\XML\Enum\<Name>.xml` **through the VFS, so mods can override** [H]
    (`GameObjectCategoryType`, `GameObjectPropertiesType`, `MovementClassType`, `ObjectWeatherCategoryType`,
    `SurfaceFXTriggerType`; `AIGoalCategoryType` and `PerceptionTokenType` are "loaded the same way", §9.1.2 [H]);
  - static enum / bitfield: name / value tables compiled into the exe (`DifficultyLevelType` and `HardPointType` are
    `EnumConversionClass<...>`, `SUBSYSTEMS.md` RTTI list: no `Dynamic` variant) [H].
- File rules (§9.1 "Enum XML files") [H]: each root child is one entry (tag = name, text = value); dynamic enum values are C
  `atoi`, empty text = the running entry index; dynamic bitfield values are `sscanf("%llx")` (always hex), empty text = 64 −
  index [M], more than 64 entries discards the converter; a duplicate name, or a duplicate value, **discards the whole
  converter** (every later conversion fails). Names are trimmed and upper-cased on both sides (case-insensitive lookup) [H].
- Shipped FoC data: the seven files `AIGOALCATEGORYTYPE.XML`, `GAMEOBJECTCATEGORYTYPE.XML`, `GAMEOBJECTPROPERTIESTYPE.XML`,
  `MOVEMENTCLASSTYPE.XML`, `OBJECTWEATHERCATEGORYTYPE.XML`, `PERCEPTIONTOKENTYPE.XML`, `SURFACEFXTRIGGERTYPE.XML` under
  `Data\XML\Enum` [H data]; their values equal the port's built-in tables (`OBJECT_TYPES.md` §9.1.1 / §9.1.2,
  `AiDefinitions` defaults).

### 1.2 Port change

- New `ai::EnumLiteralTable` (`perception_equations.hpp`): default = the built-in tables (the a2i behaviour). `load(provider)`
  re-reads the five `objects::EnumRegistry` dynamic converters with `EnumRegistry::load_dynamic` and `AIGoalCategoryType` with
  the same dynamic-enum rules; a missing file keeps the built-in table; a discarded converter stays registered and fails every
  literal (Unconvertible, so the equation is dropped as in §3.2 of A2I); problems go to the diagnostics. `DifficultyLevelType`
  and `HardPointType` stay compiled in.
- `EquationSet` carries the table (`set_enum_literals`, `enum_literals()`); `add()` builds with it; `parse_expression` takes an
  optional table. `load_equation_files(provider, ...)` loads the table from the same provider first unless the caller set one,
  so every host (`alamo_game`, `alamo_headless`) gets the mod's enum files with no host change; the table's problems are
  appended to `LoadReport::messages`.
- `Parameter_Category` on the force leaves (`galactic_ai_tokens.cpp` `category_param`) now uses the literal's converted value
  (`ParamValue::enum_value`), as 0x5f4780 reads the raw qword (`A2I_TOKENS_RE.md` §3.1 [H]); before, it looked the names up
  again in the built-in GameObjectCategoryType table. Same value for shipped data: the FoC equations have 82
  `Parameter_Category = GameObjectCategoryType[...]` literals (same bits either way) and 7 `AIGoalCategoryType[MajorItem]`
  ones, all on `ActiveGoals` / `BudgetFractionToBuild`, which do not use `category_param` [H data]. A non-category literal
  on a force leaf (`AIGoalCategoryType[MajorItem]` -> 11 = bits 0xB) now filters by those bits as the qword read implies;
  before it matched nothing by name and so meant "all".
- `AiDefinitions::load_category_enum`: an entry with empty text now gets the running entry index (the §9.1 dynamic-enum rule;
  was `atoi("")` = 0). No shipped entry is empty.
- Result for shipped data: unchanged (same names, same values). Tests: `alamo_ai_tests` `test_enum_literal_files` (in-memory
  mod enum files: added names, hex bitfield values, empty-text index, a dropped vanilla name, a discarded converter, missing
  files, a preset table, `parse_expression` with a table) and the real-data check that the FoC tables came from the files.

### 1.3 Not changed (open)

- `PerceptionTokenType` is not offered as a literal converter: whether the registry 0x140b09350 holds it is not traced, and no
  shipped equation uses it [L].
- Which converters the registry holds at all is [M] (`A2I_TOKENS_RE.md` §3.2 assumes `DifficultyLevelType` and
  `HardPointType`); the port offers every `objects::EnumRegistry` converter plus those three.
- The **type side** of a category test still uses the built-in names: `ai_category_bit` / `fill_category_masks`
  (`galactic_ai_tokens.cpp`) and its callers in `galactic_ai.cpp` / `galactic_pp.cpp` build `GalacticAiTypeInfo::category_mask`
  from the type's category names with the built-in GameObjectCategoryType table. A mod that adds a category now builds its
  literals (before: the equation was dropped), but a type listing the new category gets no bit for it, and a mod that
  **renumbers** vanilla categories would make literal and type masks disagree. Fix (lane A / integrator): build the masks
  with the data-loaded registry (`objects::GameObjectTypes::enums()` already loads the files, `game_object_types.cpp`
  `load_dynamic`), or take `GameObjectType` `CategoryMask` directly.

## 2. `Function_X.Evaluate` naming an equation that failed to build [spec missing]

- Known [H] (`A2I_TOKENS_RE.md` §3.2): a failed build means the equation is never added to the equation manager; exe sites
  that look it up by name get null.
- Not known: whether the DLL resolves `Function_X` while building the referencing equation (then that equation would fail to
  build too, and its own referrers in turn) or at evaluation (then only evaluations reaching the reference fail)
  (`A2I_TOKENS_RE.md` §4 [L]). The DLL's handling of a name that is not (yet) defined was not traced.
- Shipped data gives no evidence: FoC and EaW load with 0 parse errors and **0 unresolved `Function_` references**
  (`alamo_ai_tests` real-data line: FoC 21 files / 381 equations, EaW 12 / 337).
- Port rule (unchanged, [port], pinned by the a2i test in `test_ai.cpp`): the referencing equation builds and is registered;
  `link()` counts the reference in `LoadReport::unresolved_functions`; an evaluation that reaches it fails with
  `EvalError::Function`, which fails the whole evaluation like any failing leaf (`AI_GALACTIC_3.md` §7.1 rule). A later file
  that defines the name successfully satisfies the reference (the dropped definition never entered the index).
- Goal-function sets naming a dropped equation (`BASICOFFENSIVEGALACTICSET.XML` style `<Function>` lists): `ai_goals.cpp` is
  lane A's; the A2I page leaves it [L] (skip or crash).

**RE question 2.** In PerceptionFunctionG.dll, where is the `Function_<Name>` node bound to its equation: in the node build
0x16210 (callee that looks the name up in the equation manager, and what it does on a miss: build failure with a message,
or a node holding the name) or in the evaluate path? If at build time: does the file order matter (a reference to an
equation defined in a later file)? And at the goal-function load site: what does a null equation do at evaluation?

## 3. Converter-name case [spec missing]

- Entry names: trimmed, upper-cased, exact compare, so case-insensitive [H] (`OBJECT_TYPES.md` §9.1, 0x200350 / 0x200ad0).
- Converter names (the `Type` in `Type[...]`): the registry lookup 0x23b030 is not traced [L] (`A2I_TOKENS_RE.md` §4).
- Shipped data spells every converter name exactly as registered: FoC equations use `GameObjectCategoryType` 82×,
  `DifficultyLevelType` 27×, `AIGoalCategoryType` 7×, `HardPointType` 1×, no other spelling [H data]. So the choice cannot
  change shipped behaviour.
- Port rule (unchanged, [port]): ASCII case-insensitive, surrounding white space trimmed (pinned by
  `aigoalcategorytype[Infrastructure]` in `test_ai.cpp`). If the original is case-sensitive, a mod writing
  `gameobjectcategorytype[...]` loses that equation in retail and keeps it in the port.

**RE question 3.** 0x23b030 (exe callback 9 of `Init_Perception_DLL`, `DatabaseUIntConversionClass` registry 0x140b09350):
how is the converter name compared (a hash of the upper-cased name, `_stricmp`, or an exact `std::string` compare / CRC of
the raw text)? Is the text trimmed first?

## 4. Fog mask 0x40 in the force leaves [rule known; not modelled]

### 4.1 What the specs define

- `Target.<side>.MaintenanceUnnormalized` (0x6daf20) and `Target.<side>.Maintenance` (0x6dabc0) are **fogged** planet-force
  leaves with mask 0x40, lifetime 15 s (`A2I_TOKENS_RE.md` §1.2 / §1.3, registration 0x6d83c0(…, 0x40, …, 0xf)) [H].
- Fogged-leaf mechanism (`AI_GALACTIC_TOKENS.md` §0) [H]: visibility mask M (+0xD0), visible flag (+0xC0), last-seen frame
  (+0xC8). On visibility signal 0xE for the evaluator's player: `(event mask & M) == M` -> recompute every cached entry, visible
  = 1; else, if it was visible -> recompute, visible = 0, last-seen = now. `IsVisible` = visible flag **or** the mode's fog
  option off (galactic: GameConstants `AIUsesFogOfWarGalactic`, shipped **False**). Evaluate: visible -> the normal cache /
  compute; not visible -> the last cached value for that binding set regardless of age (a live compute and store when none).
  `TimeLastSeen*` suffixes read the stamp.
- The signal's level (`GALACTIC_REVEAL_CORRUPTION.md` §2.3 presence row) [H]: 0x71 when an orbit-list object of the player is
  at the planet (0x71 = 0x1 | 0x10 | 0x20 | **0x40**), OR 0x191 when a ground-list object is (no 0x40); −1 (all bits) when the
  player owns the planet; else the faction default (Rebel / Underworld 0x11, Empire 0: no 0x40). Sent only for players
  with an AI system. So with fog on, the Maintenance leaves of planet P are live for AI player A only while A owns P or has an
  object in P's orbit; otherwise they hold the value computed when that stopped being true. 0x40 is `Fleet_Contents` in
  tVisibilityLevelType (`OBJECT_TYPES.md` §9.1.3), which fits "upkeep of the fleets here" [M].

### 4.2 Why the port does not model it

- `GalacticSimTokenResolver` (`galactic_ai_tokens.cpp`) has **no fogged leaves at all**: every leaf there is live, and the
  `TimeLastSeen*` / `AverageAgeOf*Intelligence` leaves return 0 (fog off) [port, documented in `galactic_ai_tokens.hpp` and
  `galactic_ai.hpp` "The A2 token resolver stays live either way"]. Only the S2 `GalacticPerception` remembers values under
  fog (masks 0x1 / 0x20 / 0x80, `galactic_perception.hpp`). Mask 0x40 alone cannot be modelled without the shared mechanism.
- The resolver is not told whether fog is on: `GalacticAiConfig::ai_uses_fog_galactic` reaches `snapshot_from_sim` only
  (owner `galactic_ai.cpp`, lane A).
- Shipped data: fog off, so retail evaluates these leaves live, as the port does; no shipped equation reads
  `Target.*Force.Maintenance*` anyway (`A2I_TOKENS_RE.md` §1.3) [H data]. No golden can change.

### 4.3 Design for the follow-up (all fogged leaves of the A2 resolver)

1. A resolver input `fog` (= `AIUsesFogOfWarGalactic`), set by `GalacticAiWorld` (lane A); off = today's behaviour.
2. `leaf()` gets the mask: planet leaves 0x1 / 8 / 0x80 / 0x200 / 0x400, force leaves 0x2 / 4 / 0x10 / 0x20 / **0x40** /
   0x80 (`AI_GALACTIC_TOKENS.md` §0 [H masks, M meaning]; the Maintenance pair is the only [H] leaf-to-mask link).
3. Per (Self player, planet) keep the last presence level (`GalacticSim::visibility_sources(planet, player).presence`); in
   `refresh(frame)` (sim thread, before evaluation) detect each mask's transitions. On visible -> hidden, retail recomputes the
   cached entries at that moment: the port needs the compute of each cached entry (store the token path + params in the
   entry, re-resolve in `refresh`) or must accept freezing the last value computed while visible (differs by up to the 15 s
   lifetime) [port choice].
4. Hidden evaluate: the cached value regardless of age, else compute and store. `TimeLastSeen*` from the stamp.
5. Save the visible flags and stamps with the cache (N1 resolver chunk).

## 5. Confidence summary

| claim | tag |
|---|---|
| literals convert with the exe's converter objects; dynamic ones come from `Data\XML\Enum` through the VFS | H |
| file rules (atoi / %llx, empty text, duplicate discards) | H (empty bitfield text M) |
| `DifficultyLevelType` / `HardPointType` are compiled-in tables | H (registry membership M) |
| `PerceptionTokenType` in the literal registry | L (not offered) |
| reference to a dropped equation: build-time or evaluation-time failure | L (port: evaluation failure) |
| converter-name compare case-insensitive | L (port: yes); entry names H |
| Maintenance leaves fogged with mask 0x40; presence level 0x71 includes 0x40 | H |
| 0x40 = Fleet_Contents meaning | M |

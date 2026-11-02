# GameObjectType loading (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by the RE agent for roadmap step S1 (game object model). Builds on `STRUCTS.md`
(GameObjectTypeClass), `xml_field_tables.md` (tag → type code → offset), `xml_tags.md` and
`PHASE2_ANSWERS.md` §13 (duplicates), and does not repeat their tables. Clean-room: prose and tables
only, no code or pseudo-code from the decompilation. Addresses are RVAs in StarWarsG.exe (Ghidra names in
`docs/re/ghidra/functions.csv` are often misattributed; the address is authoritative). XML statistics
are from vanilla FoC (`extracted/foc_data_text.zip`) and EaW (`extracted/eaw_data_text.zip`).

Confidence: **[H]** read in the code and cross-checked, **[M]** read in the code, intent inferred,
**[L]** inferred, needs a probe.

---

## 0. Summary for the implementer

1. **One list, one pass per file, in list order.** `.\Data\XML\GameObjectFiles.xml` (opened through the
   VFS, so a mod's copy replaces the whole list) names the files; each listed file is parsed in list order,
   each top-level child element of the file root becomes one new type, in document order. [H] (0x408830,
   0x408290)
2. **The root element tag of a type (`SpaceUnit`, `Props_Story`, …) carries no behaviour.** It is stored
   (CRC of the upper-cased tag) and has exactly one reader found: a `MultiplayerStructureMarker` check. What
   a type *is* comes from its fields (behaviour lists, `CategoryMask`, …). [H] for storage, [M] for "no
   other reader". §2.
3. **Names are case-insensitive; the first definition of a name wins lookups** but later duplicates are
   still created and keep their own type index. [H] (0x407e00, 0x408290; PHASE2_ANSWERS §13)
4. **Variant = full copy of the fully resolved parent, then the child's own XML is re-applied on top.**
   The parent must already be resolved. Forward references and cross-file references work through up to
   9 extra resolve passes; cycles, self-references and missing parents leave the type unresolved silently
   (its own tags on top of class defaults). §3. [H]
5. **List fields in a variant replace the parent's list; they never append to it.** Within one element,
   a repeated list tag appends in a normal type, but in a variant each occurrence of most list kinds clears
   again, so only the last occurrence survives. Type-name lists (`Company_Units`, `Squadron_Units`,
   `Death_Explosions`, …) are the exception and accumulate all occurrences. An empty tag never changes
   anything. §3.3, §4.4. [H]
6. **Scalars: the last occurrence wins. Unknown tags are ignored silently. A value of exactly `TBD` is
   skipped with a log line.** Tag names are case-insensitive. Values are whitespace-trimmed by the XML
   reader. Bools are true iff the value is exactly `1` or starts with `Y`, `y`, `T` or `t`. §4. [H]
7. **Nested elements are transparent**: an element child that itself has children is descended into and
   its children are parsed as fields of the same type, unless it carries `SubObjectList="Yes"` (`Abilities`,
   `Unit_Abilities_Data`), which goes to a sub-object parser. §4.2. [H]
8. **References to other types are lazy.** Single and list type-name fields store the string and are
   resolved by name after every pass (`None` → null, unknown → null, silently). §4.5. [H]
9. **Post-load derivation matters for game logic.** Companies and squadrons get `CategoryMask`, combat
   power, speeds and (when the cost is negative) `Build_Cost_Credits` derived from their members. The
   build base-level requirement is normalised. `Targeting_Min_Attack_Distance` is raised to the hardpoints'
   minimum fire range. §6. [H]

---

## 1. Files, load order and overrides

### 1.1 Where it runs [H]
The engine init sequence initialises (in this order) the faction list, the targeting-priority sets, the
`HardPointDataManager`, the hero-clash types, and only then `GameObjectTypeManager.Read_Parse_Game_Object_Databases`
(0x408830, manager singleton at 0x140b2f020) with the path `.\Data\XML\GameObjectFiles.xml`. Factions and
hardpoints therefore exist before any object type is parsed.

### 1.2 The file list [H]
- The list file is opened with the same XML reader as everything else. Every child element of its root is
  read as one file name (the child's tag is not checked; vanilla uses `<File>`). [M] for "tag not checked".
- For each name, `.\Data\XML\` + name is opened through the VFS. A missing file logs
  `Read_Parse_Game_Object_Databases failed to find specified file` and is skipped. Each found file is
  appended to the manager's file list (std::list at manager +0x50) with its timestamp (+0x60 vector), in
  list order. A name listed twice is parsed twice.
- Files are then parsed in that order (§1.4). Path case does not matter (Windows file system / VFS).

### 1.3 EaW vs FoC, mods [H]
- The FoC executable loads exactly one list. Under the Phase-2 VFS order (PHASE2.md "Data\ resolution
  order") the FoC `corruption\Data\XML\GameObjectFiles.xml` wins over EaW's. **There is no merge of the
  EaW and FoC lists.** The FoC list (120 files) contains all 65 EaW entries (reordered:
  `GroundBuildablesSkirmish.xml` moved after `StarBases.xml`, the `CIN_*` and `Props_*` files moved to the end)
  plus 55 expansion files.
- Each listed file resolves independently through the VFS. A file that only exists in EaW `GameData` is
  still found through the fallback chain.
- A mod overrides at **file granularity**: shipping `Data\XML\SpaceUnitsCapital.xml` replaces that whole
  file. Shipping `GameObjectFiles.xml` replaces the whole list. Two files are never merged.
- Vanilla counts: FoC 3345 type elements (no duplicate names, 911 variants, 0 forward references, 225
  cross-file variants, chain depth ≤ 3). EaW 1887 elements, 449 variants and **7 forward references**
  (e.g. `N_Cantina` in `GroundBuildablesSkirmish.xml` → `Ground_Cantina_E` in the later
  `SpecialStructures.xml`). The EaW list also names 11 `CIN_*` files that the EaW text zip does not contain.
  Use the EaW set as the regression test for the multi-pass resolve.

### 1.4 Per-file parse (0x408290) [H]
- The file is parsed into a DOM. Each **top-level child element** of the file root is one type definition.
  The root's own tag is irrelevant: vanilla roots include `SpaceUnitsCapital`, `Props_Story`,
  `Grand_Admiral_Thrawn_Data`, ….
- For each element, in document order, a type object is created (size 0x2950):
  1. It is constructed with defaults (0x367250). The constructor calls the field parser on the element
     (0x376f90, "first parse") and then the per-parse fix-up (0x3782a0).
  2. It gets its **type index** = current number of types (stored at type +0xf0). This is the global
     creation order, duplicates included.
  3. It records its source file name (type +0x160) and a **content hash** of the element (+0x163c; sum
     of CRCs of the `Name` attribute and every tag/value, recursively; 0x370360; only used by hot reload).
  4. If `Variant_Of_Existing_Type` is empty, the type is marked **resolved** (byte +0x68 = 1). Otherwise it
     tries to resolve now (§3).
  5. It is appended to the type array (manager +0x28, data +0x30, count +0x38). It is inserted into the
     name map (manager +0x40, std::map keyed by name CRC) **only if the key is absent**.

### 1.5 Element tags in vanilla (FoC) [H]
The tag is free-form; 62 distinct tags occur. Counts: `Props_Story` 261, `SpaceProp` 242, `UpgradeObject` 194,
`Projectile` 174, `Particle` 165, `GroundBuildable` 158, `Indigenous_Unit` 138, `GroundStructure` 127,
`GroundCompany` 119, `Props_Generic` 110, `Marker` 105, `SpecialStructure` 99, `Prop_Felucia` 97,
`SpaceUnit` 94, `GroundInfantry` 92, `UniqueUnit` 91, `GroundVehicle` 77, `Props_Urban` 76,
`Props_Temperate` 68, `Planet` 65, `HeroCompany` 52, `Prop_Desert` 52, `Container` 51, `Cin_GroundProp` 51,
`StarBase` 50, `Prop_Forest` 49, `MultiplayerStructureMarker` 48, `HeroUnit` 39, `Squadron` 33,
`SpaceBuildable` 30, `TransportUnit` 24, `LandPrimarySkydome` 22, `Props_Snow` 22, `GroundBase` 21,
`Props_Swamp` 20, `Props_Volcanic` 20, `GenericHeroUnit` 18, `CIN_SpaceProp` 18, `Cin_SpaceProp` 17,
`SpaceStructure` 16, `SecondaryStructure` 15, `SpaceSecondarySkydome` 13, `MOV_Cinematic` 12,
`Cin_GroundVehicle` 12, `MiscObject` 10, `SpacePrimarySkydome` 9, `Slave_Unit` 8, `SlaveCompany` 8,
`LandSecondarySkydome` 7, `Cin_SpaceUnit` 7, `ScriptMarker` 6, `CIN_GroundInfantry` 6,
`Mobile_Defense_Unit` 5, `Cin_Projectile` 5, `TechBuilding` 4, `CIN_SpaceUnit` 4, `LandBombingUnit` 3,
`SpecialEffect` 2, `Cin_TransportUnit` 2, `Starbase` 1, `Cin_GroundStructure` 1.

The only element attributes used are `Name` (all 3345) and `Description` (4, ignored).

---

## 2. The element tag ("object class") [H]/[M]
- At the top level of the first parse, the element tag is upper-cased, CRC'd and stored at type +0x118. The
  tag string is registered in a global CRC → tag map (0x140b2c240); getter 0x3712e0.
- The `Name` attribute is upper-cased and stored as the type's name string (+0xf8). In the fix-up, the CRC of
  the upper-cased name goes to +0x11c (map key) and +0xec.
- In a variant the child **keeps its own element tag**. 214 vanilla variants differ from their parent's tag,
  e.g. `GroundInfantry` variants of `Indigenous_Unit` parents, or `GroundBuildable` variants of
  `SpecialStructure` parents.
- The only reader of the stored tag found is 0x295cf0, which compares it case-insensitively with
  `MultiplayerStructureMarker` (multiplayer start-marker / ownership remap). [M] that nothing else reads
  +0x118.
- **Port:** keep the tag string on the type, but drive every behaviour from fields. The root/element tag
  names in the task brief (SpaceUnit, UniqueUnit, …) are conventions for readers, not loader classes.

---

## 3. `Variant_Of_Existing_Type` inheritance

### 3.1 Data [H]
- `Variant_Of_Existing_Type` (type code 0x1d) stores the parent name as a heap string at +0x1e28. The
  slot at +0x1e30 receives the parent pointer once the lazy-reference pass resolves it (§4.5).
- "Resolved" is the byte at +0x68.

### 3.2 Resolving one variant (0x408160 → 0x376cc0) [H]
1. Find the parent: use the cached pointer at +0x1e30 if it is set. Otherwise look the name up in the
   name map (upper-cased CRC; 0x407c00). No parent → stay unresolved.
2. If the parent is **not resolved** (+0x68 = 0), nothing happens (the child stays unresolved).
3. Otherwise:
   a. The child saves its identity: name string, name CRC, type index, element-tag CRC, source file name,
      content hash, the variant name and parent pointer, and the +0xec/+0x08 words.
   b. The child releases its Lua script object.
   c. **The whole parent object is copy-assigned onto the child** (0x36c000, member-wise deep copy: strings,
      vectors, sub-object lists, bitsets, the resolved flag).
   d. The copied script-object pointer is dropped (the child never shares the parent's script instance).
   e. The saved identity is restored.
   f. **The child's own XML element is parsed again** with the "variant" flag set (0x376f90, third
      argument 1).
   g. The per-parse fix-up runs (0x3782a0), which reloads the Lua script, recomputes CRCs, etc.
   h. The child is marked resolved.
- Consequences:
  - Inheritance is "parent's final state, then child's tags". The child's defaults never show through for
    fields it does not mention.
  - The parent's derived values are inherited too (implicit behaviours, behaviour bitsets, CRCs), then
    recomputed.
  - Grand-children work because the parent's state already contains the grand-parent's.
  - Everything is inherited, including `Is_Dummy`, `Lua_Script`, `Text_ID`, `CategoryMask` and the
    model names.

### 3.3 Merge rules in the variant re-parse [H]
The variant flag reaches every field converter (0x51a280, last argument).
- **Scalars** (bool, int, float, string, enum, single type ref, vectors): overwritten if the child has the
  tag, kept from the parent otherwise.
- **Clearing list kinds.** These type codes clear the destination **on every occurrence** when the flag is
  set, then append the tokens:
  - 0x12 int list, 0x13 float list, 0x14 Vector3 list (`Squadron_Offsets`), 0x15;
  - 0x18, 0x19, 0x1a, 0x1b string vector (`Affiliation`, `SurfaceFX_Name`, `Land_Damage_SFX`);
  - 0x2a string list (`Behavior`, `LandBehavior`, `SpaceBehavior`, `GalacticBehavior`, `HardPoints`,
    `Encyclopedia_*`, `Fire_Category_Restrictions`, `Target_Bones`, …);
  - 0x2b, 0x2d, 0x34, 0x35, 0x37;
  - 0x38 `Abilities`, 0x39 `Unit_Abilities_Data`, 0x3a;
  - 0x3d `Targeting_Fire_Inaccuracy`;
  - 0x44, 0x45, 0x48, 0x4f, 0x51, 0x55 (`Tactical_Buildable_Objects_*`).

  Result: the child's list replaces the parent's. If the child repeats the tag, **only the last
  occurrence survives** (unlike a normal type, §4.4).
- **Type-name lists (0x1e; probably also the per-key lists inside 0x3b `Death_Clone` [M]).** These hold an
  "inherited" flag that the copy sets (0x4c4920). The first append after the copy clears the list (0x4c4bb0), and later
  appends accumulate. Result: the child's list replaces the parent's, and **all child occurrences are
  kept**. Affected tags: `Company_Units`, `Squadron_Units`, `Company_Transport_Unit`, `Death_Explosions`,
  `Projectile_Types`, `Required_Planets`, `Required_Orbiting_Units`, `Tactical_Build_Prerequisites`,
  `Planet_Slice_Affinity_Types`, `Planet_Restricted_Unit_Types`.
- **No clear, plain append** (code 0x46 `Starting_Spawned_Units_Tech_N`, `Reserve_Spawned_Units_Tech_N`,
  `Potential_Indigenous_Power`; 0x47 `Destruction_Survivors`, `Map_Load_Spawn_Table`; 0x53
  `Required_Special_Structures`): the child's entries are appended to the parent's copy. [M] (no clear path
  in the converter; a container-level reset like 0x1e was not checked).
- **Bitmask enums (0x54/0x56: `CategoryMask`, `Property_Flags`, `Garrison_Category`,
  `Attack_Category_Restrictions`, `Targeting_Priority_Set`)** are scalars: the child's value replaces the
  parent's whole mask. There is no OR.
- **Empty tags** (`<HardPoints></HardPoints>`, `<X/>`) are no-ops before any type dispatch. **An empty tag
  cannot clear an inherited list or string.** Vanilla has 58 empty 0x2a tags, 384 empty 0x1e tags and 547
  empty strings. [H]

### 3.4 Passes, ordering, failure cases [H]
- **Pass 0** creates every type. At creation a variant resolves immediately if its parent is already in
  the map **and** resolved. A parent defined earlier in load order is therefore enough, even in another
  file.
- After each pass:
  1. The lazy-reference resolution runs (§4.5); this fills +0x1e30.
  2. Then per-type derivation 0x377500 runs on every type (§6.2).
  3. Then every type with `Lua_Script` gets a script path recorded.
  4. If every type is resolved, loading stops.
- **Passes 1–9** cover each listed file that contains an unresolved type. The file is re-read, and for each
  element the type is found **by name** (first definition) and resolution is retried.
  - The file is re-read once per unresolved type it contains (wasteful but harmless).
  - Already-resolved variants in that file are re-applied (idempotent).
- **Limit:** 10 iterations in total (pass 0 + 9). A forward chain longer than that, or any unresolved
  type, stops the loop **without an error message**.
- **Missing parent / cycle / self-reference:** the type stays unresolved. It keeps only "class defaults +
  its own tags" (its first parse). It stays in the array and in the name map, with no warning. [M] that
  nothing later rejects unresolved types.
- **Duplicated variant names:** the re-pass lookup always lands on the first definition. A later duplicate
  that failed in pass 0 can never resolve.
- **Port:**
  - Resolve with an explicit dependency walk (topological; detect cycles and report them) instead of
    passes. The result is the same as long as no chain exceeds 9 forward hops.
  - Log missing parents and cycles. The original does not, so a warning-only diagnostic is
    compatible.

### 3.5 Other inheritance-like mechanisms
- **`Death_Clone`** (0x3b): pairs of `<key>, <TypeName>`. The key is a death/damage-type enum, or else a
  `Damage_Type` name, which maps to an index + 0x18. Each key holds a type-name list. This is a lookup
  for "spawn this type when dying by X", not inheritance. [H]
- **Single type references (0x1d)** are lazy pointers, not inheritance:
  - level and upgrade chains: `Prev_Level_Base`, `Next_Level_Base`, `Previous_Upgrade_Level_Type`,
    `Next_Upgrade_Level_Type`;
  - tactical construction: `Tactical_Additional_Structure_Type`, `Tactical_Buildable_Constructed`;
  - projectile effects: `Projectile_*_Detonation_Particle`, `Projectile_Absorbed_By_Shields_Particle`;
  - hardpoint projectiles: `Fire_Projectile_Type`. [H]
- **Companies / squadrons** derive several fields from their members after load (§6.2). This is the
  only "upward" inheritance. [H]
- **Galactic/tactical/land/space variants** are not separate types. Per-mode data are separate fields on
  one type (§5.2).
- **HardPoints** do not support `Variant_Of_Existing_Type`: the tag is not in their field table and is
  unused in vanilla. [H]

---

## 4. Field parsing rules

### 4.1 Pipeline [H]
For every child element of the type element (0x376f90):
1. Tag = element name, value = its character data.
2. If the value is exactly `TBD` (case-sensitive, 3 chars), log
   `Warning: GameObjectTypeClass::Parse_Database_Entry() - %s key %s value is TBD.` and skip.
3. Otherwise, dispatch (0x5180d0):
   - an empty value returns at once (no change);
   - the tag is upper-cased, CRC'd and binary-searched in the GameObjectType descriptor table;
   - an unknown tag returns "not handled", which the caller **ignores silently**;
   - a known tag calls the type-code converter (0x51a280) with the field offset.
4. If the child has element children, see §4.2.

At the end of the top-level parse:
- implicit behaviours are appended (§6.1);
- the five behaviour bitsets are rebuilt (§5.3);
- `Political_Faction` is resolved to a faction pointer.

### 4.2 Nested elements [H]
- A child with element children and the attribute `SubObjectList="Yes"` (exactly `Yes`) is dispatched
  with its tag as the value. The 0x38/0x39 converters then build sub-objects from the child elements.
  `Abilities`: 412 uses; `Unit_Abilities_Data`: 246. If the dispatch fails, it logs
  `Unprocessed sub-object list '%s' in object '%s'`.
- Any other child with children is **descended into**. Its children are parsed as fields of the same type,
  at any depth. Only the outermost level does the name/element-tag handling.

### 4.3 XML text, whitespace, case [H]
- The game XML reader trims leading and trailing space, tab, CR and LF from every value (0x23a4b0, trim
  enabled by the loader). Vanilla has thousands of values like ` true ` or `\n\t\tYes` that rely on this.
- Character data must come before any child element. Otherwise the reader logs an error and skips the
  extra text.
- **Tag names are case-insensitive** (upper-cased before the CRC lookup). The `Name` attribute and every
  type-name lookup are case-insensitive. The attribute *name* `Name` was not checked for case-insensitivity [L].
- Enum and bitmask values are case-insensitive and trimmed; see §9.1 [H].

### 4.4 Repeated tags in one element [H]
- **Scalars: last occurrence wins.**
- **Lists** in a normal (non-variant) parse: each occurrence **appends**. Vanilla relies on this for
  `Targeting_Fire_Inaccuracy` (107 elements repeat it), `Death_Clone` (66), `SFXEvent_Attack_Hardpoint`,
  `Starting_Spawned_Units_Tech_0`, `Squadron_Units` and `Death_Explosions`.
- In a variant, see §3.3. Most list kinds keep only the last occurrence; type-name lists keep all of them.
- Vanilla FoC variants that repeat a clearing list tag: `Targeting_Fire_Inaccuracy` 14 variants,
  `Attack_Category_Restrictions` 3, `SFXEvent_Hardpoint_Destroyed` 3. **In those variants only the last
  line is effective.** Reproduce this exactly.

### 4.5 Converters by type code (0x51a280) [H] unless marked
Lists are tokenised with the delimiters space, comma, tab, CR and LF (consecutive delimiters collapse). For
many list and vector codes, a value of 8192 or more characters is ignored.

| code | kind | parse rule |
|---|---|---|
| 0x0 | bool (1 byte) | true iff value is exactly `1` or its first char is `Y`/`y`/`T`/`t`; anything else false (`No`, `0`, `False`, `Nos`, `np` → false; `10` → false) |
| 0x1, 0x5, 0x6 | int32 | C `atoi` (leading sign/digits, rest ignored, garbage → 0). 0x5 and 0x6 behave the same at parse time |
| 0x2, 0x3 | int8 | `atoi`, truncated |
| 0x4 / 0x7 | uint8 / uint32 | `atoi`, clamped to ≤ 100 |
| 0x8 | float | C `atof` to double, stored as float |
| 0x9 | float | as 0x8, clamped to [0,1] with log `DatabaseMapper: Key(%s) needs to have a value between 0 and 1` |
| 0xa / 0xb | double | 0xb clamped to [0,1] |
| 0xe | enum → int32 | per-field converter object; on failure logs `Failed to convert value` and leaves the field |
| 0xf | 2 floats | first two tokens; missing tokens leave the rest untouched |
| 0x10 | Vector3 | three tokens (`Galactic_Position`, `Name_Adjust`, `Space_Obstacle_Offset`) |
| 0x11 | 4 floats | colour |
| 0x12 | int list | appended |
| 0x13 | float list | appended (`Land_Damage_Thresholds`) |
| 0x14 | Vector3 list | groups of three (`Squadron_Offsets`) [M] for group delimiter |
| 0x16 | RGBA | four ints |
| 0x17 | std::string | raw value |
| 0x1b | vector of std::string | appended (`Affiliation`, `SurfaceFX_Name`) |
| 0x1c | std::string | upper-cased |
| 0x1d | single type ref | name string stored and registered for lazy resolution |
| 0x1e | type-ref list | each token appended, registered (inherited-flag semantics §3.3) |
| 0x1f, 0x20, 0x21 | SFX / speech / music event refs | name stored; event objects resolved lazily or by their managers [M] |
| 0x2a | string list | each token appended |
| 0x38 / 0x39 | sub-object lists | built from child elements (`Abilities`, `Unit_Abilities_Data`) |
| 0x3b | keyed type-name lists | `Death_Clone` (§3.5) |
| 0x3d | keyed float list | `Targeting_Fire_Inaccuracy`: `<category>, <float>` pairs [M] |
| 0x46 | (type, count) list | `Starting_Spawned_Units_Tech_N`, `Reserve_Spawned_Units_Tech_N`: `<TypeName>, <int>` |
| 0x47 | list | `Destruction_Survivors`, `Map_Load_Spawn_Table` (format not checked) |
| 0x53 | OR-grouped type list | `Required_Special_Structures`: `\|` starts a new alternative group |
| 0x54 / 0x56 | 64-bit mask | whole value through a converter (`CategoryMask`, `Property_Flags`, `Garrison_Category`, `Attack_Category_Restrictions`); vanilla writes `A \| B` [M] for token syntax |
| 0x55 | faction-keyed list | `Tactical_Buildable_Objects_*`: `<Faction>, <Type>, …` |

Lazy references:
- Single type refs (0x1d) and type-ref lists (0x1e) are added to two global registries.
- After every pass (0x378dd0), each name is compared case-insensitively with `None` (→ null). Otherwise it
  is looked up by name: first definition, null if unknown, no warning.
- Four further registries resolve other names after every pass: an effect/particle manager, a
  0x140b2b300 manager, a 0x140b27dc0 manager, and factions (0x4c56c0, 0x4c5750, 0x4c5620, 0x4c5580).
- The registries are freed after loading. References created later are not auto-resolved. [H] for the
  mechanism, [M] for the manager identities.

### 4.6 Defaults [H]
- The constructor (0x367250) plus the bool block initialiser (0x367100) set every default. Everything not
  listed below is 0, false, empty or null.
- **Bools true by default:**
  - `Is_Editor_Placed`, `Cache_Crusher_Boxes`, `Movie_Object`, `Is_Homogeneous`, `Is_Discardable`,
    `Is_Valid_Target`, `Model_Visible_To_Enemy`;
  - `Can_Participate_In_Space_Battle`, `Can_Hyperspace_Without_Activating_Ability`;
  - `Projectile_Does_Shield_Damage`, `Projectile_Does_Hitpoint_Damage`;
  - `Is_Visible_On_Enemy_Radar`, `Radar_Show_Facing`;
  - `Influences_Cash_Point`, `Influences_Capture_Point`;
  - `Should_Cause_Limited_Turrets_To_Turn`, `Targeting_Allowed_When_Burning`,
    `Targeting_Land_Model_Stay_Horiz_Flat`;
  - `Can_Indigenous_Unit_Stop`, `Is_Indigenous_Unit_Corruptible`;
  - `Shield_Requires_Base_Power`, `TSW_Start_Pos_Match_Targets_Z`;
  - `Death_Clone_Is_Obstacle`, `Death_Leave_Hulk_Behind`;
  - `Fine_Tune_Occupied_Passability`, `Display_Contained_Hero_Grab_Bars`, `Is_Branched_Map_Discardable`;
  - `Should_Be_Destroyed_When_All_Hardpoints_Destroyed`;
  - `Fire_Weapon_When_Deployed`, `Fire_Weapon_When_Undeployed`,
    `Fire_Weapon_When_In_Rocket_Attack_Mode`, `Fire_Weapon_When_In_Normal_Attack_Mode`;
  - `Planet_Surface_Accessible`, `Planet_Retains_Residual_Influence`, `Planet_Is_Destroyable_By_TSW`;
  - `Destroy_Previous_Upgrade_Level`, `Show_In_Sidebar_While_Building`, `Show_In_Sidebar_When_Complete`,
    `Show_In_Sidebar_Tray`, `Tactically_Built_Child_Object_Persists`;
  - `Auto_Deploys`, `Should_Reinforcements_Move`, `Use_Transported_Object_Bounds_For_Landing_Z`;
  - `Terrain_Texture_Modifier_Square`;
  - `Destroy_When_Used_As_Corruption_Prereq`, `Respects_Skirmish_Pre_Build_Bases_Option`.
- **All other bools default false**, e.g. `Is_Visible_On_Radar`, `Victory_Relevant`,
  `Collidable_By_Projectile_Living`, `Hyperspace`, `Is_Dummy`, `Is_Named_Hero`, `Build_Initially_Locked`.
- Non-zero numeric defaults are listed in §5.4. Notable ones:
  - `Tactical_Health` 100.0, `Max_Speed` 0.25, `Max_Rate_Of_Turn` 2.3, `Mass` 0.995, `Scale_Factor` 1.0,
    `LOD_Bias` 4.0;
  - `Armor_Type` "Armor_Default", `Shield_Armor_Type` "Shield_Default", `Damage_Type` "Damage_Default";
  - `Max_Squad_Size` 10, `Ranking_In_Category` 25;
  - all `Build_Limit_*` and `Build_Max_Instances_Per_Planet` −1 (unlimited);
  - `Tactical_Build_Cost_Campaign` −1, `Space/Land_Override_Population_Value` −1;
  - `MovementClass`, `Ship_Class`, `OccupationStyle`, `UnitCollisionClass` −1 (none).

---

## 5. In-memory layout for the implementer

### 5.1 Identity and bookkeeping (not XML) [H]
| offset | meaning |
|---|---|
| +0x00 | vtable (GameObjectTypeClass) |
| +0x08 | word preserved across variant copy (unknown) |
| +0x68 | resolved flag |
| +0xec | name CRC (initialised to 0x3fffff) |
| +0xf0 | type index (creation order) |
| +0xf8 | name, upper-cased (std::string) |
| +0x118 | element-tag CRC |
| +0x11c | name CRC (map key) |
| +0x140 | localized display name (from `Text_ID` +0x120) |
| +0x160 | source file name |
| +0x163c | element content hash (hot reload) |
| +0x1e28 / +0x1e30 | variant parent name / parent pointer |
| +0x1af8 | Lua script object (from `Lua_Script` +0x1ad8; loaded only if `Execute_Script_On_Type`) |

Manager (0x140b2f020):
- +0x28 type array (data +0x30, count +0x38);
- +0x40 CRC → type map;
- +0x50 file list;
- +0x60 file timestamps;
- +0x78 Lua script list (path `.\Data\Scripts\GameObject\<Lua_Script>`, `.lua` appended if no dot);
- +0xa0 hot-reloaded types.

### 5.2 Field groups and per-mode split
Every type has one flat set of ~880 XML fields (offsets in `xml_field_tables.md`). Mode-specific data is
carried by **separate fields**, not separate types: [H]
- **Galactic:**
  - presentation: `Galactic_Model_Name`, `Galactic_Fleet_Override_Model_Name`, `Galactic_Position`,
    `GalacticBehavior`;
  - economy and production: `Build_Cost_Credits`, `Build_Time_Seconds`, `Tech_Level`, `Maintenance_Cost`,
    `Build_Limit_*`;
  - build requirements: `Required_Timeline`, `Required_Ground_Base_Level`, `Required_Star_Base_Level`,
    `Required_Special_Structures`, `Required_Planets`, `Base_Level`;
  - planets: `Planet_*`, `Political_Control`.
- **Land tactical:** `Land_Model_Name`, `LandBehavior`, `Land_FOW_Reveal_Range`, `Land_Damage_*`,
  `Land_Override_Population_Value`, `Tactical_Build_*`, `Tactical_Buildable_Objects_*`,
  `Garrison_Category`, `MovementClass`, `OccupationStyle`.
- **Space tactical:** `Space_Model_Name`, `SpaceBehavior`, `Space_FOW_Reveal_Range`, `Space_Layer`,
  `Space_Override_Population_Value`, `Hyperspace`, `Hyperspace_Speed`, `Layer_Z_Adjust`.
- **Shared by both tactical modes:** health, shields, energy, speeds, targeting, `Population_Value`,
  `Score_Cost_Credits`, `AI_Combat_Power`.
- There is **no `Is_Space`/`Is_Land` field**. Which modes an object exists in follows from the behaviour sets,
  the model names and fields such as `Space_Layer`/`MovementClass`. [M]

### 5.3 Behaviour sets [H]
- Behaviour names are matched case-insensitively against a static registry of 106 behaviour ids (table at
  0x140a2ac90, first entry `LAND_TEAM_CONTAINER_LOCOMOTOR`). Unknown names are ignored silently.
- Five bitsets are built (0x374dc0): Galactic (+0x750), Land (+0x760), Space (+0x770), generic `Behavior`
  (+0x790), and a union of all four (+0x780).
- `DeployedBehavior` (+0x730) is stored but not put into a set by this function.
- Implicit behaviours: after each top-level parse, registry entries whose "auto-add" predicate accepts the
  type are appended to the generic `Behavior` list by name (0x4c33d0). Only `STARSHIP_COMBATANT` has one; see
  §9.3. [H]

### 5.4 Most-used fields (prioritised for milestones 1–3)
Type code per §4.5. "Default" is the constructor value. Usage is the number of vanilla FoC elements
using the tag.

**Identity, presentation**
| tag | code | offset | default | notes |
|---|---|---|---|---|
| Text_ID | 0x17 | 0x120 | "" | localisation key; resolved to display name +0x140 (2358 uses) |
| Variant_Of_Existing_Type | 0x1d | 0x1e28 | — | §3 (911) |
| Is_Dummy / Is_Marker / Is_Decoration | 0x0 | 0x10 / 0x11 / 0x3b | false | non-gameplay objects |
| Is_Named_Hero / Is_Generic_Hero | 0x0 | 0x13 / 0x14 | false | |
| Galactic_Model_Name / Land_Model_Name / Space_Model_Name | 0x17 | 0x180 / 0x1e0 / 0x200 | "" | per-mode ALO |
| Scale_Factor | 0x8 | 0x658 | 1.0 | model scale (2008 uses) |
| LOD_Bias | 0x8 | 0x240 | 4.0 | |
| Icon_Name / Radar_Icon_Name | 0x17 | 0x1820 / 0x16f8 | "" | |
| Lua_Script / Execute_Script_On_Type | 0x17 / 0x0 | 0x1ad8 / 0x5d | "" / false | per-type Lua |
| Select_Box_Scale / Select_Box_Z_Adjust | 0x8 | 0x318 / 0x31c | 0 / −1.0 | |
| Blob_Shadow_Material_Name / _Scale | 0x17 / 0xf | 0x17d0 / 0x17c0 | "" / (1,?) | |

**Classification, ownership**
| tag | code | offset | default | notes |
|---|---|---|---|---|
| Affiliation | 0x1b | 0xe00 | empty | faction names; resolved to faction pointers +0xef0. If empty, the first `Is_Neutral` faction is added (§9.2) [H] |
| CategoryMask | 0x56 | 0x1648 | 0 | 64-bit (837 uses); company/squadron overwritten §6.2 |
| Property_Flags | 0x56 | 0x1650 | 0 | second mask word |
| Garrison_Category | 0x56 | 0x1658 | 0 | third mask word |
| Behavior / LandBehavior / SpaceBehavior / GalacticBehavior | 0x2a | 0x6d0 / 0x718 / 0x700 / 0x6e8 | empty | §5.3 |
| Political_Faction | 0x17 | 0x7a0 | "" | resolved to faction at end of parse (+0x7c0) |
| Space_Layer | 0xe | 0x16dc | 0 | |
| MovementClass | 0xe | 0x1688 | −1 | |
| UnitCollisionClass / OccupationStyle | 0xe | 0x169c / 0x1698 | −1 | |
| Ship_Class | 0x32 | 0x650 | −1 | |
| Weather_Category | 0xe | 0x168c | −1 → 0 in post-pass | |
| Victory_Relevant | 0x0 | 0x5e | false | |
| Is_Visible_On_Radar | 0x0 | 0x52 | false | |

**Economy, production, tech**
| tag | code | offset | default | notes |
|---|---|---|---|---|
| Build_Cost_Credits | 0x6 | 0x86c | 0 | if negative, derived from members (§6.2) |
| Build_Time_Seconds | 0x5 | 0x890 | 0 | galactic |
| Tech_Level | 0x5 | 0x89c | 0 | |
| Base_Level | 0x5 | 0x8a0 | 0 | |
| Build_Advances_Tech_Level | 0x5 | 0x894 | 0 | |
| Score_Cost_Credits | 0x6 | 0x878 | 0 | |
| Slice_Cost_Credits / Sabotage_Cost_Credits | 0x8 / 0x6 | 0x868 / 0x870 | 0 | |
| Maintenance_Cost | 0x8 | 0x798 | 0.0 | |
| Size_Value | 0x5 | 0x898 | 0 | |
| Build_Max_Instances_Per_Planet | 0x6 | 0x87c | −1 | −1 = unlimited |
| Build_Limit_Current_Per_Player / _Lifetime_Per_Player | 0x6 | 0x880 / 0x888 | −1 | |
| Build_Limit_Current_For_All_Allies / _Lifetime_For_All_Allies | 0x6 | 0x884 / 0x88c | −1 | |
| Build_Initially_Locked / Build_Can_Be_Unlocked_By_Slicer | 0x0 | 0xc1 / 0xc2 | false | |
| Build_Tab_Land_Units / _Space_Units / _Heroes / _Special_Structures | 0x0 | 0x4c–0x4f | false | |
| Required_Timeline | 0x5 | 0xf08 | 0 | |
| Required_Ground_Base_Level / Required_Star_Base_Level | 0x5 | 0xf0c / 0xf10 | 0 | normalised §6.1 |
| Required_Special_Structures | 0x53 | 0xf18 | empty | OR groups |
| Required_Planets | 0x1e | 0xf50 | empty | lazy type refs |
| Tactical_Build_Cost_Campaign / _Multiplayer | 0x6 | 0x9f8 / 0x9fc | −1 / 0 | |
| Tactical_Build_Time_Seconds | 0x5 | 0xa00 | 0 | |
| Tactical_Build_Prerequisites | 0x1e | 0xa10 | empty | |
| Tactical_Production_Queue | 0xe | 0xa30 | −1 | |
| Tactical_Sell_Percentage | 0x8 | 0xa08 | 0.5 | |
| Tactical_Bribe_Cost | 0x6 | 0x2358 | 0 | |
| Population_Value | 0x6 | 0x2124 | 0 | |
| Additional_Population_Capacity | 0x6 | 0x2120 | 0 | |
| Space_Override_Population_Value / Land_Override_Population_Value | 0x6 | 0x2128 / 0x212c | −1 | forced to 0 by post-pass if the space/land behaviour set has TACTICAL_SUPER_WEAPON (id 44) [H] |
| Company_Units / Squadron_Units | 0x1e | 0x950 / 0x970 | empty | members; entries {name, type*} at +0x958/+0x960 and +0x978/+0x980 |
| Company_Transport_Unit | 0x1e | 0x930 | empty | |
| Squadron_Capacity | 0x5 | 0x990 | 0 | |
| Max_Squad_Size | 0x6 | 0x1e44 | 10 | |
| Squadron_Offsets | 0x14 | 0x998 | empty | |
| Starting_Spawned_Units_Tech_0 / Reserve_Spawned_Units_Tech_0 | 0x46 | 0xad0 / 0xb60 | empty | (type, count); six tech slots of 0x18 bytes each from 0xad0 |
| Spawned_Squadron_Delay_Seconds | 0x8 | 0xa98 | 5.0 | |

**Combat**
| tag | code | offset | default | notes |
|---|---|---|---|---|
| Tactical_Health | 0x8 | 0xdcc | 100.0 | float; scaled at runtime by difficulty/faction (HOTSPOTS, 0x3727a0) |
| Shield_Points / Shield_Refresh_Rate | 0x8 | 0xdd4 / 0xdd8 | 0.0 | |
| Energy_Capacity / Energy_Refresh_Rate | 0x8 | 0xdd0 / 0xddc | 0.0 | |
| Damage | 0x8 | 0xdec | 0.0 | |
| AI_Combat_Power | 0x8 | 0xdf0 | 0.0 | company/squadron: sum of members |
| Armor_Type / Shield_Armor_Type / Damage_Type | 0x17 | 0x3d8 / 0x3f8 / 0x418 | "Armor_Default" / "Shield_Default" / "Damage_Default" | CRCs at +0x438/+0x43c/+0x440, registered with the damage table |
| HardPoints | 0x2a | 0xe18 | empty | names resolved to HardPoint data (+0xe38); unknown names dropped silently |
| Priority_Attack_HardPoints | 0x2a | 0xe78 | empty | converted to hardpoint-type ints (+0xe98) |
| Targeting_Max_Attack_Distance / _Min_ | 0x8 | 0x500 / 0x504 | 0.0 | min raised from hardpoints §6.2 |
| Targeting_Fire_Inaccuracy | 0x3d | 0x508 | empty | `<category>, <value>` pairs |
| Targeting_Priority_Set | 0x54 | 0x520 | 0 | |
| Attack_Category_Restrictions | 0x56 | 0x518 | 0 | |
| Fire_Category_Restrictions | 0x2a | 0x3f0 | empty | |
| Guard_Chase_Range / Idle_Chase_Range / Attack_Move_Response_Range | 0x8 | 0x52c / 0x530 / 0x538 | 0.0 | |
| Projectile_Types | 0x1e | 0xc38 | empty | |
| Projectile_Fire_Recharge_Seconds | 0x8 | 0x4e8 | 0.0 | |
| Projectile_Fire_Pulse_Count / _Delay_Seconds | 0x5 / 0x8 | 0x4f0 / 0x4ec | 1 / 1.0 | |
| Projectile_Damage | 0x8 | 0x474 | 0.0 | projectile types |
| Projectile_Max_Flight_Distance | 0x8 | 0x4bc | 50.0 | |
| Projectile_Category | 0x4a | 0x1fe8 | −1 | |
| Projectile_Does_Shield_Damage / _Hitpoint_Damage / _Energy_Damage | 0x0 | 0x45 / 0x47 / 0x46 | true / true / false | |
| Death_Clone | 0x3b | 0xd58 | empty | |
| Death_Explosions | 0x1e | 0xcd8 | empty | |
| Remove_Upon_Death | 0x0 | 0x9b | false | |
| Is_Escort | 0x0 | 0xb8 | false | |
| Collidable_By_Projectile_Living / _Dead | 0x0 | 0x41 / 0x42 | false | |
| Abilities / Unit_Abilities_Data | 0x38 / 0x39 | 0x1a98 / 0x1ab0 | empty | sub-object lists |

**Movement**
| tag | code | offset | default | notes |
|---|---|---|---|---|
| Max_Speed | 0x8 | 0x37c | 0.25 | squadron: min of members |
| Alternate_Max_Speed | 0x8 | 0x380 | 1.0 | read only by the airborne JetPack (MOVEMENT §3.1.1) |
| Max_Thrust | 0x8 | 0x384 | 0.015 | |
| Max_Rate_Of_Turn | 0x8 | 0x38c | 2.3 | squadron: min of members (scaled) |
| Alternate_Max_Rate_Of_Turn | 0x8 | 0x390 | 2.0 | ctor 0x367250; read only by the airborne JetPack (MOVEMENT §3.1.1) |
| Max_Rate_Of_Roll / Bank_Turn_Angle | 0x8 | 0x394 / 0x39c | 2.0 / 70.0 | |
| Mass | 0x8 | 0x398 | 0.995 | |
| Max_Lift | 0x8 | 0x3a0 | 0.8 | |
| Stopping_Distance | 0x8 | 0x3a4 | 2.0 | |
| Min_Speed / Min_Speed_Fraction_For_Turn | 0x8 | 0x3b4 / 0x3ac | 0.0 | squadron: both set to max of members' scaled Min_Speed |
| OverrideAcceleration / OverrideDeceleration | 0x8 | 0x3c0 / 0x3c4 | 0.0 | |
| Movement_Animation_Speed / Walk_ / Crouch_ | 0x8 | 0x370 / 0x374 / 0x378 | 0.25 | |
| Hyperspace / Hyperspace_Speed | 0x0 / 0x8 | 0x24 / 0x648 | false / 0.0 | squadron: speed = min of members, flag = last member's |
| FormationOrder / FormationSpacing / Formation_Priority | 0x5 / 0x8 / 0x6 | 0x16a0 / 0x16d4 / 0x654 | 0 | |
| Custom_Hard_XExtent / _YExtent / Custom_Soft_Footprint_Radius | 0x8 | 0x16bc / 0x16c0 / 0x16b8 | 0.0 | |
| Layer_Z_Adjust | 0x8 | 0xdfc | 0.0 | |

**Vision, misc**
| tag | code | offset | default | notes |
|---|---|---|---|---|
| Land_FOW_Reveal_Range / Space_FOW_Reveal_Range | 0x8 | 0x1d68 / 0x1d6c | 0.0 | |
| Dense_FOW_Reveal_Range_Multiplier | 0x9 | 0x1d70 | 0.5 | clamped [0,1] |
| Political_Control | 0x6 | 0x79c | 0 | planets |
| Planet_Credit_Value | 0x6 | 0x7c8 | 0 | planets |
| Galactic_Position | 0x10 | 0xdc0 | (0,0,0) | planets |
| Encyclopedia_Text / Encyclopedia_Unit_Class | 0x2a / 0x17 | 0x1880 / 0x18b0 | empty | UI |
| SFXEvent_* | 0x1f | 0xf90–0x1610 | — | event names (e.g. `SFXEvent_Select` 0xf90, `_Fire` 0x1240, `Death_SFXEvent_Start_Die` 0x12d0) |

---

## 6. Validation and post-load passes

### 6.1 After every parse of a type (0x3782a0) [H]
- Name CRC (+0x11c, +0xec) from the upper-cased name.
- `Text_ID` and `Build_Countdown_Text_ID` resolved to localized strings.
- `Max_Speed` and `Max_Thrust` multiplied by 1.0 (no-op).
- `Armor_Type`, `Shield_Armor_Type` and `Damage_Type` CRC'd and registered with the damage/armor table
  (0x140b15800).
- **Build-level normalisation.** It applies when `Base_Level` = 0, `Build_Cost_Credits` > 0 or = −1, the
  type is not `Is_Named_Hero`, and its generic `Behavior` list lacks `DUMMY_GROUND_STRUCTURE`
  (case-insensitive). Then:
  - if both `Required_Ground_Base_Level` and `Required_Star_Base_Level` are 0, set
    `Required_Ground_Base_Level` = 1;
  - if both are non-zero, set `Required_Star_Base_Level` = 0;
  - if only the star-base level is set, leave it.
- Lua script (re)load if `Execute_Script_On_Type`.
- `Blob_Shadow_Material_Name` resolved to an id.
- `Priority_Attack_HardPoints` names converted to hardpoint-type ids (unknown names dropped).
- `Radar_Icon_Name` / `Radar_Range_Icon_Name` resolved to texture rectangles normalised by the atlas size.

### 6.2 After every pass, on every type (0x377500) [H]
1. `Weather_Category` −1 → 0.
2. If `Build_Cost_Credits` < 0, it becomes the sum of the members' costs: the first
   `Company_Transport_Unit` entry plus every `Company_Units` entry plus every `Squadron_Units` entry.
   Duplicates count each time.
3. **Company** (`Company_Units` non-empty):
   - `CategoryMask`, `Property_Flags` and `Garrison_Category` are **replaced** by the OR of the members'
     masks;
   - `AI_Combat_Power` = sum over members of their combat power;
   - nested members may be clamped by `Max_Squad_Size`; the exact rule (per member, applied at load) is in
     §9.5 [H];
   - each member's byte +0x2a is set. The field table names that offset `Is_2D`; the code uses it as an
     "is a company member" mark [M].
4. **Squadron** (`Squadron_Units` non-empty):
   - `CategoryMask` and `Property_Flags` are replaced by the members' OR;
   - `AI_Combat_Power` is summed;
   - `Hyperspace_Speed` = minimum of the members; `Max_Speed` = minimum of the members;
   - `Max_Rate_Of_Turn` = minimum of the members' turn rate × a per-mode speed multiplier (1.0 at load,
     §9.5). The multiplier is skipped when the member's generic behaviour set has id 8 = PROJECTILE [H];
   - `Min_Speed` and `Min_Speed_Fraction_For_Turn` are both set to the maximum of the members' scaled
     `Min_Speed`;
   - `Hyperspace` = last member's;
   - each member's byte +0x29 is set (table name `Is_3D`) [M].

   Members not yet resolved by name contribute nothing in that pass. The derivation reruns every pass,
   so the final pass wins.
5. `Affiliation` names → faction pointers (unknown factions dropped). If the list is empty, one default
   faction is appended, both by name and by pointer [M].
6. `HardPoints` names → HardPoint data pointers (unknown names dropped silently). If the generic
   `Behavior` list does **not** contain `WEAPON`, `Targeting_Min_Attack_Distance` = max(itself, every
   hardpoint's `Fire_Min_Range_Distance`).
7. Lua `Base_Definitions` is called on the type script if present.
8. Unless `Cinematic_Object_Only`: the six `Starting_Spawned_Units_Tech_*` slot lists are linked to their
   types (0x370230).
9. `SFXEvent_Attack_Override` entries resolved to (type, SFX event) pairs (both must exist).
10. Two further string fields (+0x1d98, +0x1dc0) are CRC'd (+0x1db8, +0x1de0).
11. The selection and highlight blob materials are resolved to ids.
12. `Space_Override_Population_Value` / `Land_Override_Population_Value` are forced to 0 if the space/land
    behaviour set contains id 44 = TACTICAL_SUPER_WEAPON [H] (§9.4).

### 6.3 After all passes [H]
- The lazy-reference registries and the Lua script bookkeeping lists are freed.
- **Unresolved variants, missing type references, unknown hardpoints, factions and behaviours produce no
  error and no load failure.** The only load-failure path is a list file that cannot be read.
- Errors that are logged:
  - a missing listed file;
  - XML syntax errors (file skipped from the first error on) [M];
  - `TBD` values;
  - enum conversion failures;
  - out-of-range [0,1] values;
  - unprocessed sub-object lists.

### 6.4 Hot reload (dev only) [M]
When a file's timestamp changes, it is re-read with the reload flag (0x409190):
- each element is looked up by name, and its content hash is compared with +0x163c;
- a changed type is destroyed and re-constructed **in place** (same pointer, same index), then re-resolved,
  and live objects of that type are notified.

Not needed for the port beyond keeping type pointers stable.

---

## 7. Port checklist
1. One registry: a vector of types in creation order, plus a case-insensitive name map with first-wins
   insertion.
2. Parse into "raw tag lists per element" first; apply them with the converter rules of §4.5 and the
   repeat semantics of §4.4.
3. Resolve variants parent-first. The child is a deep copy of the resolved parent, with the child's
   identity kept, then the child tags are applied in **variant mode** (§3.3).
4. Resolve the lazy references and run the derivations of §6.2 after the variants. Do them once in
   dependency order (members before companies) rather than in 10 global passes. The result is identical
   for acyclic data.
5. Keep sentinel defaults (−1 limits, −1 enums, 100 HP, 0.25 speed); game logic depends on them.
6. Determinism: the type index (creation order) is the stable id. Never iterate the name map by pointer.
   CRC32 is the engine's (0x215a30) over the upper-cased name.
7. Tests:
   - FoC vanilla must give 3345 types and 911 resolved variants with no warnings.
   - EaW vanilla must resolve its 7 forward variants.
   - Synthetic cases: a cycle, a missing parent, a repeated `HardPoints` in a variant (last wins), a
     repeated `Squadron_Units` in a variant (all kept), and an empty-tag no-op.

## Open items
- ~~The auto-add predicates and the id → name map of the behaviour ids~~ → answered in §9.3/§9.4.
- ~~The token syntax and case rules of the enum/bitmask converters~~ → answered in §9.1.
- Whether any later system skips unresolved types (+0x68 = 0). [L]
- ~~The default faction picked for empty `Affiliation`~~ → answered in §9.2.

---

## 9. Value tables and remaining load rules (2026-10-27)

Answers the S1 implementer's open list (PHASE4.md S1 notes). Read from the decompilation and, for static
tables, the retail exe (FoC v1.121, sha256 07daeeae…) with `toolchain\llvm\bin\llvm-objdump`. Where §9
contradicts §5.3 or §6.2, §9 wins.

### 9.1 Enum and bitmask converters

**Where the converter comes from.** Each descriptor row of the GameObjectType table (0x140a2bf90, 32-byte
rows: tag name, type code, offset, converter slot) has a converter slot. For codes 0xe, 0x54 and 0x56 the slot
points at a global that holds a converter object. All converters are created once by 0x46ea70, called from the
engine init 0x4b250 (the same function that loads factions, hardpoints and object types). [H] for the creator,
[M] for "before the object databases".

**Three converter kinds.** [H]

| kind | value source | used by (GameObjectType) |
|---|---|---|
| dynamic enum (`DynamicEnumConversionClass`) | XML file `Data\XML\Enum\<Name>.xml` | `MovementClass` (MovementClassType), `Weather_Category` (ObjectWeatherCategoryType), `Projectile_Ground_Detonation_SurfaceFX`, `Explosion_Scorch_Mark` (SurfaceFXTriggerType) |
| dynamic bitfield (`DynamicBitfieldConversionClass`) | XML file `Data\XML\Enum\<Name>.xml` | every category mask (GameObjectCategoryType): `CategoryMask`, `Garrison_Category`, `Space_Or_Garrison_Category`, `Attack_Category_Restrictions`, `Special_Weapon_Valid_Targets`, `Planet_Restricted_Unit_Categories`, `Projectile_*_Category_Mask`, `Projectile_Combat_Mod_Category_Mask`; `Property_Flags` (GameObjectPropertiesType) |
| static enum or bitfield (`EnumConversionClass` / `BitfieldConversionClass`) | name/value pairs compiled into the exe | everything else, §9.1.3 |

**String → value rules (all three kinds).** [H] (lookup 0x1dc300 / 0x46e9d0; normalisation 0x200350 + 0x200ad0)
- The value is trimmed of leading/trailing space, tab, CR and LF, then upper-cased (C `toupper`). The entry
  names were normalised the same way when registered. **Lookup is case-insensitive and ignores surrounding
  white space. Inner spaces are significant** (`Large Vehicle` is one name).
- **Enum fields (code 0xe):** the whole value is one name. Unknown name → the converter fails, the loader logs
  `Error: Class data member <%s>: Failed to convert value (%s)!` and **the field keeps its previous value**
  (default, or the parent's in a variant).
- **Bitmask fields (codes 0x54/0x56, and code-0xe fields whose converter is a bitfield):** the value is split on
  `|`, space, comma, tab and LF (consecutive delimiters collapse, so `A | B`, `A|B`, `A, B` and `A B` are the
  same). Each token is trimmed, upper-cased and looked up; the result is the OR of all token values.
  - **One unknown token rejects the whole tag**: the error above is logged and the field keeps its previous
    value. No partial mask is stored. [H] (0x46e680, 0x511300)
  - A value made only of delimiters yields mask 0 and succeeds. [H]
  - The mask is 64 bits for codes 0x54/0x56; code 0xe stores 32 bits.

**Enum XML files.** [H] (dynamic enum 0x46c120; dynamic bitfield 0x46ad50 / 0x46b350)
- Path `.\Data\XML\Enum\` + enum name + `.xml`, through the VFS (mods can override). Each child element of the
  root is one entry: tag = name, text = value.
- Dynamic enum values are read with C `atoi` (decimal). An entry with empty text gets the running entry index.
- Dynamic bitfield values are read with `sscanf("%llx")`: **always hexadecimal**, with or without `0x`
  (`10` means 16). An entry with empty text gets 64 minus its entry index [M]. More than 64 entries logs
  `Error - too many entries for bitfield` and the converter is discarded.
- A duplicate name, or (for dynamic kinds) a duplicate value, logs `ERROR: couldn't add enum entry …` and
  **discards the whole converter**: every later conversion of that enum then fails (fields keep their
  defaults). Vanilla files have no duplicates.
- Comments in the file are ignored (vanilla `GameObjectCategoryType.xml` keeps an old commented-out layout).

#### 9.1.1 GameObjectCategoryType (vanilla FoC `Data\XML\Enum\GameObjectCategoryType.xml`) [H]
`Fighter` 0x1, `Bomber` 0x2, `Transport` 0x4, `Corvette` 0x8, `Frigate` 0x10, `Capital` 0x20, `Super` 0x40,
`Infantry` 0x80, `Vehicle` 0x100, `Air` 0x200, `Structure` 0x400, `AntiFighter` 0x800, `AntiBomber` 0x1000,
`AntiTransport` 0x2000, `AntiCorvette` 0x4000, `AntiFrigate` 0x8000, `AntiCapital` 0x10000, `AntiSuper` 0x20000,
`AntiInfantry` 0x40000, `AntiVehicle` 0x80000, `AntiAir` 0x100000, `AntiStructure` 0x200000, `LandHero` 0x400000,
`Wall` 0x800000, `Upgrade` 0x1000000, `SpaceHero` 0x2000000, `NonCombatHero` 0x4000000,
`Hero` 0x6400000 (= LandHero|SpaceHero|NonCombatHero), `All` 0xFFFFFFFFFFFFFFFF, `None` 0.

#### 9.1.2 Other data-file enums (vanilla FoC) [H]
- **GameObjectPropertiesType** (`Property_Flags`, bitfield): `HealsInfantry` 0x1, `HealsVehicles` 0x2, `Turret` 0x4,
  `TacticalStructure` 0x8, `InBase` 0x10, `OutBase` 0x20, `Fodder` 0x40, `SmallShip` 0x80,
  `NotOpportunityTarget` 0x100, `GoodInfantryCrusher` 0x200, `IsSupercrusher` 0x400, `IsRushTarget` 0x800,
  `CanContainGarrison` 0x1000, `GarrisonCanFire` 0x2000.
- **MovementClassType** (`MovementClass`): `Infantry` 0, `InfantryNoShield` 1, `Wheeled` 2, `WheeledNoShield` 3,
  `Walker` 4, `WalkerNoShield` 5, `Flying` 6, `Tracked` 7, `TrackedNoShield` 8, `Hover` 9, `HoverNoShield` 10,
  `LargeWalker` 11, `LargeWalkerNoShield` 12, `Space` 13, `Lancet` 14, `Spotter` 15, `LargeHover` 16,
  `BombingRun` 17. Default −1 (none).
- **ObjectWeatherCategoryType** (`Weather_Category`): `Default` 0, `Infantry` 1, `LargeWalker` 2, `Speeder` 3,
  `Walker` 4, `Tracked` 5, `Hover` 6, `Wheeled` 7, `Flying` 8. Default −1, forced to 0 after every pass (§6.2).
- **SurfaceFXTriggerType**: 34 entries in `Enum\SurfaceFXTriggerType.xml` (`GENERIC_TRACK` 0 … `INFANTRY_ONLY_GROUND`
  33; 20 is unused). Load it from the file rather than hard-coding.
- `AIGoalCategoryType` and `PerceptionTokenType` are loaded the same way (AI data, not GameObjectType).

#### 9.1.3 Static enums (compiled in) [H]
Names are matched case-insensitively after trimming (rules above). "bitfield" = `|`-combinable.

| field(s) | enum (factory) | names = values |
|---|---|---|
| `Space_Layer`, `Stationary_Space_Layer` | SpaceLayerType, **bitfield** (0x5e0c00) | `None` 0, `Capital` 1, `Frigate` 2, `Corvette` 4, `StaticObject` 8, `Land` 16, `SuperCapital` 32 |
| `UnitCollisionClass` | UnitCollisionClassType (0x5e1110) | `None` −1, `Landing Transport` 0, `Build Pad` 1, `Low Obstacle` 2, `Medium Obstacle` 3, `High Obstacle` 4, `Infantry` 5, `Bike` 6, `Vehicle` 7, `Large Vehicle` 8, `Giant Vehicle` 9 |
| `OccupationStyle` | UnitOccupationType (0x5e1820) | `None` −1, `1x1` 0 (nothing else) |
| `OverridePassability`, `SecondaryOccupationPassability` | CellPassabilityType, **bitfield** (0x5dd090) | `Invalid` −1, `None` 0, `Clear` 1, `Steep` 2, `Obstacle` 4, `RebelWall` 8, `EmpireWall` 16, `Water` 32, `Shield` 64, `InfantryOnly` 128, `Impassable` 256 |
| `ContainerArrangement` | ContainerArrangementType (0x5dd790) | `None` −1, `Rows` 0, `Dartboard` 1 |
| `The_Force` (and Faction `Force_Alignment`) | ForceAlignmentType (0x5ddaa0) | `Light` 0, `Neutral` 1, `Dark` 2 |
| `Formation_Formup_Wait_Style` | FormationFormupWaitType (0x5dddc0) | `WaitForAll` 0, `WaitForFront` 1, `WaitNone` 2 |
| `FormationGrouping` | FormationGroupingType (0x5de0f0) | `Solo` 0, `SameOrder` 1, `Standard` 2 |
| `Can_See_Visibility_Level` | tVisibilityLevelType, **bitfield** (0x510120) | `Planet_Ownership` 0x1, `Starbase_Level` 0x2, `Ground_Base_Level` 0x4, `Credit_Income` 0x8, `Fleet_Presence` 0x10, `Num_Fleets` 0x20, `Fleet_Contents` 0x40, `Num_Ground_Companies` 0x80, `Ground_Company_Contents` 0x100, `Special_Structures` 0x200, `Political_Control` 0x400, `Political_Control_Breakdown` 0x800, `Object_Under_Production` 0x1000, `Enemy_Minor_Stealth_Heroes` 0x2000, `Enemy_Major_Stealth_Heroes` 0x4000, `Credit_Income_Breakdown` 0x8000, `Most_Powerful_Ship` 0x10000, `Force_Sensitive_Heroes` 0x20000, `Super_Weapons` 0x40000, `Evil_Default` 0, `Good_Default` 0x11, `Planet_Owner` 0x9FFF, `Maximum` −1 |
| `Internal_Damage_Type`, `Squash_Damage_Type` (and ability `Damage_Type`, `Bomb_Damage_Type`) | tDamageType (0x5e1ac0) | `Damage_Debug_Chear` 0 (sic), `Damage_Normal` 1, `Damage_Force_Whirlwind` 2, `Damage_Force_Telekinesis` 3, `Damage_Force_Lightning` 4, `Damage_Force_Corruption` 5, `Damage_Hard_Point_Self_Destruct` 6, `Damage_Fire` 7, `Damage_Cable_Attack` 8, `Damage_Explosion` 9, `Damage_Asteroid` 10, `Damage_Cable_Attack_Deployed` 11, `Damage_Normal_Deployed` 12, `Damage_Vehicle_Thief` 13, `Damage_Crush` 14, `Damage_Eat` 15, `Damage_Redirected` 16, `Damage_Wampa` 18, `Damage_Infection` 20, `Damage_Remote_Bomb` 21, `Damage_Drain_Life` 22 (17 and 19 have no name) |
| `Tactical_Production_Queue` | ProductionQueueType (0x5e26b0) | `Land` 0, `Space` 1, `Tactical_Units` 2, `Tactical_Upgrades` 3. Default −1 |
| `Is_Sellable` | SellableTypeEnum (0x370850) | `Sellable_Default` 0, `Sellable_Always` 1, `Sellable_Never` 2 |
| `Corruption_N_Type`, `Corruption_N_Required_Mission_Type` | CorruptionTypeEnum (0x5e0590) | `Corruption_None` −1, `Corruption_Intimidation` 0, `Corruption_Piracy` 1, `Corruption_Kidnapping` 2, `Corruption_Racketeering` 3, `Corruption_Corrupt_Militia` 4, `Corruption_Bribery` 5, `Corruption_Black_Market` 6, `Corruption_Bonded_Citizens` 7 |
| `Corruption_N_Tactical_Game_Mode` (and AI `GameMode`) | tSubGameModeType (0x5de710) | `Galactic` 0, `Land` 1, `Space` 2 |
| `Specific_Death_Anim_Type`, `Shield_On_Anim`, `Shield_Off_Anim`, all `*_Anim_Type` | ModelAnimType (0x279010) | the model animation-type list (large; not reproduced here) |
| `Targeting_Priority_Set` (code 0x54) | TargetingPrioritySetClass (0x483ae0) | the name of a set from `TargetingPrioritySetFiles.xml`, not a fixed enum [M] |

Two codes do not use a converter object: [H]
- **`Ship_Class` (code 0x32)**: compared with `_stricmp` against a table at 0x140a43590: `fighter` 0, `bomber` 1,
  `transport` 2, `corvette` 3, `frigate` 4, `capital` 5, `super` 6, `infantry` 0, `tank` 4, `artillery` 2,
  `walker` 5. **Unknown names leave the field unchanged with no log.** Default −1.
- **`Projectile_Category` (code 0x4a)**: upper-cased, then compared exactly (0x469280): `DEFAULT` 0, `MISSILE` 1,
  `BOMB` 2, `GRENADE` 3, `ROCKET` 4, `STATIC_BOMB` 5, `MPTL_ROCKET` 6, `LASER` 7, `LIGHT_SABER` 8,
  `CLUSTER_BOMB` 9; anything else stores −1.

#### 9.1.4 Most used sub-object enums [H]
- Ability `Activation_Style` (AbilityActivationType, 0x5e42e0): `Global_Automatic` 1, `Combat_Automatic` 2,
  `Galactic_Automatic` 3, `Space_Automatic` 4, `Ground_Automatic` 5, `Ground_Activated` 6, `Hero_Detected` 7,
  `Combat_Imminent` 8, `Special_Attack` 9, `Take_Damage` 10, `User_Input` 11, `Skirmish_Automatic` 12.
- Ability `Unit_Strength_Category`, `Applicable_Unit_Categories`, `Excluded_Unit_Categories` and AI
  `Category_Exclusions`, projectile `Infection_CategoryMask`: GameObjectCategoryType bitfield (§9.1.1).
  AI `Property_Exclusions`: GameObjectPropertiesType.
- Unit ability type names (UnitAbilityType, 0x5dea20): `DEFEND` 1, `DEPLOY` 2, `INTERDICT` 3, `HARASS` 4,
  `BARRAGE` 5, `HUNT` 6, `SPREAD_OUT` 7, `AFTERBURNER` 8, `TURBO` 9, `LURE` 10, `ROCKET_ATTACK` 11,
  `AVOID_DANGER` 12, `FORCE_TELEKINESIS` 13, `FORCE_WHIRLWIND` 14, `FORCE_LIGHTNING` 15, `FLAME_THROWER` 16,
  `JET_PACK` 17, `CAPTURE_VEHICLE` 18, `TARGETED_INVULNERABILITY` 19, `TARGETED_HACK` 20, `TARGETED_REPAIR` 21,
  `STICKY_BOMB` 22, `AREA_EFFECT_STUN` 23, `AREA_EFFECT_HEAL` 24, `AREA_EFFECT_CONVERT` 25, `DISTRACT` 26,
  `CABLE_ATTACK` 27, `FOW_REVEAL_PING` 28, `POWER_TO_WEAPONS` 29, `TRACTOR_BEAM` 30, `ENERGY_WEAPON` 31,
  `MISSILE_SHIELD` 32, `INVULNERABILITY` 33, `LUCKY_SHOT` 34, `HARMONIC_BOMB` 35, `UNTARGETED_STICKY_BOMB` 36,
  `CONCENTRATE_FIRE` 37, `DEPLOY_TROOPERS` 38, `SELF_DESTRUCT` 39, `SPRINT` 40, `ION_CANNON_SHOT` 41,
  `WEAKEN_ENEMY` 42, `SPOILER_LOCK` 43, `REPLENISH_WINGMEN` 44, `MAXIMUM_FIREPOWER` 45, `EJECT_VEHICLE_THIEF` 46,
  `FIRE_LOBBING_SUPERWEAPON` 47, `STEALTH` 48, `STUN` 49, `RADIOACTIVE_CONTAMINATE` 50, `BERSERKER` 51,
  `FORCE_SIGHT` 52, `SABER_THROW` 53, `FORCE_CLOAK` 54, `SUPER_LASER` 55, `LASER_DEFENSE` 56, `FORCE_CONFUSE` 57,
  `LEECH_SHIELDS` 58, `TACTICAL_BRIBE` 59, `SWAP_WEAPONS` 60, `CLUSTER_BOMB` 61, `FULL_SALVO` 62,
  `SENSOR_JAMMING` 63, `PLACE_REMOTE_BOMB` 64, `DETONATE_REMOTE_BOMB` 65, `INFECTION` 66, `PROXIMITY_MINES` 67,
  `STIM_PACK` 68, `DRAIN_LIFE` 69, `BLAST` 70, `BUZZ_DROIDS` 71, `SHIELD_FLARE` 72, `SUMMON` 73,
  `CORRUPT_SYSTEMS` 74, `DEPLOY_SQUAD` 75.
- Ability `Owner_Light_Effect_Type` / `Target_Light_Effect_Type` (LightEffectType, 0x5e4a80), action
  `Move_Type` (MoveActionTypeEnum), `Action_Relevance_Type`: static enums, not decoded here.

### 9.2 `Affiliation` and the default faction [H]
- **Parse (code 0x1b, 0x51a280):** in a variant the list is cleared once per occurrence (§3.3). The value is
  tokenised on space, comma, tab and LF; each token is appended **as written** (case kept). Values of 8192
  characters or more are ignored. Duplicates are kept.
- **Resolve (0x377500, after every pass):** the faction-pointer list (+0xef0) is cleared and rebuilt: each name is
  looked up with `_stricmp` in faction-list order (0x3303d0); unknown names are dropped silently.
- **Empty list:** the default faction is **the first faction in faction-list order whose `Is_Neutral` (faction
  +0x108) is true** (0x331cc0). Its name (as stored, upper-cased) is appended to `Affiliation` and its pointer to
  the faction list. If no faction is neutral, the list stays empty. In vanilla the only neutral faction in
  `Factions.xml` is **`Neutral`** (`Expansion_Factions.xml` is not in the text extract; check it when available).
- Because the name is written back, a type that was empty after pass 0 resolves by name on later passes.
- **Faction list:** `FactionFiles.xml` names the files (vanilla FoC: `Factions.xml`, `Expansion_Factions.xml`);
  each `<Faction Name=…>` is appended in file order with an upper-cased name, **no duplicate check**. Lookups are
  linear, so **the first definition of a faction name wins.** (0x331d70)

### 9.3 Implicit behaviours and the behaviour bitsets

**Registry (0x140a2ac90).** 116 rows of 32 bytes, then a null row: (name, factory, type-id predicate,
auto-add predicate). Every type-id predicate is a single equality test, so each name maps to exactly one id; the
ids are the ones in OBJECT_RUNTIME.md §4.3. [H]

**Only one implicit behaviour exists: `STARSHIP_COMBATANT`** (row 20, auto-add predicate 0x638b40). No other
row has an auto-add predicate. [H]
- **When:** at the end of every top-level parse (the first parse and every variant re-parse), 0x4c33d0 appends the
  name `STARSHIP_COMBATANT` to the generic `Behavior` list (+0x6d0) when the predicate holds. **No duplicate check.**
- **Predicate:** all of
  - `Is_Decoration` is false;
  - the type's generic behaviour bitset does **not** have id 8 (PROJECTILE) or id 0x0C (PARTICLE);
  - `Damage` > 0 or `Tactical_Health` > 0 or `Shield_Points` > 0 or `AI_Combat_Power` > 0.
- **Quirk: the bitset tested is the one from before this parse.** The append runs immediately before the bitsets
  are rebuilt (0x374dc0, its only caller). The constructor allocates the bitsets empty (0x367250), so on a
  type's **first parse the PROJECTILE/PARTICLE test never fires.** Since `Tactical_Health` defaults to 100,
  **almost every non-decoration type gets `STARSHIP_COMBATANT`, projectiles and particles included.** [H]
- In a variant the test uses the bitset copied from the parent:
  - child with its own `Behavior` tag: list replaced, then `STARSHIP_COMBATANT` appended once, unless the parent
    was a projectile or particle;
  - child without a `Behavior` tag: the inherited list already holds the parent's `STARSHIP_COMBATANT`, and a
    second copy is appended (unless the parent was a projectile or particle). **The list then names it twice.**
- At runtime `STARSHIP_COMBATANT` builds a `CombatantBehaviorClass`, which is **not serviced** (§9.6). It sits in the
  generic list, so it is attached in every mode.

**Bitsets (0x374dc0, end of every top-level parse).** [H]
- Five bitsets of 0x6a bits: Galactic (data +0x750, from `GalacticBehavior`), Land (+0x760, `LandBehavior`),
  Space (+0x770, `SpaceBehavior`), Union (+0x780), Generic (+0x790, `Behavior`). (The bitset objects start 8 bytes
  earlier; the offsets are the data pointers.)
- All five are cleared, then for each name of each list: the first registry row whose name matches with
  `_stricmp` gives the id; the bit is set in that list's set and in Union. Unknown names set nothing.
  `DeployedBehavior` is not included.
- They are rebuilt only here. In a variant they are first copied from the parent, then rebuilt from the child's
  final lists.

### 9.4 Behaviour ids 8 and 44 and the population override [H]
- Bit numbers in the bitsets are the runtime type ids. **Id 8 = `PROJECTILE`. Id 44 (decimal, 0x2C) =
  `TACTICAL_SUPER_WEAPON`.** (Id 0x44 is `DEATH`, not involved.)
- **Population override (last step of 0x377500, every pass):**
  - if the **Space** bitset (from `SpaceBehavior` only) has `TACTICAL_SUPER_WEAPON` and
    `Space_Override_Population_Value` ≠ 0, it is set to 0;
  - if the **Land** bitset (from `LandBehavior` only) has it and `Land_Override_Population_Value` ≠ 0, it is set
    to 0.
  - The −1 default is therefore also forced to 0. Listing it under the generic `Behavior` has no effect here.
- **Id 8 in squadrons:** a member whose generic bitset has `PROJECTILE` skips the speed multiplier of §9.5.
- Also in that last step: if `Place_Other_Type_At_Every_Specific_Marker_Position` (+0x22e8) resolves to a type
  (+0x22f0), that type's byte +0xE3 is set to 1. This is the flag OBJECT_RUNTIME §2.5 tests for death marking. [H]

### 9.5 "Difficulty factor" and company power clamp at load [H]
Both depend on the **current mode type**: the active mode's vtable +0xE0, or GameModeManager +0x1C
(0x140b153fc) when no mode is active. **The type database is loaded at engine init, before any mode exists,
and GameModeManager's constructor sets +0x18/+0x1C to −1, so the mode type at load is −1.**

- **Squadron speed factor (not a difficulty factor):** factor = GameConstants
  `Object_Max_Speed_Multiplier_Galactic` (mode 0, 0x140b16dbc), `_Land` (mode 1, 0x140b16dc4) or `_Space` (mode 2,
  0x140b16dc0), times a constant 1.0 (0x140a28568). Any other mode value gives 1.0.
  - It scales each member's `Max_Rate_Of_Turn` and `Min_Speed` before the min/max of §6.2.
  - It is skipped (factor 1.0) for members whose generic bitset has `PROJECTILE`.
  - **At load the mode is −1, so the factor is exactly 1.0.** (It also does not touch `Max_Speed` or
    `Hyperspace_Speed`.)
- **Company/squadron power clamp:** correction to §6.2. The clamp is per member, and only for **nested** members:
  - when the mode type ≠ 0, a member that itself has a non-empty `Company_Units` (else `Squadron_Units`) list,
    whose first entry's type has `Create_Team` (+0x67), contributes
    `AI_Combat_Power × clamp(member.Max_Squad_Size / member's own list count, 0, 1)`;
  - every other member contributes its plain `AI_Combat_Power`.
  - **At load the mode is −1 (≠ 0), so the clamp is applied.**
  - In vanilla FoC, `Create_Team` sits on infantry listed directly in companies (e.g. `Bothan_Slave` in
    `Bothan_Slave_Company`), so most companies see plain sums.
  - The same rule is used for squadron members.
  - The first entry's type pointer is read without a null check; it is always resolved for vanilla data.

### 9.6 Implementer assumptions checked
| assumption | verdict |
|---|---|
| List codes 0x22 and 0x3c clear in variants like other lists | **Wrong.** Neither tests the variant flag. **0x22** (`SFXEvent_Attack_Override`, faction `SFXEvent_*_Control`, …) appends one (first token, second token) pair per occurrence. **0x3c** (`Faction_Anim_Subindex`, `Planet_Max_Political_Control_For_Faction`) appends one (`<name>`, atoi `<int>`) pair per occurrence. A variant's entries are appended to the parent's copy; repeated tags accumulate. [H] |
| Hardpoint names are first-definition-wins | **Wrong: last definition wins.** HardPointDataManager (0x4738b0) keys a map by the CRC of the upper-cased name and assigns with insert-or-overwrite, so a later `<HardPoint Name=…>` with the same name replaces the earlier one (also across files). Lookup 0x471360 is by that CRC. [H] |
| Faction names are first-definition-wins | **Right.** Appended without a check, linear `_stricmp` lookup (§9.2). [H] |
| Every non-DUMMY behaviour is serviced each frame | **Wrong in two ways**, see OBJECT_RUNTIME §9.1: several non-dummy behaviours are never serviced, and many serviced ones have a service interval other than 1. [H] |
| A behaviour id is attached at most once per object | **Wrong.** The attach loop (0x38cb30) creates one behaviour per list name with no duplicate check (OBJECT_RUNTIME §9.2). Duplicates occur in vanilla through §9.3. [H] |
| Cascaded kills are flushed in the same flush | **Right.** OBJECT_RUNTIME §9.3. [H] |

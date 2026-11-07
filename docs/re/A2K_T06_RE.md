# A2k t06 (RE half): `Function_<Name>` binding and the converter-name compare (2026-11-06)

Answers to RE questions 2 and 3 of `A2J_T07_LITERALS.md`. StarWarsG.exe FoC x64 and `corruption\PerceptionFunctionG.dll`
(Steam), RVAs. Tags: [H] read from code, [M] one inferred link, [L] unverified. No decompiled code is reproduced.

## 1. `Function_<Name>` references (RE question 2)

### 1.1 What a `Function_X` is

`Function_X` is not special syntax in the DLL. It is an ordinary **perception token**: the DLL's token node (node kind 7,
0x17e40) converts the token text with the exe's `DynamicEnumConversionClass<PerceptionTokenType>` (Init argument 1, DLL global
0x1800fb430, virtual slot 4 = exe 0x1dc270: trim 0x200350, upper-case 0x200ad0, exact lookup) [H]. A miss logs "Invalid
perceptual token (%s) while trying to parse an equation!", shows the one-shot "Perception Parsing Error!" MessageBox and fails
the build, so the equation is dropped as in `A2I_TOKENS_RE.md` §3.2 [H].

### 1.2 Token registration happens before any equation is parsed [H]

AI data init 0x4753b0 runs, in this order: `Init_Perception_DLL`, **0x4de3c0**, other loaders, `System_Initialize` of the
equation manager (DLL parse of every equation file), the goal-function loader 0x5e6190, ..., **0x4dd050**.

0x4de3c0 appends entries to the PerceptionTokenType enum (each new value = current maximum + 1):
1. for every game object type (type manager 0x140b2f020, in its index order): the type name; and `Planet_<name>` when 0x374c80(type, "PLANET")
   is true [M: a PLANET behaviour test];
2. for every `*.XML` in `.\Data\XML\AI\PerceptualEquations\` (exe file finder on the file manager 0x140a7d080, so the VFS) and
   every root child of that file: `Function_` + the child's element name. Names are trimmed and upper-cased when inserted.
   A file the XML reader cannot open is skipped. The text of the equation is **not** looked at: an equation that will fail
   to parse still gets its token.
3. A duplicate name logs "ERROR: enum entry (%s) already exists!" and is not added; 0x4de3c0 then returns false, which only
   makes 0x4753b0 log "Error: Unable to initialize TheAIDataManager." (no abort) [M: 0x2149d0 is a log].

So at parse time every `Function_<Name>` of every equation file is already a valid token: **the file order and the
definition order do not matter**, and a reference to an equation that is defined nowhere fails the build (the token is
unknown).

### 1.3 Binding to the equation: after all files, by 0x4dd050 [H]

0x4dd050 clears the token -> evaluator map (0x140b31910), then walks the equation manager (only equations that built
successfully are in it). For each: converts `Function_<Name>` (exported `Get_Perception_Token_String`, DLL 0x17b00) to its
token id, wraps the equation in a `FunctionPerceptualEvaluatorClass` (0x64f370, equation at +0x88) and registers token id ->
evaluator (0x4db800; an id already present is not replaced and makes the init result false).

A dropped equation is not in the manager, so its token stays registered in the enum with **no evaluator**.

### 1.4 Evaluating a reference to a dropped equation [H]

- DLL token node evaluate (0x17770): calls the exe evaluate callback (Init argument 3 = exe 0x4dd340 -> 0x4dd1d0). On false
  it returns false at once; no message.
- 0x4dd1d0: looks the token up in the context's own evaluators (0x5b4300), then the per-context map, then the global map
  0x140b31910. Not found: returns **false**. Found: vf +8 of the evaluator; for a function evaluator 0x64f3e0 that is
  `PerceptionFunctionClass::Evaluate` of the bound equation (false when its pointer is null).
- So the referencing equation **builds and is registered**, and each evaluation that reaches the reference fails (false),
  silently. What a false does to the enclosing expression is the DLL's general failing-leaf rule (`AI_GALACTIC_3.md` §7.1)
  [M: not re-read here]; callers that report it log e.g. "Evaluate_Perception: Error evaluating perception function %s.".
- Names: the equation manager keys are upper-cased without trimming (DLL 0x259e0, used by `Read_Equation_File` and
  `Get_Managed_Object` / `String_To_UInt` 0x18580) [H].

**Port rule check** (`A2J_T07_LITERALS.md` §2): the port's rule (the referrer builds, the reference fails at evaluation) is
the retail behaviour for a reference to an equation that **exists in a file but failed to build**. Two differences:
(a) a reference to a name that is in **no** equation file fails the referrer's build in retail (unknown token); the port
builds it and counts it in `unresolved_functions`; (b) the port's "a later file that defines the name successfully satisfies
the reference" matches retail only when the earlier definition was dropped (a second definition of a name that loaded is
rejected: "already exists in the equation manager"; and its token registration logs a duplicate).

### 1.5 Goal-function sets (`<Function>`) [H unless tagged]

`GoalFunctions` entries are read through a field table (0xa3fab0): `Function` is field type 0x54 at +0x08 with converter
0x140b310f0 = the equation manager itself (`ThePerceptionFunctionManagerPtr`, stored right after `ThePerceptionFunctionManagerClass::Create`), whose `String_To_UInt`
(DLL 0x18580) upper-cases the name and returns false when it is absent. The loader runs after `System_Initialize`, so a
dropped equation is a conversion miss at load. What the type-0x54 field reader and 0x5e6190 do with that false (entry kept with
a null function, entry dropped, or file failed) is [L].

## 2. Converter-name compare in 0x23b030 (RE question 3) [H]

- The DLL (0x16210, enum literal node kind 0xA) builds the converter-name string from the node's first child and passes it
  through 0x254f0 before calling the exe callback (DLL global 0x1800fb470 = exe 0x474c40, a jump to 0x23b030). 0x254f0
  **trims** leading and trailing space, tab, CR and LF. It does not change case. (The bracket text gets the same trim.)
- 0x23b030 looks the string up in the global registry 0x140b09350 with FNV-1a (offset basis 0x84222325, prime 0x1b3 over the
  bytes, then folded through the 0x7fffffff / 0x1f31d step) and an exact length + `memcmp` compare (0x11f9c0 / 0x11f570).
  Miss -> null.
- Registry keys are inserted by the `DatabaseUIntConversionClass` constructor 0x23ae90 with the name string exactly as given
  (e.g. "GameObjectCategoryType"; an existing key is not replaced, the first converter of a name wins).

So the converter name is **case-sensitive and exact after trimming white space**: `gameobjectcategorytype[Infantry]` fails
("There is no enum converter named %s!") and the equation is dropped; ` GameObjectCategoryType [Infantry]` works. The port's
case-insensitive compare (`A2J_T07_LITERALS.md` §3) differs only for such misspelled mod data; shipped data is unaffected.

Port (A2k t12, 2026-11-07): `EquationSet::link` drops an equation whose `Function_` reference names no added
equation (parse error "Invalid perceptual token"), and the converter-name lookup is trimmed and case-sensitive. [port]
the check runs after all files, so a later duplicate of a referrer dropped this way was already rejected as a duplicate.

## 3. Confidence and open questions

| claim | tag |
|---|---|
| `Function_` tokens registered from the file names of all equation files before parsing | H |
| reference to an undefined name: parse error; to a dropped equation: builds, evaluates false | H |
| what false does to the enclosing expression | M (DLL failing-leaf rule) |
| converter name: trimmed, case-sensitive exact compare | H |
| goal-function `<Function>` naming a dropped equation: load-time miss; loader reaction | H miss, L reaction |

Open: §1.5 loader reaction (settles by reading the field-type 0x54 reader and 0x5e6190's entry handling, or a probe mod with
a goal-function set that names a deliberately broken equation and the AI log).
</content>
</invoke>

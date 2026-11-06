# Mod-loading contract

What makes a mod loadable by the port, how its files are found, which data mechanisms it may use, what the loader reports,
and the rule that `engine/` has no mod-specific code. Written for roadmap group `modc` (task t01, 2026-11-06); the binding
decision is in [`DECISIONS.md`](DECISIONS.md) ("Mod compatibility is agnostic", Gabriel, 2026-11-03). Status of each part is
marked here so later lanes can point at it; whether a task is finished is on the roadmap only.

Status labels used below:

| Label | Meaning |
|---|---|
| **main** | Implemented on `main` today |
| **Q6 stack** | Implemented on the unmerged Q6 branches (`claude/initial-mod-compatibility-cdd6af`, `claude/q6-integration`; PRs #65, #67, #69); on `main` once that stack lands |
| **modc tN** | Planned by that `modc` task |

## 1. The rule

A mod works in the port when it uses the same data mechanisms as base Forces of Corruption. Thrawn's Revenge (roadmap Q6)
is the first and hardest test case, but it is a test case, not a target the engine is shaped around.

1. **No mod-specific code paths in `engine/`.** Engine code (every module, sim and presentation alike) contains no mod name,
   no workshop id, no faction, unit, planet, campaign or map name that changes behaviour, and no branch of the form "if this
   is mod X". Behaviour comes from the data files through the mechanisms in §4.
2. **No fixed faction lists or player counts.** Factions come from `FactionFiles.xml` in file order; players come from the
   campaign, the skirmish setup or the map. The only fixed limits are those the original executable has, cited from a
   `docs/re/` spec (for example the skirmish slot rules in `docs/re/SKIRMISH_SETUP.md` §1).
3. **Names the original itself hard-codes are allowed**, with a comment that cites the `docs/re/` spec and address: the
   original treats a mod the same way, so the port stays agnostic. Example: a GameConstants field, a Lua global, an engine
   effect file name. A base-game *content* name (`Empire`, `Rebel`, `Underworld`, `TIE_Fighter`, `Kuat`, ...) is never in this
   class unless the spec shows the executable looks it up.
4. **A port rule may depend on a mechanism, never on an identity.** Allowed conditions: "a mod layer is loaded", "the faction
   has no `Space_Skirmish_AI_Default_Forces`", "the type has no `Hyperspace`". Not allowed: "the faction is called Empire",
   "the mod folder is 1125571106". Each port rule that applies only when a mod layer is loaded is listed in §6, and with no
   mod loaded the port behaves exactly as base FoC (Gabriel, 2026-11-04, `skirmish_buy_list` `multiplayer_list_only`).
5. **Base FoC does not move.** Loader, Lua and mod fixes must leave base-FoC behaviour unchanged: the base-FoC CRC goldens
   stay byte-identical (DECISIONS.md, "Parity tail re-sequenced", 2026-10-31). Determinism rule 3 applies to mod data
   exactly as to base data: the same mod and command line give the same per-tick CRC on every thread count and compiler.
6. **Fixes are general.** A gap a mod exposes is fixed as a general mechanism (a tag the loader did not read, a Lua function
   not bound, a fallback the original has). A fix that only makes sense for one mod is not made; the gap is reported (§5).

Allowed places for mod and faction names: tests and their fixtures (`engine/*/tests/`, `tests/`), docs, `tools/probe_mod/`,
`tools/testbuild/` launchers, and comments that cite where a behaviour was observed. The `modc` t04 lint enforces rule 1 on
`engine/` sim code with an allow-list for these.

## 2. What a mod is

A mod is a folder that contains a `Data\` tree laid out like FoC's own `corruption\Data\`. Anything under that `Data\`
replaces or adds to the game's files by path (§3). A mod needs nothing else: no manifest, no registration and no
port-specific file. A folder with no `Data\` subfolder mounts nothing; the VFS warns
`mod path has no Data folder: '<dir>'` (**main**, `engine/vfs/src/vfs.cpp`).

A mod is played on top of an installed FoC (the "corruption" folder), which in turn needs EaW (`GameData`) next to it, as
in retail. The port reads only; it never writes into a mod folder.

## 3. Finding the mod and resolving files

### 3.1 Command line

The port takes the original's switches (`docs/re/PHASE2_ANSWERS.md` §14, `docs/re/MAIN_LOOP.md` startup step 3). Keys are
case-insensitive and values may be quoted. `alamo_game`, `alamo_headless` and `alamo_mod_check` pass every `KEY=value`
argument to `vfs::parse_command_line`.

| Switch | Meaning | Status |
|---|---|---|
| `MODPATH=<dir>` | Adds `<dir>` to the mod search list. A relative `<dir>` is resolved against the FoC folder, so the retail convention `MODPATH=Mods\<Name>` finds `corruption\Mods\<Name>`. An absolute path works too, including a Steam workshop item folder | **main** |
| `STEAMMOD=<id>` | Adds the Steam workshop item `<id>` to the same list, at its command-line position. Retail asks the Steam client for the subscribed item's folder; if the item is not subscribed, nothing is mounted | **main** (modc t10): `alamo_game`, `alamo_headless` and `alamo_mod_check` install the Steam library lookup (`engine/vfs/include/alamo/vfs/steam_workshop.hpp`, below); an item not found warns `STEAMMOD=<id>: workshop item not found; nothing mounted`. A `Vfs` created by other code (tests, tools) without a `VfsConfig::steam_resolver` still warns `no workshop resolver`. `MODPATH=<item folder>` keeps working |
| `FALLBACKPATH=<dir>` | Adds `<dir>` to the fallback chain (below the MEGs, above EaW `GameData`) | **main** |

`MODPATH=` and `STEAMMOD=` entries go into **one** search list in command-line order: retail mounts each as it parses the
token, so they interleave (PHASE2_ANSWERS §14). The earliest entry wins. Several mods may be stacked this way; the port
supports it exactly as the original does, and a "submod" is just an earlier entry.

Where mods are found, for tools that list them (**modc t08** mod picker):

- local: `<FoC folder>\Mods\<Name>\` (each subfolder with a `Data\` folder is a candidate);
- Steam workshop: `<library>\steamapps\workshop\content\32470\<id>\` for every Steam library folder (32470 is the FoC app
  id). The port does not use the Steam API: an item folder present on disk counts as installed (**main**, modc t10).
  Library folders, first match wins: the library holding the FoC folder (the parent of its nearest `steamapps` ancestor),
  then for each Steam client folder the folder itself and every `path` of its `steamapps\libraryfolders.vdf` and
  `config\libraryfolders.vdf` (file order; the pre-2021 numeric-key form too). Client folders: `ALAMO_STEAM_ROOT`
  (`;`-separated; when set, nothing else), else the registry's `SteamPath` / `InstallPath` (read only), else
  `C:\Program Files (x86)\Steam`. An id that is not all digits is never found.

### 3.2 File resolution order

Unchanged from Phase 2 (`docs/PHASE2.md` "Data\ resolution order", `docs/re/SUBSYSTEMS.md` "File lookup priority",
`engine/vfs/include/alamo/vfs/vfs.hpp`). First match wins:

1. **Mod search list**, earliest entry first: `<mod>\Data\<rest>`. Only paths starting with `Data\`.
2. **FoC loose** `corruption\Data\<rest>`.
3. **MEG archives**, newest-mounted first: FoC `64Patch` > `patch2` > `Patch` > FoC `MegaFiles.xml` entries (later-listed
   wins) > EaW `64Patch` > `patch2` > `Patch` > EaW `MegaFiles.xml` entries.
4. **Fallback chain**: `FALLBACKPATH=` folders in order, then EaW `GameData\Data`.

Consequences for mods (all **main**):

- **File granularity.** A mod replaces whole files, never parts of them. Shipping `Data\XML\SpaceUnitsCapital.xml` replaces
  that file; shipping `Data\XML\GameObjectFiles.xml` replaces the whole list. Two copies of a file are never merged
  (`docs/re/OBJECT_TYPES.md` §1.3).
- **Loose beats archived.** A mod's loose files beat every MEG, including `64Patch.meg`.
- **Mod archives.** `Data\MegaFiles.xml` and `Data\Patch*.meg` are opened through the search list, so a mod's own
  `MegaFiles.xml` replaces FoC's list and the MEGs it names are mounted in place of the base ones; a mod MEG with the same
  name as a base MEG (e.g. `Data\Config.meg`) is mounted *instead* of it. EaW-side archives always come from EaW.
- **Paths** are ASCII case-insensitive and `/` equals `\`.
- **Directory scans are not lookups.** Listings (skirmish maps, AI plan folders, scripts) follow the rules of the spec that
  defines them (`docs/re/UI.md` §9.2: mod-folder maps go to the *custom* map list; `PHASE2.md` note on `Vfs::list(glob, layers)`).

### 3.3 What a save remembers

A save stores the VFS command line it was made with (`SessionDescriptor::data_args`) and a fingerprint of the loaded
definitions (`data_fingerprint`). Loading a save rebuilds the session from those arguments; a save made with different data
(another mod, another mod version, no mod) is refused with the first difference (`SessionDescriptor::mismatch`,
`engine/apps/sim/include/alamo/sim/save_session.hpp`; **main**). Saves of the original game are out of scope (DECISIONS.md).

## 4. Mechanisms a mod may use

Everything below is reached through the VFS of §3, so a mod uses it by shipping the file. The port implements these as the
original does (the spec column), for base FoC and for mods alike; nothing here is special to mods.

| Mechanism | Files | Rules (spec) | Status |
|---|---|---|---|
| Faction list | `Data\XML\FactionFiles.xml` and the files it names | Factions appended in file order, upper-cased names, no duplicate check: the first definition of a name wins. The default faction of a type with an empty `Affiliation` is the first faction with `Is_Neutral` (`OBJECT_TYPES.md` "Faction list", "Empty list"). Map object owners are faction indices in this order (`GALAXY_MAP_PRESENTATION.md`, `REMAINING_GAPS.md` S10 probe) | **main** (galactic rules); no fixed list anywhere: **modc t02/t03** |
| AI player type | Faction `Basic_AI`; campaign `AI_Player_Control` pairs; `Data\XML\AI\` definitions | Skirmish: an AI slot gets its faction's `Basic_AI` (`SKIRMISH_SETUP.md` §2.1). Campaign: the campaign's `AI_Player_Control` pair; no pair = no AI. A type the AI definitions do not define = no AI ("Unrecognized AI player type", `AI_GALACTIC.md` §1.2) | `main` uses built-in types by faction name (`ai_type_for`, §7). `resolve_ai_type` / `load_faction_basic_ai`: **Q6 stack** (q6 t08), which still keeps the built-in types for `Empire` / `Rebel` / `Underworld` when their `Basic_AI` matches (`is_stock_faction`). Fully data-driven: **modc t03** |
| Skirmish starting forces | Faction `Space_Skirmish_AI_Default_Forces`, `Land_Skirmish_AI_Default_Forces` | `SKIRMISH_SETUP.md` §2 step 4, §2.2 | `main` (`alamo_game`) uses built-in lists by faction name (§7). Data path: **Q6 stack** (`skirmish_default_forces`). Built-in lists removed: **modc t03** |
| Game object types | `Data\XML\GameObjectFiles.xml` and the files it names | One list, no EaW/FoC merge; files in list order; first definition of a type name wins; `Variant_Of_Existing_Type`; unknown names in lists dropped (`OBJECT_TYPES.md` §1–§6) | **main** (engine/objects) |
| Galaxy and campaigns | `TradeRouteFiles.xml`, `CampaignFiles.xml`, the planet / route / campaign XML they name | One `CampaignFiles.xml` (first match, no merge); route and campaign lists de-duplicated and parsed in ascending CRC32 of the listed name; last definition wins as a whole (`PHASE2_ANSWERS.md` Q13, `FALLBACK_CAMPAIGNS.md`) | **main** (engine/data) |
| Story / campaign scripts | Story XML named by the campaign, `Data\Scripts\Story\*.lua` | `docs/re/STORY.md`, `L1_S4_REMAINING.md` | **main** (engine/lua, story session) |
| Lua overrides | `Data\Scripts\` under the original's fixed roots: `AI\`, `AI\SpaceMode\`, `AI\LandMode\`, `AI\StoryArc\`, `Interventions\`, `Evaluators\`, `GameObject\`, `Story\`, `Library\`, `FreeStore\`, `Miscellaneous\` | Scripts are looked up by name under the roots (`.lua`, then `.lc`); a loose mod script overrides an archived one (`SUBSYSTEMS.md` "Lua loading"). Language is Lua 5.0.2 (DECISIONS.md); precompiled chunks must match the port's VM | **main** (`engine/lua/src/script_loader.cpp`) |
| Object scripts | `Lua_Script` on a type, `Data\Scripts\GameObject\` | `docs/re/OBJECT_SCRIPTS.md` | **Q6 stack** (`engine/apps/sim/src/object_scripts.cpp`) |
| AI data | `Data\XML\AI\` (templates, goals, equations, difficulty), `Data\Scripts\AI\` plans | `AI_GALACTIC*.md`, `AI_TACTICAL*.md` | **main** |
| Game constants and the other boot databases | `GameConstants.xml` and every database in the boot list (`MAIN_LOOP.md` startup step 8: SFX, music, speech, abilities, hard points, movement classes, difficulty, scoring, ...) | Loaded once at startup through the VFS | **main** for those the port loads; the rest follow their phase |
| Maps | `Data\Art\Maps\*.ted` | Skirmish map catalog: official = FoC loose + every MEG, custom = `Data\CustomMaps` + each mod folder's `Data\Art\Maps` (`UI.md` §9.2) | **main** |
| Text and fonts | `Data\Text\MasterTextFile_<lang>.dat`, mod fonts | Through the VFS; font search includes the mod folders | **main** |
| Art, audio, video | Models, textures, animations, particles, SFX / music / speech, Bink movies | Loaded by name through the VFS when referenced | **main** |
| Effects (shaders) | `Data\Art\Shaders\**.fx`, `.fxo` | Retail adds each `MODPATH` / `STEAMMOD` folder's `Data\Art\Shaders\` and `\Terrain\` to the shader search (PHASE2_ANSWERS §14); the port's effect order already includes them (`engine/materials/include/alamo/materials/effect_loader.hpp`). The renderer draws with its own SPIR-V translation of the shipped effects. A mod `.fxo` that adds or replaces an effect goes through the same translation; `.fx` sources and shader model 3 do not yet (§4.1) | **main** (modc t11, §4.1); `.fx` compile: **modc t23**; SM3: **modc t22** |

Not supported (by decision, for every mod): multiplayer (no networking), loading the original game's save files, and
retail 32-bit Lua bytecode (the x64 VM rejects it; retail's own `64Patch.meg` ships text sources for that reason,
`SUBSYSTEMS.md` "Lua loading"). Lua written for 5.1 or later fails to compile on the 5.0.2 VM, as in retail.

### 4.1 Effects (shaders) in mods

**Original** (`docs/re/RENDER.md` §6, `SPACE_AND_OPEN.md` D.3): each mod folder's `Data\Art\Shaders\` and `\Terrain\` are
searched before the base ones; every `*.fx`, then every `*.fxo`, is loaded at boot; a name is looked up as `<name>.fx`
first, then `<name>.fxo`, so **a `.fx` source wins over a `.fxo` of the same name** and is compiled with D3DX at run time.
The first technique (file order, LOD within Shader Detail) that passes `ValidateTechnique` is used. A missing effect draws
the built-in green default; a file that fails to compile or load draws the same green effect under its own name.

**Port** (no fixed shader set: every effect is looked up by name through the VFS, mod folders first;
`engine/materials/include/alamo/materials/effect_loader.hpp`, `effect_audit.hpp`):

| Case | What the port does | Issue kind |
|---|---|---|
| Mod `.fxo` replaces or adds an effect | Parsed like a shipped one; shader passes are translated (shaderxlat → HLSL → DXC → SPIR-V) on background jobs and cached on disk. Same result as the original | none |
| `.fx` and `.fxo` of the same name | The `.fxo` is loaded; the original compiles the `.fx` instead (same result when the `.fxo` was built from that source) | `source_shadowed` |
| `.fx` without a `.fxo` | Green default under the effect's name (= the original's result when its compile fails); before t11 it counted as missing (silent shared default) | `source_only` |
| `.fxo` that does not parse / load | Green default under its name (D.3) | `load_failed` |
| No selectable technique | Shared green default (D.3) | `no_technique` |
| Best technique has a shader pass the port has no program for (`vs_3_0` / `ps_3_0`; a VS without a PS) | [port] That technique is skipped like one failing `ValidateTechnique`; the next technique by LOD / file order whose passes all have a program is drawn (`MaterialEffect::skipped_technique`) | `technique_skipped` |
| No technique the port can draw | Submeshes draw unlit (base texture, the pass's blend); Heat / Shadow bucket submeshes are not drawn | `unsupported_pass` |
| Translation or DXC fails for a pass | Same unlit fallback; logged once per program | `translate_failed` |

Nothing crashes and nothing is skipped silently: every case except the first is a `materials::ShaderIssue`
(kind, effect, path, `in_mod`, detail). Up front: `materials::audit_effects()` / `alamo_effect_audit <FoC dir> [MODPATH=..]
[--no-dxc] [--json FILE|-]` (JSON schema `alamo.effect_audit` v1: counts + `issues`). At run time:
`render::EffectCache::issues()` and `Renderer::shader_issues()` (adds translation failures); each issue is also logged
once. The load report shows them through modc t21.

**Installed mods** (2026-11-06, base FoC + 64Patch / Patch / Patch2 = 82 effects; workshop folder
`F:\SteamLibrary\steamapps\workshop\content\32470`; the other 8 items ship no shaders). "Differs" = same path as a base
file, other bytes; most mods ship a full recompiled set. Audit with DXC, so every translated pass also compiled.

| Item | Shader files (loose + MEG) | Same as base / differs / new | `.fx` / `.fxh` sources | Effects (from the mod) | Issues |
|---|---|---|---|---|---|
| 1125571106 Thrawn's Revenge | 87 | 31 / 51 / 5 | `AlamoEngine.fx`, `.fxh` | 85 (84) | `source_shadowed` AlamoEngine |
| 3622043265 Thrawn's Revenge (other copy) | 88 | 31 / 51 / 6 | + `Terrain\_TerrainRenderBump.fx` | 86 (85) | `source_shadowed` AlamoEngine, `source_only` _TerrainRenderBump |
| 1976399102 Fall of the Republic | 88 | 31 / 51 / 6 | as 3622043265 | 86 (85) | as 3622043265 |
| 1770851727 | 93 | 31 / 51 / 11 | none | 93 (93) | `technique_skipped` Planet_Deathstar (`sph_t3` vs_3_0 / ps_3_0 → `sph_t2`) |
| 2858787558 | 55 | 0 / 49 / 6 | none | 88 (55) | `unsupported_pass` MeshFresnelGlow (only SM3 bytecode: unlit) |

All other mod effects (including 3-11 new `.fxo` per mod, e.g. MeshShieldEngines, MeshBumpSpecGlowColorize, DEFAULT.fxo) load
and translate: 0 `translate_failed`, 0 `load_failed` in every mod. `_TerrainRenderBump.fx` is preloaded by the original;
whether any model names it was not checked (t21's report cross-references issue effects with the models' materials). The project-local `fxc.exe` (Windows SDK) compiles `AlamoEngine.fx` with `/T fx_2_0`
(warning X4717 "Effects deprecated"), so a compile step for `.fx` sources is feasible (t23).

## 5. Fallbacks and what the loader reports

The original loads with most data problems and only logs them; the port does the same and reports them. Nothing is guessed
from a mod's name. Two levels:

- **Error**: the data cannot be played. `alamo_game` refuses to start the session; `alamo_mod_check` exits 1.
- **Warning**: the original would load and run on; the port does too, and the report lists it.

| Situation | Original | Port | Level | Status |
|---|---|---|---|---|
| FoC folder missing / not a directory | Abort | VFS create fails, message names the folder | Error | **main** |
| EaW install not found | Abort | Warning, EaW MEGs and `GameData` skipped (PHASE2 "VFS: missing EaW install warns") | Warning | **main** |
| `MODPATH=` folder without `Data\` | Nothing found there | `mod path has no Data folder` | Warning | **main** |
| `STEAMMOD=` item not installed | Nothing mounted | `workshop item not found; nothing mounted` | Warning | **main** (modc t10) |
| `MegaFiles.xml` names a missing or unreadable MEG | Skipped | `MegaFiles.xml entry not found` / `cannot open MEG` | Warning | **main** |
| A list file (`GameObjectFiles.xml`, ...) names a missing file | Logged, skipped | Loader diagnostic, file skipped; the load report lists it under missing files | Warning | **main** |
| An object type's `Space_` / `Land_` / `Galactic_Model_Name` names a model no layer has | Draws nothing (not traced) | Listed under missing files (a name without an extension is also tried with `.ALO`) | Warning | **main** (modc t05) |
| A loader step throws | Crash or log, depends | Step reported as failed with the step name | Error | **Q6 stack** (`GameData::stages`, `alamo_mod_check`) |
| A campaign has no planets | Listed in the Galactic Conquest menu (menu-only rows, e.g. headers; starting one not tested: `re/MODC_LOADER.md` §4) | Listed by name as a menu entry that cannot be started (retail lists it: `docs/re/MODC_LOADER.md` §4, modc-loader); one error when no campaign has planets | Warning; Error if none has planets | **Q6 stack**; level: modc t14 |
| Campaign row names an unknown planet / route / faction | Row skipped | `build_galaxy_map` diagnostic, grouped by message | Warning | **Q6 stack** |
| Faction `Basic_AI` / `AI_Player_Control` names an undefined AI type | "Unrecognized AI player type", no AI | No AI for that player, diagnostic | Warning | **Q6 stack** |
| Faction has no skirmish default forces | Places nothing | [port] up to three of the faction's cheapest buildable units (`data_fallback_forces`), listed as a diagnostic | Warning | **Q6 stack** |
| A Lua script does not compile (also: a UTF-8 BOM at its start, which the original does not skip: `re/MODC_LOADER.md` §2) | `LuaScript Error in <name>.lua!`, script does not run | Listed with the compiler message | Warning | **Q6 stack** (`alamo_mod_check` compiles every `Data\Scripts\**.lua`) |
| A script calls an engine function the port does not bind, or reads a value that fails | Script error, the script stops | The load report runs the main chunk of every entry script (not `Library\`) once in its own VM and lists the errors, and lists the engine globals / methods the scripts call that the port registers as unbound stubs (static scan). Later failures show up in session runs (`alamo_headless --story` / `--galactic-ai`) | Warning | **main** (modc t05) |
| XML that is not well-formed | Its own reader is lenient; element names may start with a digit (`<181st>`: `re/MODC_LOADER.md` §3) | Digit-led names parse as elements (probe); other recoveries (`<?xml ...>` without `?`, mismatched close tags, a `<` that starts no markup) keep the file and are listed as loader diagnostics | Warning | **main** (engine/data `xml.cpp`; digit-led names: modc t14) |
| A script's main chunk fails after reading a `GlobalValue` another script sets during the session | Runs: the setter ran first | Listed apart as session-dependent, with the key and the setting script (below) | Info (not counted) | **modc t12** |
| An XML tag the port does not read | Ignored | Per database (object types, factions, campaigns, trade routes, hard points, GameConstants, SFX / music / speech events, Audio.xml, tactical cameras, movies, difficulty adjustments, lightning effects, targeting priority sets, mouse pointers): tags that neither the original's table for that class nor the port's loader reads, per tag with counts (below) | Warning | **main** (modc t05 object types, t15 the others) |
| A submod checked without its parent | Its lists name the parent's XML files: skipped, hundreds of missing files | Listed as missing files as before, plus a submod hint naming the installed mod that has them (below) | Info (not counted) | **modc t16** |
| A mod `.fxo` replaces or adds an effect | Loaded | Loaded and translated like a shipped effect (§4.1) | — | **main** (modc t11) |
| An effect only a mod ships as `.fx` source, or a `.fx` beside a `.fxo` | Compiled by D3DX | `.fx` only: green default under its name; with a `.fxo`: the `.fxo` (§4.1, `source_only` / `source_shadowed`) | Warning | **main** (modc t11); in the report: **modc t21**; compile: **modc t23** |
| An effect's best technique uses shader model 3 | Drawn (SM3 hardware) | Next drawable technique (`technique_skipped`), else unlit (`unsupported_pass`) | Warning | **main** (modc t11); report **modc t21**; SM3: **modc t22** |
| An effect fails to load, has no technique, or a pass fails translation | Green default / validation failure | Green default (`load_failed`, `no_technique`) or unlit (`translate_failed`) | Warning | **main** (modc t11); report **modc t21** |

The report: `alamo_mod_check [--game DIR] [MODPATH=..] [STEAMMOD=..] [FALLBACKPATH=..] [--out FILE] [--json FILE|-] [--verbose]
[--no-campaigns] [--no-lua] [--no-lua-run] [--no-files] [--no-xml-tags] [--no-submod]` loads the data exactly as
`alamo_game` does and prints files (VFS layers, warnings, per-folder new / replaced counts), loader steps, content counts,
campaign checks, missing files, unknown XML tags, Lua compile errors, main-chunk errors (and the session-dependent ones
apart), unbound engine calls and the submod hint, then `== result: OK|ERRORS, N errors, M warnings`
(**main**, `engine/apps/game/src/mod_check.cpp`; exit 0 without errors, 1 with, 2 usage). Each finding of the missing
files, XML tags and Lua sections is marked `[mod]` when it comes from a file in a mod folder: base FoC itself has some (49
missing cinematic models; 72 unread tags: 56 object type tags, 8 GameConstants, 3 hard point, 2 faction, 2 campaign, 1
tactical camera), so a mod's own problems are the marked ones. `--json` writes the summary below (`-`: to stdout instead
of the text). **modc t08** shows that summary in the game's mod picker.

A main-chunk error is a lead, not proof: the script runs with no session (no players or objects). Session state another
script sets is handled (modc t12): every entry script's main chunk runs with one `GlobalValue` store shared by the runs
(as the session's), and with `GlobalValue` wrapped so the check sees which keys a chunk read while they were unset. A
chunk that fails after reading such a key, when some script sets it (a `GlobalValue.Set` / `GlobalValue:Set` call with
that literal key outside a `--` comment, wherever it is, e.g. a `Base_Definitions` the session calls at startup), is
listed as session-dependent with the key and the first setting script (path order), not as a load warning. Example:
Thrawn's Revenge's `ModContentLoader` reads `GlobalValue.Get("MOD_ID")`, which its `GameScoring` sets in
`Base_Definitions`: 77 of TR's 80 main-chunk errors (Fall of the Republic: 107 of 111) are session-dependent; the 3 left
are `require` targets TR does not ship. A key no script sets stays an error.

Unread XML tags (modc t15): for the object types, the field tables and sub-object elements as before; for the other
databases the port loads (`kXmlDatabases` in `mod_check.cpp`: where each one's entries are, the list file's `<File>`
entries or one file, and the entry element), a direct child of an entry counts as read when the original's descriptor
table for that class has a row for it (`docs/re/xml_field_tables.md` through `objects::field_schema()`, by segment owner)
or the port's loader reads it although no row lists it (named per database with the loader: hard point `Type`,
GameConstants `Ships_Per_Stack`, Audio.xml `Ambient_Map_Sounds`, tactical camera `MinHeightAboveTerrain`, and every
speech event tag, whose class has no table in the doc). Not checked: CommandBarComponent (its table misses 48 tags base
FoC uses) and nested databases (RadarMap.xml).

Submods (modc t16): when a mod's own list files (`Data\XML\*Files.xml`) name XML files that no layer has, the report
looks for them in every installed mod outside the stack: the workshop items of the Steam library folders (the same
folders as the STEAMMOD= resolver, §3.1) and the folders of `<game>\Mods`. Each candidate is opened as a tree of its own
(its loose `Data` and the MEGs its `MegaFiles.xml` / `Patch*.meg` name), so a hit is the candidate's own file. The
candidate with the most hits is the likely parent when it has at least 2 and at least half of them; ties go to the one
whose workshop id appears in the mod's `modinfo.json` (e.g. "Requires Thrawns Revenge 1125571106"), then to workshop
items, then by id. Models are not counted: full mods miss art too (TR: 27 models, 0 list entries), only a mod whose
database lists another mod's files is a submod. The hint gives the arguments to check it stacked (the mod's own, then
the parent's: `STEAMMOD=<sub> STEAMMOD=<parent>`, the order of Thrawn's Legacy's own launcher). Nothing is taken from a
mod's name. On the workshop mods installed here (2026-11-06): 3643661236, 3803967503 (Thrawn's Legacy) and 3806968867
come out as submods of 1125571106 (241 / 259 / 212 missing list entries, 241 / 259 / 207 of them in TR), and load
with 0 errors stacked on it; 2858787558's 18 missing list entries are in no installed mod.

### 5.1 The machine-readable summary (`alamo.mod_report`, version 2)

One JSON object, UTF-8 (bytes of the data that are not UTF-8 are written as the Latin-1 character of that value), keys in
the order below, every list complete. A new key may be added in a later version of the same number; a key is never
removed or changed in meaning without raising `version`. Paths are game-relative and upper case unless noted.

| Key | Type | Meaning |
|---|---|---|
| `schema`, `version` | string, number | `"alamo.mod_report"`, `2` |
| `result` | string | `"ok"` (no errors) or `"errors"` |
| `errors`, `warnings` | number | as the text report's result line (levels in the table above) |
| `game`, `args` | string, [string] | the FoC folder and the VFS command line as given |
| `loaded`, `load_error` | bool, string | whether the data loaded; why not |
| `files` | object | `mod_dirs` [host path], `vfs_warnings` [string], `added`, `replaced` (mod files with no lower-layer copy / replacing one), `folders` [{`folder` (the first folder below `Data`, `""` for `Data` itself), `added`, `replaced`}] |
| `loaders` | [object] | per loader step in load order: `name`, `ok`, `ms` (wall clock, varies per run), `diagnostics` [string] |
| `content` | object | `campaigns`, `planets`, `trade_routes`, `object_types`, `factions`, `ai` (bool: AI data loaded), `ai_plans`, `tactical_space_plans`, `tactical_land_plans`, `official_maps`, `custom_maps` |
| `campaigns` | object | `checked`, `without_planets` [campaign name] (warnings: menu entries that cannot be started; when it lists every checked campaign, one error), `skipped_rows` (number of skipped rows over all campaigns), `warnings` [one line per message with its campaigns] |
| `missing_files` | [object] | `path`, `referenced_by` (the first naming list file, or `"<type> (<tag>)"`), `references`, `in_mod`; sorted by path |
| `unknown_xml_tags` | [object] | `database` (`"object types"`, `"factions"`, `"campaigns"`, `"trade routes"`, `"hard points"`, `"game constants"`, `"sfx events"`, `"music events"`, `"speech events"`, `"audio"`, `"tactical cameras"`, `"movies"`, `"difficulty adjustments"`, `"lightning effects"`, `"targeting priority sets"`, `"mouse pointers"`), `tag` (first spelling), `occurrences`, `types` (distinct entries using it), `first_file` (as listed, not upper-cased), `in_mod`; most occurrences first, then by database and tag |
| `lua` | object | `scripts` (compiled), `compile_errors` [{`path`, `error`, `in_mod`}], `chunks_run` (entry scripts run), `run_errors` [{`path`, `error`, `in_mod`}] (not session-dependent), `session_errors` [{`path`, `error`, `in_mod`, `global_value` (the unset key read), `set_by` (the first script setting it)}] (info), `unbound_calls` [{`name`, `kind` (`"global"` / `"method"`), `call_sites`, `scripts`}] |
| `submod` | object | `likely` (bool: stack it on `parents[0]`), `missing_list_entries` (XML files the mod's own lists name that no layer has; 0: no hint), `parents` [{`id` (workshop id or `Mods` folder name), `kind` (`"workshop"` / `"local"`), `path` (host folder), `arg` (`"STEAMMOD=<id>"` / `"MODPATH=Mods\<name>"`), `provides` (of the missing list entries), `mentioned` (its id is in the mod's `modinfo.json`)}] (most `provides` first, at most 5), `stacked_args` [string] (when likely: `args` then the parent's `arg`) |
| `check_errors` | [string] | a check that threw (errors) |

`in_mod` is true when the file the finding comes from resolves to a mod folder. The synthetic mods of
`engine/apps/game/testdata/mod_fixture` (modc t07, t12, t15, t16; README there) are checked through this summary in ctest
(`alamo_game_mod_fixture_tests`, `alamo_mod_check_fixture`).

Version 2 (modc t12, t15, t16, 2026-11-06): `run_errors` no longer holds the session-dependent errors (now in
`session_errors`), so `warnings` no longer counts them; `unknown_xml_tags` covers the other databases and has
`database`; `submod` is new; a campaign without planets is a warning unless no campaign has planets (then one error).

## 6. Port rules that apply only when a mod layer is loaded

Each is keyed on "a mod layer is loaded", never on which mod (§1 rule 4). With no mod loaded the port behaves as base FoC.

| Rule | Where | Why | Decided |
|---|---|---|---|
| Skirmish buy list also requires the type to be named in some type's `Tactical_Buildable_Objects_Multiplayer` (`SkirmishBuyOptions::multiplayer_list_only`) | `engine/apps/game/rules` (**Q6 stack**) | Mods list option / marker / layer types as buildable; the interim rule drops them (`SKIRMISH_SETUP.md` §2.7 port notes) | Gabriel, 2026-11-04 |

Add a row here for every new rule of this kind, in the same change that adds it.

## 7. Known violations (to be removed)

Found while writing this contract; the full audit is **modc t02**, the replacements **modc t03**, the lint **modc t04**.

- `ai_type_for(faction)`: built-in AI types for `Empire`, `Rebel`, `Underworld` (**main**, `engine/apps/game/src/game_data.cpp`,
  used by `game_session.cpp`). The **Q6 stack** moves it to `engine/apps/game/rules/skirmish_rules.hpp` and limits it to
  "stock" factions (`is_stock_faction`: the name matches and the data's `Basic_AI` agrees), which is still a name test.
- `default_skirmish_fleet(faction)` / `default_skirmish_ground(faction)`: built-in unit lists for the same three factions
  (**main**; the Q6 stack uses them only for stock factions).
- `SkirmishSetup` defaults `human_faction = "Rebel"`, `ai_faction = "Empire"`; `make_skirmish_galaxy` looks up `"Hostile"` by
  name and picks the battle planet from `Kuat` / `Coruscant` first; the skirmish map list puts `_MP_SPACE_KUAT` first
  (**main**, `engine/apps/game/include/alamo/game/game_data.hpp`, `game_data.cpp`).

Each needs either a data-driven replacement or a `docs/re/` citation showing the original does the same (§1 rule 3).

## 8. Where the parts are implemented

| Part | Task |
|---|---|
| VFS, MODPATH / FALLBACKPATH, resolution order, saves remember the mod | done (Phase 2, N1) |
| Mod diagnostics `alamo_mod_check`, loader stages, `Basic_AI`, skirmish forces from the data, object scripts | Q6 stack |
| Audit of hard-coded names and counts | modc t02 |
| Data-driven replacements, base-FoC goldens unchanged | modc t03 |
| Lint: no mod / faction name literals in `engine/` sim code | modc t04 |
| Load report for any mod folder, machine-readable (§5, §5.1) | modc t05 |
| Second mod (not Thrawn's Revenge) loads and plays | modc t06 |
| Synthetic fixture mod in ctest through the real `MODPATH` route | modc t07 |
| Mod picker in the game UI | modc t08 |
| `STEAMMOD=` resolver from the Steam library folders, no Steam API (§3.1) | modc t10 |
| Mod-only effects: what the renderer does, issue record, `alamo_effect_audit` (§4.1) | modc t11 |
| Shader issues in `alamo_mod_check` and `alamo.mod_report` | modc t21 |
| Shader model 3 in shaderxlat | modc t22 |
| `.fx` source compilation (prebuild / cached) | modc t23 |

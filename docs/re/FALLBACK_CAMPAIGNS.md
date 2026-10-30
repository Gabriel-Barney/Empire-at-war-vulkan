# A8: EaW campaigns and assets under `FALLBACKPATH=`

Status: static analysis complete. One open point needs a short runtime re-probe (see "Discrepancy with the probe").
Inputs: `extracted/foc_data_text.zip`, `extracted/eaw_data_text.zip`, `foc_INDEX.json` / `eaw_INDEX.json`, `docs/assets/catalog.json`, the Ghidra exports (exe), and directory listings of `corruption\Data` and `GameData\Data` (read-only).
Scripts: `/home/claude/work/a8/scripts/{campaigns,deps,closure}.py` (container only).

> **Status 2026-10-30:** The scripts named on the line above are not in the repository (a search of `tools/`, `docs/` and `scripts/` for `campaigns.py`, `deps.py` and `closure.py` finds none), so the counts below cannot be regenerated from it.

## Answer

**No. `FALLBACKPATH=` does not drop any campaign from FoC's list, and it costs FoC's campaigns no XML, Lua, model, map or audio file.**

1. **EaW-only campaigns are never in FoC's list, with or without `FALLBACKPATH=`.**
   - The list comes from one `Data\XML\CampaignFiles.xml`, opened once with first-match lookup. Nothing is merged across tiers.
   - FoC's copy (in `config.meg`) always beats EaW's copy (EaW `config.meg`, mounted below FoC's MEGs).
   - FoC's file lists only FoC campaign files, so EaW's sandbox GCs, EaW multiplayer GCs and the EaW story campaigns are never loaded in FoC. That is vanilla behaviour, not a side effect of `FALLBACKPATH=`.
   - "Equal Footing" (`Sandbox_Equal_Footing_*`, `TEXT_CAMPAIGN_SINGLE_56`) is a **FoC** campaign. Its XML and maps all come from FoC files.
2. **FoC does depend heavily on EaW-only assets, but it reaches them through EaW's MEGs.** The loose-file fallback tier is not involved.
   - `FALLBACKPATH=` only adds a loose-file search root.
   - The EaW archives are mounted from the EaW install path that the boot code finds on its own (registry, then `..\..\EAW\Run\`, then `..\GameData\`). The archives are: EaW `MegaFiles.xml` entries, EaW `patch`/`patch2`/`64Patch`, and EaW `Audio\SFX\*.meg`.
   - The `FALLBACKPATH=` handler never touches that path.
   - Under `FALLBACKPATH=`, the only files at risk are **loose** files under `GameData\Data`. None of them is a campaign dependency.

Confidence: **high** for (1). **High** that `FALLBACKPATH=` does not change which EaW MEGs are mounted. **Medium** on whether it replaces or comes before the loose `GameData\Data` root (see below). That last point affects no campaign either way.

## Campaign-list loader (behaviour)

The loader is `CampaignDataManager.System_Initialize`. It is the `Game_Boot_Init_Filesystem_helper_5_helper_55` @ RVA 0x479fe0, called from the boot sequence right after `TradeRouteManager.System_Initialize`, with the path `.\Data\XML\CampaignFiles.xml`.

1. Logs `Creating campaign data entries from XML database file %s...`.
2. Parses the list file with the generic XML loader (0x2397d0, logs `xmlParseFile(%s);`). That loader does a single `FileClass::Open`, so normal first-match resolution applies: MODPATH > FoC loose > MEGs (FoC over EaW) > fallback roots.
   - **No merge.** A mod that ships its own `CampaignFiles.xml` replaces FoC's whole list.
   - If the list file itself fails to parse, the function returns false and boot reports `Error: Unable to initialize CampaignDataManager.`
3. For each `<File>` child, it builds `Data\XML\<name>` and tries to open it through `FileClass`.
   - **Open fails:** it logs `Error: Unable to open listed XML file '%s'` to the debug log and goes on to the next entry. The user sees nothing, which matches the probe (missing files are skipped silently).
   - **Open succeeds:** it hashes the name (CRC) and stores the file in a map keyed by that hash, so a duplicated entry is loaded once.
4. Second pass: for each stored file it logs `Creating campaign data entries from database file %s` and parses it, again through the generic loader, so the same first-match rule applies.

Consequence: each `Campaigns_*.xml` is also first-match. A file that FoC and EaW both provide always comes from FoC.

## Campaign table

Planets and trade routes were checked against FoC's `Planets.xml` (Patch2) and `TradeRoutes.xml` (config). All names used by both games' campaigns exist in FoC. Story closure means: plot file, then `Active_Plot` XML, then `Lua_Script`, then `.ted` references. Provider is the archive that wins in FoC's effective file set.

### Listed by FoC `CampaignFiles.xml` (what FoC actually shows)

| File (FoC config.meg) | Campaigns | Story XML + Lua | Planet tactical maps | EaW-only dependency (served from EaW MEGs) |
|---|---|---|---|---|
| `Campaigns_Underworld_GC.xml` | 18: Gateways, Equal Footing, Origin of Corruption, Heart of the Maw, Rim Worlds, Clusters (each in Underworld, Rebel and Empire versions) | FoC (6 files each) | FoC (23–107 TEDs) | Galactic planet models (see below) |
| `Campaigns_Underworld_Story.xml` | `Full_Story_Campaign_Underworld` | FoC (incl. `Story_Campaign_Underworld.lua`, UM01–UM12 TEDs) | FoC | Planet models |
| `Campaigns_Multiplayer.xml` | 6 `Multiplayer_Sandbox_*` (same six maps) | none | FoC | Planet models |
| `Campaigns_Tutorial.xml` | `Tutorial_One` … `Tutorial_Seven` | FoC XML | FoC | **Mission maps `TM01_GAINING_GROUND`, `TM03_EMPIRE_AT_WAR`, `TM05_IMPERIAL_POLICY`, `TM06_LAND_MULTIPLAYER`, `TM07_SPACE_MULTIPLAYER` (.ted)**, plus planet models |
| `Campaigns_Underworld_Tutorial.xml` | `Tutorial_Underworld` | FoC (8 files) | FoC | Planet models |

### Listed only by EaW `CampaignFiles.xml` (never loaded by FoC)

| EaW file | Campaigns | FoC also ships the file? | If a mod lists it in FoC |
|---|---|---|---|
| `Campaigns_Singleplayer.xml` | 16: Alderaan's Demise, Shipyards of Kuat, Empire Surrounded, Empire at War, Galactic Conflict, The Conflict Begins, The Lines Are Drawn, Closing on Coruscant (each in Rebel and Empire versions) | **Yes** (FoC config.meg, unlisted; same 16 campaign names) | Story plots, planets, routes and planet TEDs all resolve to FoC files |
| `Campaigns_Multiplayer.xml` (EaW version) | 7: Hoth, Bespin, Endor, Naboo, Tatooine, Yavin, Galaxy-Wide Conflict | FoC's file has the **same name** with different content, so EaW's version is unreachable in FoC | n/a |
| `Campaigns_Alpha.xml` | `Full_Story_Campaign_Empire`, `Full_Story_Campaign_Rebel` | **Yes** (FoC config.meg, unlisted) | Story XML/Lua from FoC; mission maps EM02–EM15 and RM01–RM09 (24 TEDs referenced from FoC's `Story_Campaign_*_Act_*.xml`) come only from EaW `maps`/`patch*` MEGs |
| `Campaigns_Tutorial.xml` | Same seven tutorials | Yes (FoC's copy wins) | n/a |

## EaW-only dependencies of FoC itself (all served from EaW MEGs)

A scan of the FoC effective text tree (XML/Lua/TXT, comments stripped) against the catalog finds references to names that exist **only** in EaW archives:

| Category | Count | Main referencing files | Under `FALLBACKPATH=` |
|---|---:|---|---|
| WAV (SFX) | 6,261 | `SFXEvents*.xml` | Kept: EaW `Audio\SFX\*.meg` is mounted by the SFX managers from the EaW install path |
| MP3 (speech/music) | 1,011 | `SpeechEvents.xml`, `MusicEvents.xml` | Kept (EaW `englishspeech`/`music` via EaW `MegaFiles.xml`) |
| ALO models | 854 | `SpaceProps`, `Hardpoints`, `Particles`, `Props_*`, `Planets.xml`, … | Kept (EaW `models`) |
| BIK movies | 34 | `Movies.xml` | Kept (EaW `movies`) |
| TED maps | 24 | FoC tutorials (5), FoC copies of EaW story acts (19) | Kept (EaW `maps`/`patch*`) |
| TEC camera | 6 | `GroundVehicles`, `SpaceUnitsCapital`, `UniqueUnits` | Kept (EaW `cinematics`) |
| DDS | 3 | `GameConstants.xml` (Bink/GameSpy/Miles logos) | Kept (EaW `textures`) |
| Lua | 0 | none | None of the 46 EaW-only Lua files (interventions, sandbox events) is referenced from FoC data |
| XML | 0 | none | None of the 11 EaW-only XML files (Intervention*, SandboxEvent*, Ewok*) is referenced |

The scan is heuristic: 1,247 FoC references resolve in neither game, which gives the noise baseline (dead or localised names).

**Every FoC GC depends on EaW-only planet models.** `Planets.xml` uses 30 EaW-only `W_planet_*.alo` files as `Galactic_Model_Name`, `GUI_Model_Name` and `Destroyed_Galactic_Model_Name`, for example `W_planet_Temperate01`, `W_planet_Asteroids` and `W_planet_hoth_low`. That covers essentially every planet in every FoC campaign. These models live in EaW `models.meg`.
- If the EaW MEG mount were ever lost (no EaW install found, or a reimplementation that drops the EaW mount), FoC's galactic map would lose its planets.
- `FALLBACKPATH=` does not cause this.

### What `FALLBACKPATH=` can actually affect: loose `GameData\Data`

From the directory listing, the loose (non-MEG) files there are:
- `EnvironmentSets\*.tee`, `MaterialSets\*.tem`: terrain-editor sets. The exe references neither directory name.
- `Text\mastertextfile_english.dat`, `Text\creditstext_english.dat`: FoC has its own `corruption\Data\Text`.
- `Art\Textures\splash*.dds`: FoC has its own.
- Ship-name `*.txt` files.

No campaign references any of these files.

## Fallback mechanics (static)

- `Parse_Command_Line` (0x67d70): the `FALLBACKPATH=` handler copies the value into a temporary and calls `Add_Fallback_Search_Path` (0x214110). That function appends to the fallback search-root vector, ignoring empty strings. It does not write the `FALLBACK_PATH` global at RVA 0xa15320.
- `Find_EaW_Fallback_Path` (0x21dce0) runs after command-line parsing. It sets the 0xa15320 global from one of three sources: a separate optional override string (a global that `FALLBACKPATH=` does not write), the registry, or `..\..\EAW\Run\` / `..\GameData\`.
- The boot code uses the 0xa15320 global for three things:
  1. mounting `<EaW>\Data\MegaFiles.xml` and EaW `patch*.meg`;
  2. the SFX managers mounting `<EaW>\Data\Audio\SFX\SFX2D_Non_Localized.meg`, `SFX2D_<lang>.meg` and `SFX3D_Non_Localized.meg` before FoC's own copies;
  3. one more `Add_Fallback_Search_Path` call **after** FoC's MEGs are mounted.
- `FileClass::Try_Search_Path_List` (0x214550) walks every entry in that vector in order, and the first `CreateFileA` that succeeds wins.

### Discrepancy with the probe

Read statically, the fallback roots are a **chain**: `FALLBACKPATH` entries (registered during parsing) come first, then `GameData` (appended later in boot). The runtime probe recorded that `FALLBACKPATH` *replaces* `GameData`.

- A probe that put the tag in both locations cannot tell the two cases apart.
- Suggested **Probe E:** put a file only in loose `GameData\Data\XML\`, launch with `FALLBACKPATH=<empty dir>`, and check whether it resolves.
- Neither outcome changes the campaign answer above, because no campaign data is loose in `GameData\Data`.

## Recommendation for the Phase 2 VFS

1. **Model the tiers explicitly and separately:**
   - `ModRoots[]` (command-line order)
   - `BaseLoose` (FoC `Data\`)
   - `Archives[]`: push-front order, holding FoC MEGs above **EaW MEGs mounted from the detected EaW install**, plus SFX MEGs in their own leaf-name namespace
   - `FallbackRoots[]`
2. **Keep the EaW archive mount independent of `FALLBACKPATH=`**, as vanilla does. Losing it removes the 30 planet models every GC uses, plus thousands of SFX, speech and model files.
3. **Default behaviour must match vanilla.**
   - Keep `CampaignFiles.xml` first-match and single-file.
   - Skip missing `<File>` entries silently, but log them through the mod-diagnostics channel (Phase 5).
   - Fail boot only if the list file itself cannot be parsed.
4. **Opt-in improvements** (off by default, behind flags):
   - `--fallback-chain`: guarantee `FALLBACKPATH` roots, then `GameData`. This may already be vanilla behaviour, pending Probe E.
   - `--merge-campaign-lists`: union of `CampaignFiles.xml` across tiers, de-duplicated by name hash as the vanilla second pass already does. This would let EaW's GCs appear in FoC. Their XML and assets already resolve, apart from the EaW story-mission TEDs, which come from EaW MEGs.

> **Status 2026-10-30:** The merge is implemented as the load option `LoadOptions::merge_campaign_lists` (default false) with `extra_campaign_lists` (`engine/data/include/alamo/data/game_database.hpp:55`, `engine/data/src/game_database.cpp:336`); no command-line flag of that name exists in `engine/`.

5. **Diagnostics:** warn when a campaign's dependency closure (story → Lua/TED, planets → models/TEDs) resolves only through EaW archives, so mod authors can see EaW coupling.

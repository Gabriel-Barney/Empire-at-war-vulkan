# File-priority probe v2 (FoC, Steam 64-bit 1.121)

This probe checks the binary-analysis prediction for how `Data\` paths are resolved. The
first match wins:

> **(a)** MODPATH dirs in command-line order (earlier wins) > **(c)** FoC loose `corruption\Data` >
> **(d)** MEGs, newest-mounted first. A mod's own `MegaFiles.xml` replaces the base list, and within one
> list the later MEG wins. > **(e)** EaW loose `GameData\Data` > `FALLBACKPATH=`

This replaces probe v1. Delete the old `tools\probe_mod\mods\` folder, and also any
`corruption\Mods\ProbeA/B/C` you already copied from it.

## How it works

`ProbeA` overrides `Data\XML\CampaignFiles.xml`, which normally comes from `config.meg`, to
list nine extra campaign files, `Probe_G1.xml` … `Probe_G9.xml`. Each one is a copy of the
"Gateways" sandbox set and adds one entry to the **Galactic Conquest list**. That entry is
named after the tier its copy came from, for example **`PROBE G3 = C`**. Each `G` file is
supplied by a different set of tiers, so one screen shows every winner.

| Entry | Supplied by | Predicted | Checks |
|---|---|---|---|
| G1 | A, B, C, D2, E | **A** | (a) beats everything |
| G2 | B, C, D2, E | **B** | (b) > (c) |
| G3 | C, D2, E | **C** | (c) > (d) |
| G4 | D2, E | **D2** | (d) > (e) |
| G5 | E, F | **E** | (e) > FALLBACKPATH |
| G6 | D1, D2 (both in ProbeA's MegaFiles.xml, D2 listed later) | **D2** | later-listed MEG wins |
| G7 | D1 only, stored as `data/xml/probe_g7.xml` (lower case, `/`), CRC over the normalised name | **shown** | MEG lookup ignores case and `/` vs `\` |
| G8 | D3 only, stored as `data/xml/probe_g8.xml`, CRC over the *raw* lower-case name | ? | does the engine re-hash stored names or trust the stored CRC? Shown means it re-hashes or compares names |
| G9 | F only | **shown** | FALLBACKPATH is searched at all |

More signals:
- The main-menu buttons carry an `[A]` or `[B]` prefix, showing whose loose `.dat` won.
- Probe entries appearing at all proves that a loose mod file overrides a file in base `config.meg`.

Tier key:
- **A / B:** `Mods\ProbeA` / `Mods\ProbeB` loose files.
- **C:** FoC loose `corruption\Data`.
- **D1 / D2 / D3:** MEGs in `Mods\ProbeA\Data`, listed in its `MegaFiles.xml`. That list repeats the retail FoC list and then adds D3, D1, D2.
- **E:** EaW loose `GameData\Data`.
- **F:** `Mods\ProbeF` through `FALLBACKPATH`.

## Install

Let `GAME` = `F:\SteamLibrary\steamapps\common\Star Wars Empire at War`.

1. Copy `tools\probe_mod\v2\Mods\ProbeA`, `ProbeB` and `ProbeF` into `GAME\corruption\Mods\`.
2. **Optional tier (c).** Copy the three files in
   `v2\OPTIONAL_copy_into_corruption_Data\Data\XML\` (`Probe_G1.xml`, `Probe_G2.xml`,
   `Probe_G3.xml`) into `GAME\corruption\Data\XML\`. Create the `XML` folder if it doesn't exist.
   These are **new files** and don't overwrite anything.
3. **Optional tier (e).** Copy the five files in
   `v2\OPTIONAL_copy_into_GameData_Data\Data\XML\` (`Probe_G1.xml` … `Probe_G5.xml`) into
   `GAME\GameData\Data\XML\`. Create `XML` if needed. These are new files too.

## Launches

Use `cmd` with Steam running, and keep the working directory set to `corruption`:

```
cd /d "F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
```

For each launch, go to Main menu → Single Player → **Galactic Conquest**, scroll the
campaign list, and take screenshots showing all the `PROBE G…` entries plus the main
menu. Then quit. **Don't start a probe campaign.**

| # | Command | Optional files present |
|---|---|---|
| L1 | `StarWarsG.exe MODPATH=Mods\ProbeA MODPATH=Mods\ProbeB FALLBACKPATH=Mods\ProbeF` | C and E installed |
| L2 | `StarWarsG.exe MODPATH=Mods\ProbeB MODPATH=Mods\ProbeA FALLBACKPATH=Mods\ProbeF` | C and E installed |
| L3 | same as L1 | **after deleting** the C and E files (Cleanup step 1) |

### Expected results if the prediction holds

| Entry | L1 | L2 (mod order swapped) | L3 (C and E removed) |
|---|---|---|---|
| menu prefix | [A] | [B] | [A] |
| G1 | A | **B** | A |
| G2 | B | B | B |
| G3 | C | C | **D2** |
| G4 | D2 | D2 | D2 |
| G5 | E | E | **F** |
| G6 | D2 | D2 | D2 |
| G7 | shown (D1) | shown | shown |
| G8 | record it | same as L1 | same as L1 |
| G9 | F | F | F |

How to read deviations:
- If **no** `PROBE` entries appear and the menu shows `[A]`, the mod's loose `CampaignFiles.xml` did not override `config.meg`.
- If a whole tier never shows up, that tier is not on the search path. For example, G9 missing means FALLBACKPATH is not searched. G5 = F in L1 means GameData was replaced by FALLBACKPATH.
- In L2, G6/G7 should still come from D, because B has no `MegaFiles.xml` and the search falls through to A's.
- An entry that is missing, or shows a raw `PROBE_G…` key, is a result in itself. Note it.

If the game crashes at startup, report which launch it was. The likely trigger is a listed
campaign file that no tier supplies, which is G8 by design. Then repeat that launch after
deleting `ProbeA\Data\ProbeD3.meg` and removing its line from `ProbeA\Data\MegaFiles.xml`.

**Send back:** the screenshots from each launch. Also send any `*.txt` or `*.log` in
`corruption\` with today's timestamp, for example `LogFile.txt`, if one exists.

## Cleanup (puts everything back)

1. Delete `GAME\corruption\Data\XML\Probe_G1.xml`, `Probe_G2.xml` and `Probe_G3.xml`. If `XML`
   was created by you and is now empty, delete it too.
2. Delete `GAME\GameData\Data\XML\Probe_G1.xml` … `Probe_G5.xml`. If you created `XML` and it is now empty, delete it as well.
3. Delete `GAME\corruption\Mods\ProbeA`, `ProbeB` and `ProbeF`. If `Mods` was created by you, delete it as well.

None of the probe files share a name with a retail file, so deleting them restores the
original state exactly.

## Not covered

- **Lua.** No Lua runs at the main menu or in the GC list. Lua only runs once a mission or
  campaign has started (story, AI and GameObject scripts), and none of them gives on-screen
  output you could read reliably without playing. A Lua tag would need a mission launch and a
  script whose message reaches the screen, which costs several launches and adds a second
  variable. The XML/text probe already answers file resolution. If RE confirms that Lua files
  go through the same resolver, no Lua probe is needed.
- **STEAMMOD.** It needs a subscribed Workshop item. To test it, add
  `STEAMMOD=<id>` before `MODPATH=Mods\ProbeA` in L1 and see whether `[A]` and G1 = A
  still hold.

## Rebuild

```
python3 -I tools/probe_mod/build_probe.py <extracted/foc root> <corruption/Data/Text/MasterTextFile_English.dat> tools/probe_mod/v2
```

Before writing anything, the builder checks that its `.dat` writer reproduces the retail
file byte for byte.

---

# Probe E: does FALLBACKPATH chain or replace? (`tools\probe_mod\probeE\`)

This probe settles A3 in `docs\re\SUBSYSTEMS.md` in one launch. It uses the same
Galactic Conquest list-entry technique as above. `Mods\ProbeE` overrides
`CampaignFiles.xml` to list `Probe_E1.xml` and `Probe_E2.xml`:

| File | Where it exists | Tag |
|---|---|---|
| `Probe_E1.xml` | **only** `GameData\Data\XML` | `PROBE E1-GAMEDATA` |
| `Probe_E2.xml` | the FALLBACKPATH dir `Mods\ProbeE_FB\Data\XML` **and** `GameData\Data\XML` | `PROBE E2-FB` / `PROBE E2-GAMEDATA` |

Neither file name exists in FoC loose files, in any MEG, or in any MODPATH directory.
A missing campaign file is skipped silently, as the v2 runs showed.

**Install** (`GAME` = `F:\SteamLibrary\steamapps\common\Star Wars Empire at War`):
1. Copy `probeE\Mods\ProbeE` and `probeE\Mods\ProbeE_FB` into `GAME\corruption\Mods\`.
2. Copy `probeE\OPTIONAL_copy_into_GameData_Data\Data\XML\Probe_E1.xml` and `Probe_E2.xml` into `GAME\GameData\Data\XML\`.
   - Create the `XML` folder if it doesn't exist. Stock EaW has no `GameData\Data\XML`.
   - Both are new files and overwrite nothing.

**Launch** (single):
```
cd /d "F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
StarWarsG.exe MODPATH=Mods\ProbeE FALLBACKPATH=Mods\ProbeE_FB
```
Open Main menu (buttons show an `[E]` prefix) → Single Player → Galactic Conquest, take a
screenshot of the `PROBE E…` entries, then quit.

| Result | E1 entry | E2 entry |
|---|---|---|
| **Chain** (static prediction) | `PROBE E1-GAMEDATA` | `PROBE E2-FB` |
| **Replace** | missing | `PROBE E2-FB` |

Possible deviations:
- `E2-GAMEDATA` means GameData is searched before FALLBACKPATH.
- No E entries at all means the ProbeE mod didn't load.
- Getting **replace** means something at runtime changes the fallback list, for example one of the eaw_* patch DLLs. Repeat the launch without those DLLs.

**Cleanup:**
1. Delete `GAME\GameData\Data\XML\Probe_E1.xml` and `Probe_E2.xml`. Delete the `XML` folder too if you created it.
2. Delete `GAME\corruption\Mods\ProbeE` and `ProbeE_FB`.

Rebuild: `python3 -I tools/probe_mod/build_probe.py probeE <extracted/foc root> <MasterTextFile_English.dat> tools/probe_mod/probeE`

---

# A1 tactical-AI probes (`tools\probe_mod\a1_tactical\`)

Two probes for roadmap A1, one game session: **free-store timing** (A1 t02: do `FreeStoreService` /
`On_Unit_Service` first run at mode frame 600 / 63, AI_TACTICAL §5.3 and §12 item 10) and **landing-point
ownership** (A1 t04: does the campaign attacker own its landing reinforcement point at battle start, and what is its
land cap +0x404). Double-click `a1_tactical\run_a1.cmd` (or `freestore_timing\run_freestore.cmd` /
`landing_points\run_landing.cmd`), play skirmish land Utapau and space Kuat (90 s each) and the campaign
"S10 PROBE A: TATOOINE" as Empire (fight the invasion, 60 s), giving no orders; send back
`a1_tactical\out\<run>\` (`report.txt` first). Cleanup is automatic (`cleanup_a1.cmd` after a crash).
Full instructions and how to read the result: `a1_tactical\README.md`.

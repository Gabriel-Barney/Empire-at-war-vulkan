# modc t09: play the second mods in the port

Roadmap group `modc`, task t09 ("Gabriel: run the second mod in game and report what breaks"). t06 picked two installed
workshop mods that are not Thrawn's Revenge and checked them headless (`docs/MODS.md` §9, "Second mod"). This run is the
part only a person can do: play them in `alamo_game` and look at what the headless checks cannot see (rendering, UI,
text, sound, how the AI plays). Nothing here drives the game for you and nothing writes into the game or mod folders.

| Mod | Workshop id | What it is |
|---|---|---|
| EAW-Remake (Empire at War Remake) | 1770851727 | Total conversion by its own team: Rebel / Empire / Underworld + Hutts and minor factions, own MEGs and shaders |
| Star Wars: Age of Legends (New Jedi Order era) | 2858787558 | Era mod: New_Republic, Galactic_Empire, Vong and ~45 minor factions, loose XML, story files with their own root tag |

## How to start

From the repository root, after a normal `build_msvc.cmd` (or set `ALAMO_BIN` to a folder with `alamo_game.exe`):

```
tools\probe_mod\modc_t09\run_t09.cmd check  1770851727    load report only, no window (out\<time>-<id>-report.txt)
tools\probe_mod\modc_t09\run_t09.cmd port   1770851727    the port with the mod (log: out\<time>-<id>-alamo_game.txt)
tools\probe_mod\modc_t09\run_t09.cmd retail 1770851727    the original with the same mod (Steam must be running)
```

Same with `2858787558`. Instead of the command line you can also start `alamo_game` without a mod and use the main
menu's **Mods** button (`docs/MODS.md` §5.2): select the mod's row, wait for its report in the right pane, then
**Launch**. Both ways give the same command line (`STEAMMOD=<id>`).

The retail run is only for comparison: when something looks wrong in the port, check whether the original does the same
before reporting it.

## What to look at (each mod, about 20 minutes)

1. **Main menu** (2 min): the mod's menu background, fonts and text (no raw `TEXT_...` keys), music. The Mods dialog row
   says "(running)" for this mod. Known (modc t34): on the Remake the hidden smoke saw "Single Player" drawn as a heading
   with no button, and its click did nothing; on Age of Legends the skirmish setup's Land / Space list did not offer
   Space. Say what the original shows there, and whether Single Player / Space work when you click them.
2. **Space skirmish** (5 min): Skirmish > Space, any map of the mod (the Remake's own maps are on the Custom tab too),
   you vs one AI. Look at: the buy screen (only the mod's units, sensible costs), ship models and textures (missing =
   invisible or pink / green), engine glows and shields, weapons firing, the AI building and attacking within a few
   minutes, no freeze or crash.
3. **Land skirmish** (5 min): Skirmish > Land, any map. Look at: starting units, terrain, infantry / vehicle models and
   animations, build pads, the AI capturing pads and attacking.
4. **Galactic conquest** (8 min): Remake: "GC Mini Web Way" (any faction). Age of Legends: a "Balance Point" sandbox (New
   Republic or Vong). Look at: the galaxy map (planets, trade routes, icons), planet info, building and moving a fleet,
   the AI factions taking turns (planets changing owner after a few minutes at normal speed), one space battle (fight it
   or auto-resolve), the story / event pop-ups.

Known from the headless runs (`docs/MODS.md` §9), so not worth reporting again: Age of Legends' 95 story scripts are
32-bit bytecode (they do not run in the original either), two of its AI scripts start with a UTF-8 BOM (same), its
`Try_Weapon_Switch` / unknown filter Lua errors are in the mod's own scripts; the Remake's land factions have no
`Land_Skirmish_AI_Default_Forces`, so the port starts them with their three cheapest units ([port] fallback, the original
places nothing); Remake space battles on maps with very large asteroid fields run slowly in the port (about 26 ms a sim
tick headless, modc t33): say how slow it feels, but it is known.

## Where to report

Write what you saw in a reply to the modc coordinator session (or on the roadmap, group `modc`): per mod and per step
one line "OK" or what broke, with a screenshot for anything visual, and the `out\<time>-<id>-alamo_game.txt` log of a run
that crashed or misbehaved. For a crash, also say what you clicked last. Then delete `out\` (it holds only logs, the
effect cache and saves).

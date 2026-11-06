# Synthetic mod fixture (modc t07, t12, t15, t16)

A minimal stand-in for an FoC install (`corruption/Data`) and mods in the retail place for local mods
(`corruption/Mods/<Name>`), loaded by `alamo_game_mod_fixture_tests` and `alamo_mod_check_fixture` through the real
`MODPATH=Mods\<Name>` route (relative to the FoC folder, as retail resolves it; `docs/MODS.md` §3.1). No game data.
Read only: the tests never write here.

- `FixtureMod` (t07) adds a faction (`Mod_Alliance`), a space unit (`Mod_Corvette`, with its model), a planet
  (`Mod_Prime`) and a campaign (`Mod_GC`) by replacing the base list files, and overrides the base Lua library
  `FixtureLib`; its story script errors at load unless `require` found the mod's copy. Its load report must come out with
  no errors, no missing files, no unknown XML tags, no Lua errors and no submod hint.
- `FixtureChecks` (t12, t15) carries findings on purpose: one tag no reader knows in each of FactionFiles, CampaignFiles,
  GameConstants and SpeechEvents (next to tags the original's tables or the port's loaders do read); a campaign with no
  Locations next to one with planets (a menu entry, a warning: retail lists such campaigns); a scoring script that
  sets the GlobalValue `CHECKS_MOD_ID` only in `Base_Definitions`, a GameObject script whose main chunk fails on that
  unset value (session-dependent), and one whose main chunk fails on a key no script sets (a real run error).
- `FixtureSub` (t16) is a submod of `FixtureMod`: its lists name `ModObjects.xml` and `ModFactions.xml`, which only
  `FixtureMod` ships. Its `modinfo.json` names workshop item `900000001` of the fake Steam library `steamlib/` (the
  `<library>\steamapps\workshop\content\32470\<id>` layout), which has only `ModFactions.xml`: the report must rank
  `FixtureMod` first and stack `MODPATH=Mods\FixtureSub MODPATH=Mods\FixtureMod`.

# Synthetic mod fixture (modc t07)

A minimal stand-in for an FoC install (`corruption/Data`) and one mod in the retail place for local mods
(`corruption/Mods/FixtureMod`), loaded by `alamo_game_mod_fixture_tests` and `alamo_mod_check_fixture` through the real
`MODPATH=Mods\FixtureMod` route (relative to the FoC folder, as retail resolves it; `docs/MODS.md` §3.1). No game data.

The mod adds a faction (`Mod_Alliance`), a space unit (`Mod_Corvette`, with its model), a planet (`Mod_Prime`) and a
campaign (`Mod_GC`) by replacing the base list files, and overrides the base Lua library `FixtureLib`; its story script
errors at load unless `require` found the mod's copy. The load report must come out with no errors, no missing files, no
unknown XML tags and no Lua errors. Read only: the tests never write here.

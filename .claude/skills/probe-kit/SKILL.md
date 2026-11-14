---
name: probe-kit
description: Prepare an in-game probe kit under tools/probe_mod/<kit>/ to answer a question about the original game (retail FoC) - mod payload builder, Lua probe script, run / cleanup scripts, analyser, README, PROBES.md index entry - so Gabriel can run it. Use whenever a roadmap task needs a retail check, says "probe", "needs Gabriel" or "retail run", when a docs/re spec has an unconfirmed guess that only the running game can settle, or when asked to build, extend or rerun a probe kit.
argument-hint: <kit name, e.g. s78n_t08> <question>
---
# In-game probe kits

Some questions about the original game can only be answered by running it: timings, ranges, which branch a rule takes. A probe
kit asks one such question in a way that is cheap for Gabriel to run and leaves his install exactly as it was. Rule 7 in
`CLAUDE.md`: probes need Gabriel to launch the game, so the kit must be ready to run end to end, and you ask before driving the
game yourself. Never drive the port's own `alamo_game.exe` with computer use (Gabriel declined that, 2026-11-09).

Look at a recent kit before writing a new one and copy its shape; `tools/probe_mod/s78n_t08/` and `tools/probe_mod/s78n_t14_squads/`
are good models. Index of every kit: `docs/PROBES.md`.

## 1. Pin the question first

Write down, before any code: the exact question, the port's current guess and where it lives (`docs/re/<SPEC>.md` section), and
what result confirms or refutes it. A kit that answers "what happens?" without a prediction is hard to read afterwards. Prefer
one launch that answers several sub-questions over several launches.

## 2. Kit layout (`tools/probe_mod/<kit>/`)

| File | Role |
|---|---|
| `README.md` | the question, predictions with numbers, how to run (exact menu steps: mode, map, factions, slots), how long it takes, how to read the result, cleanup |
| `run_<kit>.cmd` | entry point; calls `toolchain\venv\Scripts\python.exe -I <kit>_run.py %*` and passes on the usual flags (`--non-interactive`, `--no-launch`, `--windowed`, `--dry-run`, `--no-profile`, `--game DIR`, `--report-only RECORDS.log`) |
| `<kit>_run.py` | backs up the player profile, builds the mod into the install, waits for the game, scans memory for probe records, writes `out\<run>\report.txt`, cleans up, restores the profile; uses `tools/probe_mod/common/probe_run.py` and `launch_foc.py` instead of copying helpers |
| `build_<kit>_mod.py` | builds `Mods\<Name>Probe` from the user's own install (never ship retail files in git) |
| `<kit>_probe.lua` | the probe script: writes greppable ASCII records into memory; compiled with `common/lua50.py` (Lua 5.0, no math lib in free-store Lua) |
| `<kit>_analyse.py` | turns the records into PASS / FAIL lines against the README's predictions |
| `cleanup_<kit>.cmd` / `<kit>_cleanup.py` | puts the install back exactly (uses `common/probe_cleanup.py`) |
| `selftest_<kit>.py` | offline check of the builder and analyser (no game needed) |

Payload folders and `out/` are not committed (the landing check refuses generated files).

## 3. Things earlier kits learned the hard way

- Launch FoC **windowed**; it pauses on focus loss, and a small window can hide HUD elements a probe needs.
- Run long kits detached (a normal PowerShell / cmd window), not in a Claude terminal tab, so the game is not a child of Claude's shell.
- An engine call that fails inside retail Lua kills the script silently; guard each call and write a record before and after it.
  `Activate_Ability` with no argument, a bool or a position kills retail scripts.
- If FoC exits silently at launch, Steam's stale `ActiveProcess` pid is the usual cause: restart Steam.
- Set the state the probe depends on explicitly (facing, position, locomotor suspended) rather than hoping the game reached it.

## 4. Before handing it over

Run the selftest and `--dry-run`. Add a row to `docs/PROBES.md` (kit, question, where findings will go). Put the run command and
expected duration on the roadmap task, then ask Gabriel to run it (or ask whether you may drive retail FoC with computer use).

## 5. After the run

Findings go into the `docs/re/` spec the README names (a `<TASK>_RUN.md` page for a long run), never only in the kit README;
a decision that follows goes in `docs/DECISIONS.md`; the roadmap task records the run id and outcome. Update the `docs/PROBES.md`
row with the run date and result.

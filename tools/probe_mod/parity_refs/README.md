# Parity reference capture (`tools\probe_mod\parity_refs\`)

Roadmap group `q4d`. The parity harness (`tools\parity\README.md`) can now judge random-driven outcomes by
distribution (parity tier 2, `docs/DECISIONS.md` "Parity tiers"): the port over 10 seeds against every retail run of a
scenario, per-metric means and spreads. Its means are only as good as the retail sample, and the spreads are graded only
from 5 retail runs per scenario on (`tools\parity\scenarios.json`, `distribution`). Today there are 4 usable Heart of
the Maw runs and 3 Geonosis runs. This kit records more, hands-off, in one sitting.

It adds nothing of its own to the game: every run is an ordinary Q2 trace (`tools\probe_mod\q2_traces\run_trace.ps1
-NonInteractive -LuaProbe`), which drives the menus, records with the read-only `q2reader.exe`, closes the game and cleans
up after itself (`tools\probe_mod\q2_traces\README.md`). `run_capture.ps1` repeats it and lists the results.

## Steps for Gabriel

About 1 h 50 min for the default 6 + 6 runs. Nothing to click once it runs.

1. **One time only**, if `tools\probe_mod\q2_traces\bin\q2reader.exe` is missing: double-click
   `tools\probe_mod\q2_traces\build_reader.cmd`.
2. Start Steam. Close the game if it is open.
3. Double-click **`run_capture.cmd`**. It runs 6 Galactic Conquest runs (Heart of the Maw, Empire, 10 fiscal cycles,
   about 12 min each), then 6 space skirmishes ((2) Battle Over Geonosis, Empire vs Medium AI Rebel, 5400 ticks, about
   6 min each). Each run starts the game in a window, drives the menus, and closes the game when its trace is complete.
   **Do not touch the game window or move the mouse over it** while a run is going (the menu driver clicks in it, and
   FoC pauses when it loses focus). Fewer or one scenario: `run_capture.cmd -Scenario skirmish -Runs 3`.
4. At the end the window says how many runs were complete and waits for a key. Tell Claude it is done (or zip
   `tools\probe_mod\q2_traces\out\` without `selftest`, and `tools\probe_mod\parity_refs\out\`, and send them).
5. If the window was closed early or something looks stuck: double-click **`cleanup.cmd`** (safe to run twice).

The batch stops at the first run that does not complete (`-KeepGoing` carries on). A run that stops at once usually means
Steam's record of the last game process is stale: restart Steam and start again.

## Options

| option | default | what |
|---|---|---|
| `-Scenario galactic\|skirmish\|both` | both | which scenarios (galactic first) |
| `-Runs N` | 6 | runs per scenario |
| `-NoLuaProbe` | off | plain traces; by default the Lua probe is on, as for the newest reference runs |
| `-DryRun` | off | print what each run would do, start nothing |
| `-PauseSec N` | 20 | pause between runs |
| `-KeepGoing` | off | go on after a run that did not complete |

Exit codes: 0 every run complete, 10 a run partial or failed, 20 preflight failed (nothing started), 40 cleanup left
something in the game folder (run `cleanup.cmd`).

## Output and what happens next

- `tools\probe_mod\q2_traces\out\<run>\`: one Q2 run each (trace, summary, run status, Lua records), as for any Q2 run.
- `out\<stamp>\batch.json`: every run of the batch with its scenario, exit code and reason.
- `out\<stamp>\add_refs.cmd`: adds the complete runs to `tools\parity\scenarios.json`
  (`parity_harness.py add-refs --scenario <id> <run> ...`, which checks each run's `trace.q2t` and keeps the file's
  layout).

Then (Claude): run `add_refs.cmd` in the main checkout, check the new runs with `parity_harness.py retail` (leave-one-out:
a run far from the others is a kit or menu problem, not spread), re-record `tools\parity\baseline.json` and
`tools\parity\baseline_seeds.json` (`tools\parity\README.md`), and say in the commit that the references changed.

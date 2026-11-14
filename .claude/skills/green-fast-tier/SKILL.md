---
name: green-fast-tier
description: Drive the build and fast-tier tests to green without babysitting - build the changed modules, read the summary, fix the real failure, rebuild, repeat until 0 warnings and 0 real failures. Use when asked to "green the build", "fix the failing tests", "loop until it passes", or after a large edit that is likely to break compile or tests. Pairs with /loop for unattended runs.
argument-hint: "[modules, e.g. core;tactical] [max-iterations, default 6]"
---
Get this worktree's build and fast-tier tests green. `$ARGUMENTS` is the module list (default: the engine modules changed against
`origin/dev`, plus their dependencies) and an optional iteration cap (default 6).

Each iteration:

1. **Build and test.** `& .\build_agent.cmd --% msvc wt "<modules>" changed` while iterating (one tree per worktree; nothing after
   the `--%` command on the line). Read only the summary it prints (`tools\lint\build_summary.py`), never the whole log.
2. **Classify** with the summary's own split:
   - warnings or compile errors: fix the code (zero warnings at MSVC `/W4 /permissive-`; explicit narrowing casts, no shadowing);
   - `real failures`: read the failing test's output, fix the cause in the code;
   - `golden drift` only: **stop**. Do not regenerate or edit goldens to get green. Report which goldens moved and ask whether the
     behaviour change was intended (`regen-goldens` skill is the only way to change them);
   - skipped data tests (`skips:`): say so, they prove less.
3. **Change something before rerunning.** An identical failing rerun is refused (exit 3). `ALAMO_RERUN=1` only for a flaky test or a
   changed environment, and say so when you use it. A flaky sim test while other lanes build: rerun with `ctest -j 2` first.
4. **Stop conditions:** green (0 warnings, 0 real failures); the iteration cap; the same test failing three iterations in a row with
   different fixes (the cause is not understood: stop and report what you tried); a failure outside this group's files (record it
   with `roadmap_store.py backlog add ... --from <group>` unless it is a crash, hang, save corruption or tier-1 difference).

When green on the changed tier: run the whole fast tier once (`& .\build_agent.cmd --% msvc wt "<modules>"` with no 4th argument),
and clang too when sim code changed. Report: iterations used, what each fix was, final summary line, `skips:`, and anything left.

## Unattended use

Run it as `/loop /green-fast-tier <modules>` (omit the interval so it self-paces: a build takes minutes, so wake-ups should be at
least that far apart). End the loop (stop) as soon as a pass is green or a stop condition above is hit. Do **not** use `/loop` to
poll CI or PR state: the app's Auto-fix already does that.

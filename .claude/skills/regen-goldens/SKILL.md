---
name: regen-goldens
description: Decide whether a failing golden CRC test is an intended behaviour change, and if so regenerate the goldens the right way (tools\regen_goldens.cmd, test-owned --regen, benchmark scenarios) and record it. Use whenever a build summary shows "golden drift", a golden / CRC / checkpoint test fails after a sim change, someone asks to regenerate, update or refresh goldens, or a land_on_dev merge conflicts in testdata/*.crc files.
argument-hint: [build tree, default build-wt-msvc]
---
# Regenerating goldens

Golden CRC files pin the simulation's per-tick results. They are how this project notices that behaviour moved, so a regenerated
golden is a claim that the move was intended. Editing one just to turn a test green hides a real regression from every later
lane, which is why the rule is strict: regenerate only for an intended change, only through the tools, and say so in the commit.

Background and file format: `docs/status/golden-checkpoints.md`. Build summary rules: `docs/BUILD.md` ("Golden drift in the summary").

## 1. Is it drift or a real failure?

Read the summary that `tools\lint\build_summary.py` printed at the end of the build. It splits failed tests into
`real failures` and `golden drift`.

- **Real failure** (crash, timeout, a failed CHECK, `differs from run N`, a `_w0.crc` worker-count golden, `workers N vs 0`):
  this is a determinism or logic bug, never a golden to regenerate. Fix the code. Worker-count and run-to-run differences break
  rule 3; the `determinism-check` skill covers them.
- **Golden drift** (only stored goldens differ): continue, but first answer: *did the change in this branch mean to change sim
  behaviour?* A refactor, a render / UI change, or a tool change should not move a sim golden. If one moved anyway, treat it as a
  bug and find out why.

The failure message names the first bad checkpoint, its tick window and the differing system columns. Check that the system that
moved is the one the change touched (a movement change that moves the `economy` column is suspicious).

## 2. Regenerate (intended change only)

Commit the code first: `regen_goldens.cmd` refuses to run with uncommitted tracked edits (unless `--force`), so the goldens land
in their own commit.

| Golden kind | How |
|---|---|
| headless goldens (`engine/apps/headless/testdata/*.crc`) | `tools\regen_goldens.cmd --build-dir build-wt-msvc --only headless` |
| game smoke goldens (`engine/apps/game/testdata/game_smoke_*`) | `--only game` (needs the GPU and the game data) or `--only all` |
| benchmark scenarios (`tests/benchmark/*.abench`) | remade by the same script when stale; `--bench-check` only reports, `--no-bench` skips |
| test-owned goldens (apps/sim `alamo_sim_tests`, combat / abilities / galactic module goldens) | the test exe's own `--regen` / `--write-golden` option; then `python tools/goldens/convert_goldens.py --in-place --check <files>` to thin them to checkpoints |

Never write a golden by hand or copy one from another tree. Use your own worktree's build tree (`build-wt-msvc`), not `build-msvc`.

Read the diff summary it prints (files changed, first changed checkpoint, differing columns) and check it matches the intended
change. Then rebuild and rerun the fast tier: every golden test should pass.

## 3. Clang must agree

Sim goldens are compared across compilers. Run `& .\build_agent.cmd --% clang wt "<modules>"`: clang must reproduce the MSVC
goldens exactly. If it does not, the change has a compiler-dependent result (FMA contraction, uninitialised read, unordered
iteration): fix the code, do not regenerate again.

## 4. Record it

The goldens commit message names every changed golden file and why it moved (one line each is enough), e.g.
`goldens: skirmish_ai_foc_golden.crc moved from checkpoint 1792 (movement column): fleet cohesion cap changes arrival ticks`.
Remade benchmark scenarios: say "scenario regenerated". The landing commit repeats "goldens changed: ..." (or "none").

## Merge conflicts in goldens

When `tools\land_on_dev.cmd` hits a conflict only in goldens that `regen_goldens` writes, it resolves them itself
(`tools\lint\golden_merge.py`: take `dev`'s, build, regenerate). For a test-owned golden or a conflict mixed with other files it
stops: take `dev`'s version of the golden, finish the merge, rebuild, and regenerate as in step 2.

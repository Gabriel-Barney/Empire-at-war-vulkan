---
description: Finish a roadmap group - tests, roadmap write (group-done and box-done rule), render, docs, land on dev
---
Finish roadmap group `$ARGUMENTS` in this worktree. Work through the checklist and report each line as done or not applicable:

1. **Tests.** `& .\build_agent.cmd --% msvc wt "<modules>"` (fast tier) with 0 warnings; clang too when sim code changed
   (`& .\build_agent.cmd --% clang wt "<modules>"`); `build_asan.cmd` when `engine/jobs` or other threading code changed.
   Read `skips:`. Broad changes: `tools\pr_check.cmd --compiler both`. Do not edit goldens to get green; if a golden moved on
   purpose, `tools\regen_goldens.cmd`, review the diff, and name it in the commit message.
2. **Roadmap.** Sync first (`python roadmap-store\roadmap_store.py sync`), re-read `roadmap-store\data\items\<id>.json`, then
   `write` one file: every finished task `s: done`, plus `updated` and `by`. Re-read the group: **no `todo` task left means
   `{"status":"done","preset":{"__delete__":true}}` in that same write**. If it was the last open group of its flowchart box
   (check the other `items` with the same `node`), also `update` `nodes/<ID>` `{"status":"done"}`. If the group's `exit` test passed
   but tasks are still open, do not keep it open and do not make a follow-up group: run
   `python roadmap-store\roadmap_store.py close-group <id> --by <session> --passed "<evidence>"` (open tasks go to the backlog and
   are filed into categories; `--drop tNN --why "..."` for ones not worth keeping; it sets the group and, when last, its box done).
   If the exit test has not passed, say so and leave the group open. Findings from this work that are not in the group go in with
   `roadmap_store.py backlog add "<text>" --by <session> --from <id>`. Then `roadmap_store.py lint`. One `log` line.
   (Shapes: `docs/ROADMAP_PROTOCOL.md`.)
3. **Sync.** `python roadmap-store\roadmap_store.py sync` should print `in sync with origin/main`. The roadmap lives only in
   `roadmap-store\` since 2026-11-06 (PR #89): nothing about it is committed to this repository.
4. **Docs.** Record results where `docs/README.md` says (decisions, measurements, build rules, `docs/re/` pages the tasks name); status is only on the roadmap. Keep edits minimal and factual.
5. **Commit and land on `dev`.** `git add` the work and commit; the message lists changes, build and test results per compiler,
   skips, goldens changed or "none", tasks closed, whether the group and box are now `done`, open items, and ends with
   `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Then `tools\land_on_dev.cmd` (merges `origin/dev` in, fast-forward
   pushes `dev`, deletes the branch on origin; rerun if `dev` moved). No PR: `main` is released from `dev` by Gabriel.
6. **Release.** If you are stopping with tasks left: `python roadmap-store\roadmap_store.py release <id> --by <session>`.
   Once the work is on `dev`, the worktree goes with `tools\clean_worktrees.cmd --apply --only <name>`.

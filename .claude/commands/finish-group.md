---
description: Finish a roadmap group - tests, roadmap write (group-done and box-done rule), render, docs, PR
---
Finish roadmap group `$ARGUMENTS` in this worktree. Work through the checklist and report each line as done or not applicable:

1. **Tests.** `& .\build_agent.cmd --% msvc wt "<modules>"` (fast tier) with 0 warnings; clang too when sim code changed
   (`& .\build_agent.cmd --% clang wt "<modules>"`); `build_asan.cmd` when `engine/jobs` or other threading code changed.
   Read `skips:`. Broad changes: `tools\pr_check.cmd --compiler both`. Do not edit goldens to get green; if a golden moved on
   purpose, `tools\regen_goldens.cmd`, review the diff, and name it in the PR.
2. **Roadmap.** Sync first (`python roadmap-store\roadmap_store.py sync`), re-read `roadmap-store\data\items\<id>.json`, then
   `write` one file: every finished task `s: done`, plus `updated` and `by`. Re-read the group: **no `todo` task left means
   `{"status":"done","preset":{"__delete__":true}}` in that same write**. If it was the last open group of its flowchart box
   (check the other `items` with the same `node`), also `update` `nodes/<ID>` `{"status":"done"}`. Leftovers: a new follow-up group
   with `preset`, `eta`, `after`, and a `why` that names the preset. One `log` line. (Shapes: `docs/ROADMAP_PROTOCOL.md`.)
3. **Sync.** `python roadmap-store\roadmap_store.py sync` should print `in sync with origin/main`. The page lives only in
   `roadmap-store\ROADMAP.html` since 2026-11-06 (store commit 70f324a, PR #89): `ROADMAP.html` / `docs\ROADMAP.html` in this
   repository are no longer regenerated, so leave them out of the commit.
4. **Docs.** `docs/PHASE4.md` status line and decisions; the phase docs and `docs/re/` pages the tasks name; keep edits minimal and factual.
5. **Commit and PR.** `git add` the work plus the two pages, commit (end the message with
   `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`), `git push -u origin <branch>`, and `gh pr create --base main`
   (or the branch you are stacked on). The PR text lists: changes, build and test results per compiler, skips, goldens changed or
   "none", tasks closed, whether the group and box are now `done`, open items. End with
   `🤖 Generated with [Claude Code](https://claude.com/claude-code)`. Do not merge it.
6. **Release.** If you are stopping with tasks left: `python roadmap-store\roadmap_store.py release <id> --by <session>`.
   When the PR is merged, the worktree goes with `tools\clean_worktrees.cmd --apply --only <name>`.

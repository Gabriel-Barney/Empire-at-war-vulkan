---
name: finish-group
description: Finish a roadmap group - tests, code review of the branch diff, roadmap write (group-done and box-done rule), docs, commit and land on dev with tools\land_on_dev.cmd. Use whenever a group's work is done, when asked to "finish", "close", "wrap up" or "land" a group, or as the last step of a lane.
argument-hint: <group-id>
---
Finish roadmap group `$ARGUMENTS` in this worktree. Work through the checklist and report each line as done or not applicable:

1. **Tests.** `& .\build_agent.cmd --% msvc wt "<modules>"` (fast tier) with 0 warnings; clang too when sim code changed
   (`& .\build_agent.cmd --% clang wt "<modules>"`); `build_asan.cmd` when `engine/jobs` or other threading code changed.
   Read `skips:`. Broad changes: `tools\pr_check.cmd --compiler both`. Do not edit goldens to get green; if a golden moved on
   purpose, `tools\regen_goldens.cmd`, review the diff, and name it in the commit message.
2. **Review.** Commit the work first (a WIP commit is fine), then review this branch's diff against `origin/dev` before
   anything lands:
   - Invoke the `code-review` skill at `high` on the branch (e.g. `/code-review high`). `max` and `ultra` only when Gabriel asks.
   - Sim code changed (everything in `engine/` except render, audio, video, UI): also spawn the `reviewer` agent with the changed
     files and the `docs/re/` specs they cite, for the determinism and thread-safety read (`determinism-check` skill lists the rules).
   - Fix each confirmed correctness finding on the branch and rerun the step-1 builds for the modules it touched. A finding that does
     not block the exit test goes to the backlog (`roadmap_store.py backlog add ... --from <id>`, Stage 6 groups: a new task), and a
     finding you judge wrong is reported as such, not silently dropped. Report the review as done with the count fixed / filed / rejected.
3. **Roadmap.** Sync first (`python roadmap-store\roadmap_store.py sync`), re-read `roadmap-store\data\items\<id>.json`, then
   `write` one file: every finished task `s: done`, plus `updated` and `by`. Re-read the group: **no `todo` task left means
   `{"status":"done","preset":{"__delete__":true}}` in that same write**. If it was the last open group of its flowchart box
   (check the other `items` with the same `node`), also `update` `nodes/<ID>` `{"status":"done"}`. If the group's `exit` test passed
   but tasks are still open, do not keep it open and do not make a follow-up group: run
   `python roadmap-store\roadmap_store.py close-group <id> --by <session> --passed "<evidence>"` (open tasks go to the backlog and
   are filed into categories; `--drop tNN --why "..."` for ones not worth keeping; it sets the group and, when last, its box done).
   If the exit test has not passed, say so and leave the group open. Findings from this work that are not in the group go in with
   `roadmap_store.py backlog add "<text>" --by <session> --from <id>`. **Stage 6 · Parity and mods (boxes MODC, Q4, Q5, Q6) keeps
   its follow-ups** (Gabriel, 2026-11-12): there that command adds each finding as a new task of the group (tier 3 still goes to the
   backlog), and the group is closed only when no task is open: finish or `--drop` each (`close-group` refuses otherwise); if some
   are still wanted, leave the group open with them. Then `roadmap_store.py lint`. One `log` line.
   (Shapes: `docs/ROADMAP_PROTOCOL.md`.)
4. **Sync.** `python roadmap-store\roadmap_store.py sync` should print `in sync with origin/main`. The roadmap lives only in
   `roadmap-store\` since 2026-11-06 (PR #89): nothing about it is committed to this repository.
5. **Docs.** Record results where `docs/README.md` says (decisions, measurements, build rules, `docs/re/` pages the tasks name); status is only on the roadmap. Keep edits minimal and factual.
   A `research` report from the group is in its home and cited from the task or `docs/re/` spec that used it. A skill, agent or
   `CLAUDE.md` text you changed passes the `writing-for-agents` review checklist. A bug fixed through `diagnosing-bugs` has its
   regression test (or the filed missing-seam finding) and a commit message naming the right hypothesis.
6. **Commit and land on `dev`.** `git add` the work and commit; the message lists changes, build and test results per compiler,
   skips, goldens changed or "none", tasks closed, whether the group and box are now `done`, open items, and ends with
   the `Co-Authored-By:` line the session's attribution reminder gives. Then `tools\land_on_dev.cmd` (merges `origin/dev` in, fast-forward
   pushes `dev`, deletes the branch on origin; rerun if `dev` moved). No PR: `main` is released from `dev` by Gabriel.
7. **Friction.** Put the lane's one `friction` line (runs wasted and what would have prevented them, or "none") in the final report.
   When the group had three or more repeat-refused or identical failing runs, or the same prompt or search came up again and
   again, say so and suggest Gabriel runs `/retro <id>`; do not run it yourself.
8. **Release.** If you are stopping with tasks left: `python roadmap-store\roadmap_store.py release <id> --by <session>`.
   Once the work is on `dev`, the worktree goes with `tools\clean_worktrees.cmd --apply --only <name>`.
8. **Context.** A session that goes on to another group in the same worktree runs `/compact` first, keeping the landed commit, the
   open items and the next group's id, and dropping build logs and diffs; or starts a fresh session (preferred when the next group
   touches other modules). Clear the `/goal` set by `start-group` once the exit test has passed.

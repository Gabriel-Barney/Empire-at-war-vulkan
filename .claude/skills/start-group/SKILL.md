---
name: start-group
description: Start a roadmap group (an id such as s78m, a2m, fg1) - sync the roadmap store, show its tasks, exit test and prerequisites, claim it, create or verify the worktree, print the plan. Use whenever work on a roadmap group begins, when asked to "start", "pick up" or "take" a group, or as the first step of a lane.
argument-hint: <group-id> [session-name]
---
Start roadmap group `$ARGUMENTS` (a group id such as `s78m`; add a session name after it to override the default, which is the
worktree name). Follow the `lane` agent loop (`.claude/agents/lane.md`):

1. `python roadmap-store\roadmap_store.py sync` (pulls the store and regenerates its page).
2. Read `roadmap-store\data\items\<id>.json` and its box `roadmap-store\data\nodes\<node>.json`. Show: title, status, preset, `why`,
   every task with its state, `after` (and whether each group in it is `done`), `eta`, and the **`exit` test and `cap`** (a group with
   no `exit` yet: write one first, `update` `items/<id>` `{"exit":"..."}`, since that is how the group is finished). If an `after` group is not done, or the
   group is already `done`, or `by` shows another worker holding it, say so and stop.
3. Claim it: `python roadmap-store\roadmap_store.py claim <id> --by <session>`.
4. Worktree: if this session is already in a `.claude\worktrees\<name>` checkout, verify it: `git status`, `git branch --show-current`,
   and that `Data`, `extracted`, `toolchain`, `roadmap-store` resolve (`dir`); a missing one is re-linked with
   `cmd /c mklink /J <link> <main checkout folder>`. Otherwise, from the main checkout, run
   `tools\new_worktree.cmd <name> --modules "<modules the tasks touch>"` (branches from `origin/dev`) and continue in the folder it
   prints. A session already on its own app branch keeps it: `tools\land_on_dev.cmd` brings `dev` in when it lands.
5. Current focus (Gabriel, 2026-11-10, `docs/DECISIONS.md`): only tier 1 and tier 2 tasks. Skip tier-3 tasks (`tier` field) and say so in the plan.
   Mark each task that needs a retail fact, an unfamiliar technique or an unexplained failure: the first becomes a `docs/re/` lookup
   or a `probe-kit` (Gabriel runs probes), the second a `research` report, the third a `diagnosing-bugs` loop; plan them before the
   code tasks they feed.
   Print the plan: the tasks you will do in order, the modules and docs they touch, which builds apply (MSVC fast tier; clang when
   sim code changes; ASan for `engine/jobs`), and whether goldens may move. Then begin with the first task.

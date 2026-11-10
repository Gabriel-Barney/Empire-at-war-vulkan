---
description: Start a roadmap group - sync, show tasks and prerequisites, claim it, create or verify the worktree
---
Start roadmap group `$ARGUMENTS` (a group id such as `s78m`; add a session name after it to override the default, which is the
worktree name). Follow the `lane` agent loop (`.claude/agents/lane.md`):

1. `python roadmap-store\roadmap_store.py sync` (pulls the store and regenerates its page).
2. Read `roadmap-store\data\items\<id>.json` and its box `roadmap-store\data\nodes\<node>.json`. Show: title, status, preset, `why`,
   every task with its state, `after` (and whether each group in it is `done`), `eta`, and the **`exit` test and `cap`** (a group with
   no `exit` yet: write one first, `update` `items/<id>` `{"exit":"..."}`, since that is how the group is finished). If an `after` group is not done, or the
   group is already `done`, or `by` shows another worker holding it, say so and stop.
3. Claim it: `python roadmap-store\roadmap_store.py claim <id> --by <session>`. If `claim` is not in `roadmap_store.py --help` yet,
   `write` an `update` of `items/<id>` with `{"by":"<session>","updated":"<today>"}` instead.
4. Worktree: if this session is already in a `.claude\worktrees\<name>` checkout, verify it: `git status`, `git branch --show-current`,
   and that `Data`, `extracted`, `toolchain`, `roadmap-store` resolve (`dir`); a missing one is re-linked with
   `cmd /c mklink /J <link> <main checkout folder>`. Otherwise, from the main checkout, run
   `tools\new_worktree.cmd <name> --modules "<modules the tasks touch>"` (branches from `origin/dev`) and continue in the folder it
   prints. A session already on its own app branch keeps it: `tools\land_on_dev.cmd` brings `dev` in when it lands.
5. Print the plan: the tasks you will do in order, the modules and docs they touch, which builds apply (MSVC fast tier; clang when
   sim code changes; ASan for `engine/jobs`), and whether goldens may move. Then begin with the first task.

# Project roadmap: how sessions update it

The main roadmap and flowchart is `ROADMAP.html` in the project root (Gabriel, 2026-10-30; confirmed 2026-10-31 as the project's
local roadmap on this device). It opens in any browser, and in the Claude desktop Browser pane, with no claude.ai sign-in. It is
generated, so never edit it by hand: change the documents in `roadmap-store/` and it is rebuilt, along with the copy in that
repository that other accounts pull. `docs/ROADMAP.html` is its page shell, and it also opens on its own: it carries the same
read-only copy of the data between the `<!--roadmap-data:begin-->` and `<!--roadmap-data:end-->` markers.

`docs/ROADMAP.html` is both the page source and a loadable page. Edit only the shell, which is everything outside the two markers (layout,
script, `PRESETS`). Never edit the block between the markers by hand: `roadmap_store.py` rewrites it on every `write`, `flush`,
`sync` and `render`, so changes to the data go through `roadmap-store/` and the page is rebuilt from it. A shell without the
markers still works: the generator then injects the data into the generated copies only (the old behaviour).

Copies, so nobody edits the wrong one: `roadmap-store/page_shell.html` is the shell with the data block emptied, refreshed from
`docs/ROADMAP.html` on every render (the fallback when the store is cloned alone). `ROADMAP.html` in the project root (the main roadmap) and in
`roadmap-store/` are byte-identical to the loaded `docs/ROADMAP.html`, and
`roadmap-store/ROADMAP.md` is the text form. These and the data block are regenerated on every write, and a diff in them is never a
change to review.

To update, write a JSON list of `{"op", "collection", "doc_id", "data"}` to a scratch file, then run:

```bash
python roadmap-store/roadmap_store.py write <file>
```

`op` is `set`, `update` or `delete`; below, "`update` `items/<id>` with X" means one such entry. To read, open
`roadmap-store/ROADMAP.md` or `roadmap-store/data/<collection>/<id>.json`.

The claude.ai page https://claude.ai/artifact/56dGptBoT3JcDH4rHZ6BjT is retired: do not write to it.

## Collections

| Collection | Document id | Fields |
|---|---|---|
| `meta` | `page` | `eyebrow`, `title`, `lede`, `build` (last verified build line), `updated` |
| `lanes` | short id (`st1`…) | One stage of the plan: `title`, `goal` (what the stage is for), `exit` ("done when" condition), `order`. Stages run in `order`, top to bottom in both the plan and the flowchart |
| `nodes` | box id (`N1`, `MOVE`…) | `label`, `sub`, `lane`, `status`, `shape` (`box`/`pill`), `order`, `to` (map of target box id → `solid` "do before" or `dash` "feeds into") |
| `items` | group id (`s7`, `n1`…) | `title`, `tag`, `node` (box id), `status`, `preset`, `mod`, `why`, `order`, `after` (list of group ids that must finish first), `updated` (date), `by` (session name), `tasks`, `eta`; claim fields, all optional, set by `claim` (below): `claimed_by` (session), `claimed_at` (ISO time), `touches` (list of module folders/files the work edits; default: `mod` split on `·` `,` `;`) |
| `log` | `yyyymmdd-hhmmss-<session>` | `at` (ISO time), `by`, `text` |
| `builds` | `yyyymmddhhmmss-<machine>-<tree>-<compiler>` | `at`, `by`, `machine`, `compiler`, `tree`, `status` (`pass`/`fail`), `exits`, `warnings`, `tests`, `tests_failed`, `test_seconds`, `excerpt` (first error or warning lines), `group` (optional roadmap group id) |

Order of work: the plan sorts by stage (`lanes.order`), then box (`nodes.order`), then group (`items.order`). A group shows
as Ready, and in the "Start here" panel, while it is open and every group in its `after` is finished; otherwise it shows
"After …". So when a group really has to wait for another, say so in `after`, not only in `eta.note`. Put only hard
prerequisites there: a group that merely benefits from another stays Ready. The "Needs Gabriel" panel lists open groups
with preset or status `gab`, plus groups with an open task whose text starts with "Gabriel" or contains "ask Gabriel".

`tasks` is a map: `{ "t03": { "t": "text", "s": "todo|done|oos", "o": 3 } }`. `s` is `todo`, `done` (drawn struck
through) or `oos` (out of scope). `o` is the display order. Keys never change, so two sessions changing different
tasks of one group merge without conflict.

`eta` is the estimated completion: `{ "target": "2026-11-05", "effort": "~2 days", "note": "what it waits on" }`
(shown on open groups only). The project-wide projection under the build line is computed by the page on every
change, so never write dates for it by hand: each milestone in `meta/page` `milestones`
(`[{ "label", "groups": [ids] }` or `{ "label", "exclude": [ids] }]`) is projected at the latest `eta.target` of its open
groups, a target already past counts as today, and a milestone with no open group shows as done. `meta/page` `eta` holds
only the assumptions sentence shown after it. So the projection stays right as long as each group's `eta` does: when a
group's scope or blockers change, update its `eta`; when the G1 / Q2 in-game dates move, shift the dependent targets;
give every new group an `eta` (a group without one is counted as "without estimate").

Status keys (groups and boxes): `next` Do next, `partly` Partly done, `big` Not started, `spec` Spec-blocked,
`open` Guesses to confirm, `probe` Probe only, `gab` Needs Gabriel, `done` Done, `out` Out of scope.
Presets: `xhigh`, `high`, `re2ui`, `sonnet`, `tools`, `gab` (defined in the page, `PRESETS` in `docs/ROADMAP.html`).
A preset is the recommended model and effort level for a group: `xhigh` Opus 5.5 · xhigh, `high` Opus 5.5 · high,
`re2ui` Opus 5.5 · high then Sonnet 5.5 · high, `sonnet` Sonnet 5.5 · high, `tools` Sonnet 5.5 · medium,
`gab` Gabriel + Opus 5.5 · medium.

## Common edits

- Close a task: `update` `items/<id>` with `{"tasks":{"t03":{"s":"done"}},"updated":"2026-10-30","by":"<session>"}`.
- Split a task into a done part and an open part: set the old key's text/state, add a new key `t<next>` with `o` after it.
- Add a task: `update` with `{"tasks":{"t12":{"t":"…","s":"todo","o":12}}}`. Take the next free key and `o` from your last read.
- Bottleneck: put `BOTTLENECK` in the group's `tag` (e.g. `"G1 · BOTTLENECK"`). While the group is open the page
  draws it in red with a badge, outlines its flowchart box and counts it in the "Bottlenecks" filter. Remove the word
  from `tag` when the group stops holding others up.
- Group finished (Gabriel, 2026-10-30, required project-wide): whichever session closes a group's last `todo` task marks
  the group done in the same write: `update` `{"status":"done"}` (drop `preset`: `{"preset":{"__delete__":true}}`). Never
  leave a group open with no `todo` task. Before you end a task, re-read every group you touched (`roadmap-store/data/items/<id>.json`):
  if all its tasks are `done` or `oos`, it must already be `done`; if it is not, fix it now. A flowchart box's status is separate and
  the page never derives it: when every group in a box is `done`, set the box done in the same write (`update` `nodes/<ID>`
  `{"status":"done"}`). `PROC` was left at `partly` after `pa` and `paf` finished, which is how this rule came about (2026-10-31).
- New group: `set` `items/<new-id>` with `title, node, status, preset, mod, order, after, tasks, updated, by`.
- Follow-up group (Gabriel, 2026-10-29): when a group is finished and its leftovers go into a new follow-up group
  (e.g. `n1` -> `n1f`), the follow-up group always gets a `preset`: the recommended model and effort level for
  that remaining work, picked from the list above. Say in its `why` which preset was chosen and the reason in a few words.
- Flowchart: `update` `nodes/<ID>` `{"status":"done"}` recolours a box; `{"to":{"OTHER":"solid"}}` adds an arrow;
  `{"to":{"OTHER":{"__delete__":true}}}` removes one. New box: `set` `nodes/<ID>` with `label, sub, lane, status, shape, order, to`.
  New stage: `set` `lanes/<id>` with `title, goal, exit, order`.
- After every change write one `log` document (one line: what changed). Prune `log` documents older than ~200 entries when you notice them.

## Concurrency rules

1. `write` holds a lock on one machine and pulls before applying when a remote is set, so read the document just before writing.
2. Never resend your old copy of a whole document: send only your own change.
3. Prefer `update` with only the changed task key over `set`. `set` replaces the whole document and is for new documents.
4. Put several documents in one file when they belong together, for example a finished group and its box. They land in one commit.
5. Keep the rule from `CLAUDE.md` that every finished item is struck through, not deleted: set `s` to `done`.
   Out-of-scope work is the exception (Gabriel, 2026-10-30): delete those tasks (`{"tasks":{"t03":{"__delete__":true}}}`)
   and wholly out-of-scope groups and boxes instead of keeping them as `oos` / `out`. Record the decision in the
   `docs/PHASE4.md` Decisions table first; that table, not the roadmap, is the record of what is out of scope.
6. Keep `docs/PHASE4.md` and the phase status docs in step with the roadmap, as before.

## Other accounts

`roadmap-store/` is its own git repository, and `write` pushes to its remote when one is set. Another account clones it and
updates the same way (`roadmap-store/README.md`). Until a remote is set, updates stay on this machine.

### Both copies on every update (Gabriel, 2026-11-04)

Every roadmap update, by any agent or account, updates **both** copies of the roadmap:

1. **The GitHub copy**: `python roadmap-store/roadmap_store.py write <file>` writes `roadmap-store/data/` and pushes the store repository
   (`Empire-Vulkan-Brenach`). Check with `sync` (prints `in sync with origin/main`).
2. **The local copy**: the same `write` regenerates `ROADMAP.html` in the project root and `docs/ROADMAP.html` (data block). Both are
   tracked in the engine repository, so they show as modified after the write: **commit them in the same branch / PR as the work**.
   Never `git checkout` them away to get a clean tree, and never leave them out of the PR: the local page is the main roadmap on this
   device, and a stale one shows work as open that is already done. In a worktree, run `render` once before committing if the
   pages look older than the store.

An update that reached only one of the two copies is unfinished.

### Keeping the local page current (Gabriel, 2026-10-30)

When `ROADMAP.html` changes in the GitHub store repository (another account or machine wrote to the roadmap), the local
`docs/ROADMAP.html` data block and the `ROADMAP.html` copy in the project root must change with it. The page is generated from `roadmap-store/data/`, so this happens
whenever the store is brought up to date through the script:

- `python roadmap-store/roadmap_store.py sync` pulls the remote (`git pull --rebase`), then regenerates the data block of
  `docs/ROADMAP.html`, both copies of `ROADMAP.html` (in `roadmap-store/` and in the project root) and `ROADMAP.md`, commits and pushes. Run it at the start of a
  session, and whenever the roadmap on GitHub may have changed.
- `write` pulls first and regenerates both copies the same way, so any roadmap update on this machine refreshes the local page
  too. Build records (`record-build`) are batched since 2026-11-06 (see "Build records"): the page shows them after the next
  `write`, `sync` or `flush`.
- A plain `git pull` inside `roadmap-store/` updates the store's own `ROADMAP.html` but **not** the project-root copy. If you
  pulled by hand, run `python roadmap-store/roadmap_store.py render` (or `sync`) afterwards.
- `docs/ROADMAP.html` and the project-root `ROADMAP.html` are tracked in the engine repository, so after a refresh they show as
  modified there: commit them with the work that triggered the update (the data block is generated, never reviewed or edited by hand).

Check that the two copies agree with `python roadmap-store/roadmap_store.py sync` (prints `in sync with origin/main`) and by
comparing `ROADMAP.html` with `roadmap-store/ROADMAP.html` (byte-identical after any refresh).

## Claims and parallel batches

Several agents work in parallel, so a group can be claimed and the coordinator can ask for a batch that cannot collide. All of it
goes through the normal locked write path (pull, apply, journal, commit, background push), so a claim reaches every account.

```bash
python roadmap-store/roadmap_store.py batch 5 [--by <session>] [--json]   # up to N ready groups that touch different files
python roadmap-store/roadmap_store.py claim <group> --by <session> [--touches a,b] [--force]
python roadmap-store/roadmap_store.py claims [--all]                        # live claims (--all adds expired ones)
python roadmap-store/roadmap_store.py release <group> [--by <session>] [--force]
```

- `claim` sets `claimed_by` / `claimed_at`. It fails (exit 1, naming the holder and the expiry) while another session holds a live
  claim, unless `--force`. A claim is live for 24 hours; claiming again with the same `--by` renews it, and an expired claim can
  be taken by anyone. `--by` defaults to `ALAMO_SESSION`. `--touches` stores an explicit `touches` list; otherwise `touches` is the
  group's `mod` string split on `·`, `,` and `;` (`a/b+c` means `a/b` and `a/c`).
- `release` clears the claim (`--by` makes it refuse another session's live claim without `--force`). A group written as `done` or
  `out` through `write` (or the page hook) loses its claim in the same write and commit, so nobody has to release it.
- `batch` prints up to N groups that are open, have every `after` group finished, are not claimed by another session, do not have
  status `gab`, `probe` or `spec` (or preset `gab`), and whose `touches` overlap neither each other nor the touches of a live claim.
  Order: status `next` first, then stage, box and group order. Two paths overlap when they are equal or one is a folder of the
  other (`render` and `render/particles`). A leading `engine/` is ignored, and `apps/sim` counts as `apps`, so `apps`,
  `engine/apps` and `apps/sim` overlap each other and everything under `apps`. A group with no `mod` and no `touches` never
  conflicts. `--by <session>` stops that session's own claims from blocking it.
- Coordinator flow: `batch N`, then each agent runs `claim <group> --by <its name>` before it edits anything; a refused claim means
  take the next group from a fresh `batch`.
- The page shows a "claimed by X" badge on groups with a live claim and keeps them out of the "Start here" suggestions (it reads
  the two fields only; a claim older than 24 hours is ignored). The shell is edited in `roadmap-store/page_shell.html` (what the
  generator reads) and `docs/ROADMAP.html`, outside the data markers.
- `python roadmap-store/roadmap_store.py selftest` runs the tests for all of this on a temporary store with a local bare remote
  (claim conflicts, expiry, overlap and batch logic, done clearing, build-record batching, lock-busy fallback); it never touches
  the real store.

## Build records

`build_msvc.cmd`, `build_clang.cmd`, `build_module.cmd` and `build_agent.cmd` record every finished run in the store with
`record-build`, so the roadmap shows them with no further step. The full log stays in `logs\` on the machine that built; the
record carries the summary and the first error lines. Set `ALAMO_ROADMAP_GROUP=<group id>` before a build to link it to a
group, and `ALAMO_SESSION=<name>` to name the session instead of the OS user. Records are only added: do not edit or prune them.

`record-build` only writes the record file and a journal line (no commit, no regenerate, no push): one build run used to make one
commit and one push. The commit and the push are batched: a push happens at most every 10 minutes (`FLUSH_INTERVAL`). After a
record, a detached `flush` runs at once when the last push is older than that, otherwise one waiting `flush --wait` is started
(one at a time) that runs when the interval is over. `write`, `claim`, `release` and `sync` commit pending records first, pull
and push as before, so they never lose one; `python roadmap-store/roadmap_store.py flush` does it by hand (it also commits when
there is no remote). A busy lock is retried (4 tries of 8 s); only if it stays busy is the record written without the lock and a
line added to `hook_errors.log`, so a record is never dropped. Pushes happen outside the lock, and a rejected push pulls and retries.

The pre-database static page and its seed files are in `backups/docs-archive-2026-10-30.zip`.

# Project roadmap: how sessions update it

The main roadmap and flowchart lives in one place: the `roadmap-store/` folder (Gabriel, 2026-11-06; before that the page was also
tracked in this repository as `ROADMAP.html` and `docs/ROADMAP.html`, which are removed). Open `roadmap-store/ROADMAP.html` in any
browser, or in the Claude desktop Browser pane: no claude.ai sign-in. It is generated, so never edit it by hand: change the documents
in `roadmap-store/data/` and it is rebuilt. `roadmap-store/` is its own git repository (remote `Empire-Vulkan-Brenach`) in the main
checkout; worktrees do not have it, so run the script from the main checkout.

The page layout is `roadmap-store/page_shell.html`: edit only what is outside the `<!--roadmap-data:begin-->` and `<!--roadmap-data:end-->`
markers (layout, script, `PRESETS`), then run `python roadmap-store/roadmap_store.py render`. `ROADMAP.html` is the shell plus a
read-only copy of the data, and `roadmap-store/ROADMAP.md` is the text form; both are regenerated on every write, and a diff in them is never a
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
| `meta` | `backlog` | The backlog's settings: `cats` (category id → `label`, `parent`, `order`, `desc`, `node`: the flowchart box its entries belong to), `rules` (ordered category filters), `sevs` (severity meanings), the limits `cap`, `wip`, `inbox_days`, and `keep_followups` (the stage ids whose groups keep their follow-ups as tasks: `["st6"]`, Stage 6 · Parity and mods). See "Backlog and group exit tests" |
| `backlog` | `b0001`… | One finding that is not part of an open group: `t` (text), `cat` (category id), `node` (flowchart box, and so the stage, the work belongs to), `sev` (`S1`/`S2`/`S3`), `kind`, `s` (`open`/`promoted`/`dropped`), `from` (group it was found in), `task`, `ref`, `g` (only Gabriel can do it), `added`, `by`, and when closed `promoted` (`group/task`) or `why` |
| `lanes` | short id (`st1`…) | One stage of the plan: `title`, `goal` (what the stage is for), `exit` ("done when" condition), `order`. Stages run in `order`, top to bottom in both the plan and the flowchart |
| `nodes` | box id (`N1`, `MOVE`…) | `label`, `sub`, `lane`, `status`, `shape` (`box`/`pill`), `order`, `to` (map of target box id → `solid` "do before" or `dash` "feeds into") |
| `items` | group id (`s7`, `n1`…) | `title`, `tag`, `node` (box id), `status`, `preset`, `mod`, `why`, `order`, `after` (list of group ids that must finish first), `updated` (date), `by` (session name), `tasks` (key `t01`… → `t` text, `s` `todo`/`done`/`oos`, `o` order, optional `sev`, `tier`, `g`, `moved`, `why`), `eta`, `exit` (the exit test: the metric or test that says the group is done), `cap` (most tasks the group may hold; default `meta/backlog` `cap`, 8), `closed` (set by `close-group`: `at`, `passed`, `moved`, `dropped`); claim fields, all optional, set by `claim` (below): `claimed_by` (session), `claimed_at` (ISO time), `touches` (list of module folders/files the work edits; default: `mod` split on `·` `,` `;`) |
| `log` | `yyyymmdd-hhmmss-<session>` | `at` (ISO time), `by`, `text` |
| `builds` | `yyyymmddhhmmss-<machine>-<tree>-<compiler>` | `at`, `by`, `machine`, `compiler`, `tree`, `status` (`pass`/`fail`), `exits`, `warnings`, `tests`, `tests_failed`, `test_seconds`, `excerpt` (first error or warning lines), `group` (optional roadmap group id) |

Order of work: the plan sorts by stage (`lanes.order`), then box (`nodes.order`), then group (`items.order`). A group shows
as Ready, and in the "Start here" panel, while it is open and every group in its `after` is finished; otherwise it shows
"After …". So when a group really has to wait for another, say so in `after`, not only in `eta.note`. Put only hard
prerequisites there: a group that merely benefits from another stays Ready. The "Needs Gabriel" panel lists open groups
with preset or status `gab`, plus groups with an open task that needs Gabriel: its text starts with "Gabriel" or
contains "ask Gabriel", or it has `"g": true`. Inside a group's task list each such open task carries a pink
"Needs Gabriel" tag (Gabriel, 2026-11-08), so put `"g": true` on any task only Gabriel can do (launching the game,
a playtest, a release merge) whose text does not start with "Gabriel:".

`tasks` is a map: `{ "t03": { "t": "text", "s": "todo|done|oos", "o": 3, "g": true } }`. `s` is `todo`, `done` (drawn struck
through) or `oos` (out of scope). `o` is the display order. `g` (optional) marks a task that needs Gabriel. Keys never
change, so two sessions changing different tasks of one group merge without conflict. A task that `close-group` moved to the
backlog stays as `oos` with `moved: "b0012"`; one it dropped stays as `oos` with `why`; a task made from a backlog entry has `bl: "b0012"`.

`eta` is the estimated completion: `{ "target": "2026-11-05", "effort": "~2 days", "note": "what it waits on" }`
(shown on open groups only). The project-wide projection under the build line is computed by the page on every
change, so never write dates for it by hand: each milestone in `meta/page` `milestones`
(`[{ "label", "groups": [ids] }` or `{ "label", "exclude": [ids] }]`) is projected at the latest `eta.target` of its open
groups, a target already past counts as today, and a milestone with no open group shows as done. `meta/page` `eta` holds
only the assumptions sentence shown after it. So the projection stays right as long as each group's `eta` does: when a
group's scope or blockers change, update its `eta`; when the G1 / Q2 in-game dates move, shift the dependent targets;
give every new group an `eta` (a group without one is counted as "without estimate").

Status keys (groups and boxes): `next` Do next, `partly` Partly done, `big` Not started, `spec` Spec-blocked,
`open` Guesses to confirm, `probe` Probe only, `gab` Needs Gabriel, `done` Done, `out` Out of scope, `parked` Parked (`park`).
Presets: `xhigh`, `high`, `re2ui`, `sonnet`, `tools`, `gab` (defined in the page, `PRESETS` in `roadmap-store/page_shell.html`).
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
- Group finished by its exit test (Gabriel, 2026-11-10): when the group's `exit` test passes, close it with `close-group`
  (open tasks go to the backlog, "Backlog and group exit tests" below); do not keep it open for the tasks that are left.
- Group finished (Gabriel, 2026-10-30, required project-wide): whichever session closes a group's last `todo` task marks
  the group done in the same write: `update` `{"status":"done"}` (drop `preset`: `{"preset":{"__delete__":true}}`). Never
  leave a group open with no `todo` task. Before you end a task, re-read every group you touched (`roadmap-store/data/items/<id>.json`):
  if all its tasks are `done` or `oos`, it must already be `done`; if it is not, fix it now. A flowchart box's status is separate and
  the page never derives it: when every group in a box is `done`, set the box done in the same write (`update` `nodes/<ID>`
  `{"status":"done"}`). `PROC` was left at `partly` after `pa` and `paf` finished, which is how this rule came about (2026-10-31).
- New group: `set` `items/<new-id>` with `title, node, status, preset, mod, order, after, tasks, updated, by, exit, eta`. A group
  without an `exit` test is a lint warning (`roadmap_store.py lint`, and `write` prints it for the groups it touched).
- Leftovers of a finished group (Gabriel, 2026-11-10, replaces the 2026-10-29 follow-up-group rule): they go to the backlog, not
  into a new `…f` group. `close-group` does it in one step (see below). A new group comes from the backlog only when someone
  picks entries on purpose (`backlog promote --new`), and it needs its own exit test, `eta` and (recommended) `preset`; say in
  its `why` which preset was chosen and the reason in a few words. Exception: a Stage 6 group keeps its follow-ups as its own
  tasks ("Follow-ups stay in Stage 6 groups" below).
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
   `docs/DECISIONS.md` first; that table, not the roadmap, is the record of what is out of scope.
6. The roadmap is the only status tracker (Gabriel, 2026-11-06): `docs/PHASE2.md` / `PHASE3.md` / `PHASE4.md` no longer carry status boards or flowcharts, and `claude/roadmap.md` / `claude/phase2-status.md` are not updated. Record results where `docs/README.md` says (decisions, measurements, build rules, specs), but track what is open or done only here.

## Backlog and group exit tests (Gabriel, 2026-11-10)

Why: groups were defined by their task list, and every probe or RE pass added tasks, so chains like `a2f`…`a2m`, `s78f`…`s78n` and
`cu8`…`cu8m` never reached zero. A group is now defined by an **exit test** and a size cap, and findings that do not block it
leave the group.

- **Exit test.** Every group has `exit`: a metric or test you can run ("Underworld credits at 9000 within the retail band over 12
  seeds", not "the leftovers"). The task list is only a plan for passing it and may change; when the test passes the group is
  done, even with tasks left. For parity work the test is a tier-2 metric or a tier-1 spec check (`docs/DECISIONS.md`, "Parity
  tiers"); a difference that is tier 3, or inside the tolerance, is not a reason to keep a group open.
- **Cap.** A group holds at most `cap` open tasks (default 8, `meta/backlog`; finished tasks do not count; a Stage 6 group has no cap,
  see "Follow-ups stay in Stage 6 groups"). Above it, split the
  group (a new group with its own exit test) or move findings to the backlog; `trim-group <group> --by S --move t04:S2,t05 --drop t08
  --why "..."` takes tasks out of an open group without closing it (a group another session has claimed needs `--force`). RE and probe tasks name their stop condition (seeds, traces, the questions to answer).
- **Triage of a finding** (probe, RE pass, review, test failure), decided when it is found:

  | Bucket | When | What happens |
  |---|---|---|
  | Fix now | it breaks this group's exit test, or it is a crash, hang, save corruption or a tier-1 difference a player or modder sees | a task of the group (`backlog promote --to`, or a plain task) |
  | Backlog | real, but not blocking | `backlog add ... --from <group>`: one entry, a severity, a category. From a Stage 6 group the same command adds a task of that group instead (below) |
  | Drop | tier 3, inside tolerance, a guess nobody relies on | not recorded, or `backlog drop --why` |

- **Backlog.** Its own collection (`backlog`), so it never shows as an open group or in "Start here". Severity: `S1` crash, hang,
  save corruption or a tier-1 difference a player or modder sees; `S2` a tier-2 metric fails or a rule differs without a visible
  effect yet; `S3` polish or a documented guess. Kind: `fix`, `port`, `re`, `probe`, `test`, `decision`, `doc`.
- **Categories.** `inbox` ("Untriaged") is the intake. `meta/backlog` `rules` (ordered; first match on the entry's own text, then
  on its group's `mod`; `gab` matches Gabriel-only entries) file a new entry into a more specific category straight away; an entry no
  rule matches stays in `inbox`, and triage moves it (`backlog move`, `backlog retag --apply` re-runs the rules). Categories are
  data too (`backlog cats --add <id> --label ... --parent ... --node <box>`). Since 2026-11-12 every category has a flowchart box
  (`node`), so an entry also says which stage and box its work belongs to, as Stage 5 · OPT is performance: Parity (`parity-sim`
  movement and collision → MOVE, `parity-combat` → MOVE, `parity-anim` → ANIM, `parity-rules` other tactical rules → ABIL,
  `parity-ai` galactic AI → Q2F, `parity-tactical-ai` → AIVIEW, `parity-campaign` galactic rules → Q2F, `parity-story` → STORY),
  Engine (`lua-data` → RE, `render-ui` → UIART, `mods` → MODC, `perf` → OPT), Process (`tools-harness`, `docs-process` → PROC),
  Research (`re-research`, `gabriel` → CONFIRM). `backlog cats` lists them with their stage. Every entry carries its own
  `node`: `backlog add` sets the category's box (else the `--from` group's), `--node` overrides it, `backlog move --cat` brings the
  new category's box, `backlog move --node` re-files only the box, `backlog list --node <box|stage lane>` filters, `cats --set <id>
  --node <box>` changes a category's box, and `promote --new` without `--node` uses the entries' shared box. Lint flags an entry
  without a box (warning) or with an unknown one (error). The page's Backlog tab shows the counts per category and severity, each
  category's and entry's stage and box, and filters by category, box, severity, kind and tier; a box's view links to its backlog.
- **Closing.** `python roadmap-store/roadmap_store.py close-group <group> --by <session> --passed "<evidence>" [--cat C]
  [--drop t05,t06 --why "..."]` checks nothing itself (the exit test is the lane's evidence) and, in one commit: moves every open
  task to the backlog (filed by the rules, `--sev` default S2, Gabriel-only tasks flagged) or drops the named ones, sets the group
  `done` (preset and claim cleared, `closed` recorded), sets its box `done` when it was the box's last open group, and adds a
  log line. It replaces the "follow-up group" step. A Stage 6 group is closed differently (next point).
- **Follow-ups stay in Stage 6 groups** (Gabriel, 2026-11-12, to help development there). Stage 6 · Parity and mods (lane `st6`:
  boxes MODC, Q4, Q5, Q6 and any box added to it later; `meta/backlog` `keep_followups` lists the stages this applies to) is the
  exception to the backlog rule. A follow-up of a Stage 6 group (a finding that does not block its exit test, or a task still open
  when the test passes) becomes a new task of the group it came from, in that group's box, so the next session on the box finds it
  there:
  - `backlog add "<text>" --by S --from <group> [--sev S1|S2|S3] [--tier 1|2] [--ref PATH] [--g]` does it: it adds the group's next
    `tNN` with that severity, tier and Gabriel flag (`--ref` is appended to the text) instead of a backlog entry. A finding for a group
    that is already `done` reopens it (status `partly`, and its box when that is `done` too); the group's `closed` record stays as history.
  - The cap does not apply to these groups: `lint`, the page's "Over cap" badge and `backlog promote --to` skip it.
  - Closing: the group is done when its exit test passes **and** no task is open. Finish each follow-up or drop it
    (`close-group <group> ... --drop t21,t22 --why "..."`). `close-group` refuses a Stage 6 group that still has an open task that is
    not tier 3 (`--force` sends those to the backlog as elsewhere). `trim-group --move` is refused for them too; `--drop` works.
  - Unchanged: a tier-3 finding is still dropped or filed in the backlog as S3 (`--tier 3` keeps it there; current focus), and the
    triage still decides whether a finding is worth recording at all. A finding that is not Stage 6 work (for example a movement bug
    a mod test happens to show) can still be filed in its own box's backlog by naming the box: `--node <box>`.
  - Open backlog entries filed from Stage 6 groups before 2026-11-12 (b0014-b0017 from modc; b0018 is tier 3 and stays) are left in
    the backlog until someone promotes them back (`backlog promote <ids> --to <group>`).
- **Parity tier** (2026-11-10). A task or backlog entry that is a difference from retail carries `tier` (`1`, `2` or `3`, as in
  `docs/DECISIONS.md`, "Parity tiers"); work that is not about matching retail (tools, UI of the port, Gabriel's runs) has none. The
  tier says how the difference is judged and what it is worth: **1** exact, spec and unit test, a difference is a bug (S1 when a
  player or modder sees it, else S2); **2** statistical, judged by the parity harness over many seeds, so the entry must name the
  failing metric (without one it is S3, and one seed's outcome moving is not a finding); **3** free (internals, draw order,
  precision of internal timers, performance, save format): S3 at most, and retail-matching tier-3 work is dropped
  (`backlog drop --why`) unless a tier-2 metric fails. `backlog add --tier T`, `backlog move ID --tier T|none`, `backlog list --tier T`;
  `trim-group` / `close-group` carry a task's tier into the entry and `promote` back. The page filters the Backlog by tier and shows
  the tier in the severity badge's tooltip (since 2026-11-12 it shows S1 / S2 / S3 badges, not T1 / T2 / T3; see "Task severity"). 2026-11-10: every open task and entry was tagged (open tasks 27 tier 1, 6 tier 2, 4 tier 3, 15 none; backlog
  open 13 tier 1, 5 tier 2, 5 tier 3, 4 none); 5 tier-3 entries (b0005, b0006, b0011, b0030, b0031) were dropped and 2 (b0008, b0029) lowered to S3.
- **Task severity** (2026-11-12). Every group task carries `sev` (`S1` / `S2` / `S3`, the backlog's meanings above, applied to the
  problem or need the task addresses; tier 3 is S3 at most). `trim-group` / `close-group` move a task with its own `sev` unless `--sev`,
  `--sevs` or `t04:S2` gives one; `promote` gives the entry's severity to the new task. Lint: a bad `sev` is an error, an open task
  without one or a tier-3 open task above S3 a warning. The page and `ROADMAP.md` show the severity on every task. 2026-11-12: all
  1165 tasks were classified (S1 76, S2 468, S3 621; open tasks S1 12, S2 24, S3 55).
- **Current focus: tier 1 and tier 2 only** (Gabriel, 2026-11-10, until he lifts it; `docs/DECISIONS.md`). Plan and `batch` only groups
  whose open tasks are tier 1 or 2 (or untagged work that serves them: harness, probes, Gabriel's runs). Tier-3 tasks are not started; a
  tier-3 finding is dropped or filed `--tier 3` (S3), never a task. A crash, hang or save corruption is fixed whatever its tier. Open tier-3
  work today: group `opt3f`, `r1b` t02, and `backlog list --tier 3`.
- **Promoting.** Entries come back only by choice: `backlog promote <ids> --to <group>` (refused above the cap) or
  `--new <id> --title ... --node ... --exit ... [--preset ...] [--eta ...]`. S1 entries are reviewed first each cycle.
- **Parking.** `park <group> --by S --why "..."` sets an unstarted group to status `parked`: it leaves the open lists, "Start here" and
  the limit, keeps its tasks and shows as "Parked" (a milestone made only of parked groups shows "Parked", not "Done");
  `unpark <group> --by S` restores its old status. Use it for side goals that are not on the path to the current goal.
- **Limits and lint.** `meta/backlog` `wip` (default 8) is the number of *started* groups (one with a closed task or a live claim, not
  waiting on an `after` group, not status `gab`) the project aims for; `inbox_days` (14) is how long
  an entry may sit untriaged. `roadmap_store.py lint [--strict] [--verbose]` reports: open group without an exit test, over its cap,
  open with no open task (error), done with an open task (error), more started groups than `wip`, a stale inbox entry, a bad
  category, severity or tier (error), a tier-3 entry above S3, a Parity-category entry without a tier. Run it when planning; `write` prints the group problems it touched.
- **Plan cycle.** Plan (Gabriel: pick a few groups from the backlog or the unstarted list, each with an exit test and cap; `batch`
  hands out a conflict-free set) → run (lanes, one claimed group each) → close (`close-group`) → triage (S1 / S2 first, drop or
  promote, empty `inbox`) → repeat.
- Tasks that `close-group` leaves as `oos` (moved, dropped) are the audit trail; the 2026-10-30 rule that out-of-scope tasks are
  deleted still applies to work Gabriel declares out of scope in `docs/DECISIONS.md`.

## Other accounts

`roadmap-store/` is its own git repository, and `write` pushes to its remote when one is set. Another account clones it and
updates the same way (`roadmap-store/README.md`). Until a remote is set, updates stay on this machine.

### One location (Gabriel, 2026-11-06)

There is one copy of the roadmap: `roadmap-store/`. A roadmap update is `python roadmap-store/roadmap_store.py write <file>`; it writes
`roadmap-store/data/`, regenerates `roadmap-store/ROADMAP.html` and `ROADMAP.md` and pushes the store repository. Nothing about the
roadmap is written into, tracked in or committed to the engine repository, so branches, PRs and worktrees never carry a stale page.
This replaces the 2026-11-04 "both copies" rule. Check the remote with `python roadmap-store/roadmap_store.py sync` (prints
`in sync with origin/main`); run it at the start of a session, and whenever the roadmap on GitHub may have changed (another account or
machine wrote to it). `sync` pulls, regenerates the page and pushes. `write` and every build record pull first the same way.
If you pulled by hand inside `roadmap-store/`, run `python roadmap-store/roadmap_store.py render` afterwards.

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
  other (`render` and `render/particles`). A leading `engine/` is ignored, and a bare `apps/sim` counts as `apps`, so `apps`,
  `engine/apps` and `apps/sim` overlap each other and everything under `apps`. Since WF1b (2026-11-08) `engine/apps/sim` is three
  libraries (`alamo_sim_base`, `alamo_sim_battle`, `alamo_sim_campaign`), and a file below it counts as `apps/sim/<target>/<stem>`
  (`apps/sim/src/tactical_battle_bt.cpp` -> `apps/sim/battle/tactical_battle_bt`; a header and its source are one stem; tests ->
  `apps/sim/tests/<stem>`; target by name prefix, `SIM_TARGET_PREFIXES` in `roadmap_store.py`). So lanes that list the sim files
  they edit run side by side, a target name (`apps/sim/battle`) overlaps each of its files, and a group whose `mod` says only
  `apps/sim` still blocks all of apps: give apps/sim groups file-level `touches` (`claim --touches`, or `update` the field).
  A group with no `mod` and no `touches` never conflicts. `--by <session>` stops that session's own claims from blocking it.
- Coordinator flow: `batch N`, then each agent runs `claim <group> --by <its name>` before it edits anything; a refused claim means
  take the next group from a fresh `batch`.
- The page shows a "claimed by X" badge on groups with a live claim and keeps them out of the "Start here" suggestions (it reads
  the two fields only; a claim older than 24 hours is ignored). The shell is edited in `roadmap-store/page_shell.html` (what the
  generator reads), outside the data markers.
- `python roadmap-store/roadmap_store.py selftest` runs the tests for all of this on a temporary store with a local bare remote
  (claim conflicts, expiry, overlap and batch logic, done clearing, build-record batching, lock-busy fallback); it never touches
  the real store.

## Build records

`build_msvc.cmd`, `build_clang.cmd`, `build_module.cmd` and `build_agent.cmd` record every finished run in the store with
`record-build`, so the roadmap shows them with no further step. The full log stays in `logs\` on the machine that built; the
record carries the summary and the first error lines. Set `ALAMO_ROADMAP_GROUP=<group id>` before a build to link it to a
group, and `ALAMO_SESSION=<name>` to name the session instead of the OS user. Records are only added: do not edit or prune them.
Since 2026-11-13 the build scripts also set `ALAMO_BUILD_EXTRA` to `<tree>\build_state.json`, and the record carries `commit`,
`worktree`, `branch`, `dirty`, `source` (hash of the built source), `modules`, `test_filter`, `failure_kind` (configure, compile,
warnings, test, golden, unfinished, other; null on a pass), `golden_drift` and `real_failures` (`docs/BUILD.md`, "Build state and
repeat check", "Golden drift in the summary"). A `build_agent.cmd ... none` run has no ctest step and passes on configure and build.

`record-build` only writes the record file and a journal line (no commit, no regenerate, no push): one build run used to make one
commit and one push. The commit and the push are batched: a push happens at most every 10 minutes (`FLUSH_INTERVAL`). After a
record, a detached `flush` runs at once when the last push is older than that, otherwise one waiting `flush --wait` is started
(one at a time) that runs when the interval is over. `write`, `claim`, `release` and `sync` commit pending records first, pull
and push as before, so they never lose one; `python roadmap-store/roadmap_store.py flush` does it by hand (it also commits when
there is no remote). A busy lock is retried (4 tries of 8 s); only if it stays busy is the record written without the lock and a
line added to `hook_errors.log`, so a record is never dropped. Pushes happen outside the lock, and a rejected push pulls and retries.

The pre-database static page and its seed files are in `backups/docs-archive-2026-10-30.zip`.

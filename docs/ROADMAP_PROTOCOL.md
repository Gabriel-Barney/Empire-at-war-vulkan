# Project roadmap: how sessions update it

The main roadmap and flowchart is `ROADMAP.html` in the project root (Gabriel, 2026-10-30). It opens in any browser with no
claude.ai sign-in. It is generated, so never edit it by hand: change the documents in `roadmap-store/` and it is rebuilt,
along with the copy in that repository that other accounts pull. `docs/ROADMAP.html` is its page shell.

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
| `items` | group id (`s7`, `n1`…) | `title`, `tag`, `node` (box id), `status`, `preset`, `mod`, `why`, `order`, `after` (list of group ids that must finish first), `updated` (date), `by` (session name), `tasks`, `eta` |
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
  leave a group open with no `todo` task.
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

## Build records

`build_msvc.cmd`, `build_clang.cmd`, `build_module.cmd` and `build_agent.cmd` record every finished run in the store with
`record-build`, so the roadmap shows them with no further step. The full log stays in `logs\` on the machine that built; the
record carries the summary and the first error lines. Set `ALAMO_ROADMAP_GROUP=<group id>` before a build to link it to a
group, and `ALAMO_SESSION=<name>` to name the session instead of the OS user. Records are only added: do not edit or prune them.

The pre-database static page and its seed files are in `backups/docs-archive-2026-10-30.zip`.

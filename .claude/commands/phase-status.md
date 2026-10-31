---
description: Update the roadmap, phase status doc and flowcharts after a task finishes
---
Following rule 6 in CLAUDE.md: update `docs/PHASE2.md` (status table, Mermaid node states, open
items) for the task just finished: $ARGUMENTS. On the roadmap (`roadmap_store.py write`, `docs/ROADMAP_PROTOCOL.md`), close the
task and re-read its group: when no `todo` task is left in that group, set the group `status` to `done` (and drop its `preset`) in the same
write (Gabriel, 2026-10-30, project-wide). If that was the last open group in its flowchart box, set the box `done` too (`nodes/<ID>`). If the claude.ai project docs are reachable, update
`claude/roadmap.md` and `claude/phase2-status.md` the same way. Minimal, factual edits only.

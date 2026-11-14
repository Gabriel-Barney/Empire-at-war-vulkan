---
name: phase-status
description: Update the roadmap (roadmap-store, roadmap_store.py write) after a task finishes - close the task, apply the group-done and box-done rules, record results in the right docs. Use after finishing any roadmap task or backlog entry outside a full /finish-group run, or when asked to tick, close or record something on the roadmap.
argument-hint: <task or group just finished>
---
Following rule 6 in CLAUDE.md: update the main roadmap (`roadmap_store.py write`, `docs/ROADMAP_PROTOCOL.md`) for the task just finished: $ARGUMENTS. It is the only status tracker:
close the task and re-read its group; when no `todo` task is left in that group, set the group `status` to `done` (and drop its `preset`) in the same
write (Gabriel, 2026-10-30, project-wide). If that was the last open group in its flowchart box, set the box `done` too (`nodes/<ID>`). The roadmap is `roadmap-store/` only: nothing to commit in this repository for it. Do not update status in `docs/PHASE*.md` or `claude/*.md`; results go where `docs/README.md` says (`docs/DECISIONS.md`, `docs/PERFORMANCE.md`, `docs/BUILD.md`, `docs/re/`).
Minimal, factual edits only.

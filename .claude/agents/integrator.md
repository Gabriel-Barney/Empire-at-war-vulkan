---
name: integrator
description: Integrates finished engine modules - wires them into engine/apps, runs the full Windows MSVC build and tests, and updates roadmap/status docs and flowcharts. Use after module agents report back.
tools: Read, Write, Edit, Glob, Grep, Bash
---
Read `CLAUDE.md` and `docs/PHASE2.md`. Then:
1. Configure and build the whole tree (all modules) and run ctest. On Windows use `build_msvc.cmd` and read `logs\`.
2. Fix integration issues (core headers, top-level CMake, `engine/apps`). Send module-internal problems back to that module's owner rather than rewriting the module.
3. Update `docs/PHASE2.md` (status table + Mermaid node states) and the project docs `claude/roadmap.md` / `claude/phase2-status.md` when available.
4. Update the roadmap (`python roadmap-store/roadmap_store.py write`, see `docs/ROADMAP_PROTOCOL.md`): close each finished task, then check every group you touched. A group with no `todo` task left is marked `done` in the same write (drop its `preset`); never leave a finished group open (Gabriel, 2026-10-30). When that was the last open group in a flowchart box, set the box `done` too (`update` `nodes/<ID>` `{"status":"done"}`; box status is not derived).

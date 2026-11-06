---
name: integrator
description: Integrates finished engine modules - wires them into engine/apps, runs the full Windows MSVC build and tests, and updates the roadmap. Use after module agents report back.
tools: Read, Write, Edit, Glob, Grep, Bash
---
Read `CLAUDE.md` and `docs/ROADMAP_PROTOCOL.md`. Then:
1. Configure and build the whole tree (all modules) and run ctest. On Windows use `build_msvc.cmd` and read `logs\`.
2. Fix integration issues (core headers, top-level CMake, `engine/apps`). Send module-internal problems back to that module's owner rather than rewriting the module.
3. Record results where `docs/README.md` says (decisions in `docs/DECISIONS.md`, measurements in `docs/PERFORMANCE.md`, build rules in `docs/BUILD.md`, specs in `docs/re/`). Status is tracked only on the roadmap (next step); do not update `docs/PHASE*.md` status boards or `claude/*.md`.
4. Update the roadmap (`python roadmap-store/roadmap_store.py write`, see `docs/ROADMAP_PROTOCOL.md`): close each finished task, then check every group you touched. A group with no `todo` task left is marked `done` in the same write (drop its `preset`); never leave a finished group open (Gabriel, 2026-10-30). When that was the last open group in a flowchart box, set the box `done` too (`update` `nodes/<ID>` `{"status":"done"}`; box status is not derived).

---
name: engine-module
description: Narrow one-folder helper - implements or extends one engine/<module> library (core excluded) when the task is confined to that module and needs no roadmap or PR work. For real roadmap work use the lane agent (own worktree, whole group, PR); this is the older module-split model.
tools: Read, Write, Edit, Glob, Grep, Bash, Skill
skills:
  - determinism-check
  - diagnosing-bugs
---
You own exactly one folder, `engine/<module>/`, named in your task. Read `CLAUDE.md`, `docs/PHASE2.md` (design notes),
the core headers in `engine/core/include/alamo/core/`, and the `docs/re/` specs your task points to.

- Write only inside your module folder. If you need a change in core or the top-level CMake, describe it in your report instead of editing.
- Never open `re_reference/` (clean-room); specs come from `docs/`.
- Build just your module with the project scripts, never a hand-run `cmake`: `build_agent.cmd msvc "<module>"` (or `build_module.cmd <module>`), plus `build_clang.cmd` for sim code; see `docs/BUILD.md`. Warning-free; tests registered with `add_test`.
- Follow the determinism rules in CLAUDE.md.
- Report: files, public API, test results, decisions, open items (short).

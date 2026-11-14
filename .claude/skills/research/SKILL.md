---
name: research
description: Investigate a question from primary sources in a background agent and save a cited Markdown report in docs/. Use when asked to research a technique, API, format or spec (Vulkan, SPIR-V, DXC, Win32, Lua 5.0, FFmpeg Bink, performance techniques), to gather docs the engine must follow, or to recover what the original game does from docs/re and the extracted game text. Not for quick lookups.
argument-hint: <question> [where the report goes]
---
# Research

Answer one question with claims that trace to the source that owns them, written to one file the next agent can trust.
Adapted from `mattpocock/skills` (MIT). Two kinds of question, with different sources:

| Kind | Primary sources, best first | Report home (`docs/README.md` has the rule) |
|---|---|---|
| **External technique or API** (renderer, shaders, threading, OS, libraries, performance) | Vendor specs and docs (Khronos Vulkan / SPIR-V spec and registry, Microsoft Learn, DXC repo and wiki, the Lua 5.0 manual and source, FFmpeg source), then the library's own source and issue tracker. Developer blogs and talks are leads, flagged as secondary | `docs/OPT_RESEARCH.md` style for performance (new dated file or a section there); a section of the matching `docs/PHASE*.md` for reference; `docs/shaders/` or `docs/formats/` for those catalogs |
| **What the original game does** | `docs/re/` specs, then `extracted/` game text (`foc_data_text.zip`, `eaw_data_text.zip`: XML and Lua), then the `Data` folder via the VFS tools, then probe results (`docs/PROBES.md` index) | the matching `docs/re/<SPEC>.md`, or a new `docs/re/<GROUP>_<TASK>_<TOPIC>.md` named like its neighbours when no spec covers the subject |

## Rules

- **Clean-room.** A behaviour question is answered from the sources in the table. `re_reference/` is read only when the task is a
  named RE / doc task from the roadmap, and then the report holds behaviour in words and tables, never code or decompiler
  text. A question that only a retail run can answer ends as a probe proposal (`tools/probe_mod/`, `docs/PROBES.md`);
  Gabriel launches the game (rule 7).
- **Dependencies.** A technique that needs a new third-party library in `engine/` is reported with its licence and size, and the
  report says it needs Gabriel's approval (rule 2). It is never added by the research.
- **Fit check.** Judge each finding against the engine's constraints: determinism rule 3 (bit-identical across worker counts,
  runs, MSVC and clang), Windows x64 only, 30 ticks/s, Lua 5.0.2 original semantics, parity tiers (`docs/DECISIONS.md`), and the
  current focus (tier 1 and 2; tier 3 findings go in the report but are not started).
- **Privacy.** Web queries carry generic technical terms only; no repository paths, names or task text.

## Steps

1. **Frame the question.** One sentence, plus what decision the answer feeds. If the question is several, split it and run one
   report each.
2. **Start a background agent with a fresh context** (`Agent`, `general-purpose`, run in the background) so this session keeps
   working. The brief repeats the clean-room and dependency rules, names the sources to try first, the report path and the
   format below. Read-only agents (`Explore`) cannot write the report; use them only for the local `docs/re` sweep.
3. **Investigate.** Trace every claim to the owner of the fact (spec section, header, source file and line, doc page URL);
   follow a secondary source back to its primary one, or mark it `SECONDARY`. Run a quick check where one exists (a tiny test
   program, an `alamo_mod_check` or headless run, a `Grep` of the extracted text) instead of reasoning about behaviour.
4. **Write one Markdown file** at the report home, LF line endings:
   - title with the date, and the question in one sentence;
   - **Answer** first, in a few lines, with a confidence label per claim: `CONFIRMED` (primary source or run), `SECONDARY`,
     `UNVERIFIED` (inference, say what would settle it);
   - **Evidence** sections, each claim followed by its citation;
   - **Fit** for this engine (constraints above) and, for a technique, a ranked recommendation;
   - **Open questions**, each with the cheapest way to settle it (probe kit, test, measurement);
   - **Sources** list at the end, with access dates for web pages.
5. **Verify before reporting.** Done when every claim in Answer has a citation or a confidence label, every path and URL was
   opened once, and the file is under the right home. Add a row to `docs/README.md` or `docs/PROBES.md` only when the report
   created a new kind of document; a probe result goes in its `docs/re/` spec.
6. **Report back** with the file link, the three-line answer and the open questions. A decision the answer calls for goes to
   Gabriel as a `docs/DECISIONS.md` proposal; a follow-up goes on the roadmap (`roadmap_store.py backlog add ... --from <group>`),
   not into the report as a to-do list.

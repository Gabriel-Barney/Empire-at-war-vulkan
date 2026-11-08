# Documentation index

Every project document lives under `docs/`, one place per kind of information (centralised 2026-11-06). Before writing something new, find
its kind below and add it there instead of starting a second copy.

**Status is not in `docs/`.** What is open, done, next or blocked, the flowchart and the build records live only on the roadmap
(`roadmap-store/ROADMAP.html`; rules in [`ROADMAP_PROTOCOL.md`](ROADMAP_PROTOCOL.md)).

| Kind of information | The one place | Notes |
|---|---|---|
| Binding decisions (scope, platform, parity, dependencies, camera / controls, graphics) | [`DECISIONS.md`](DECISIONS.md) | Add a dated row for every new decision; out-of-scope work is recorded here before it is dropped from the roadmap |
| Side goals deferred until the core game and mod platform are complete (shader precompile, loading and build speed, CPU / GPU efficiency, graphics fidelity): definitions and re-analysis notes | [`DEFERRED_SIDE_GOALS.md`](DEFERRED_SIDE_GOALS.md) | Not scheduled; restart only by a new row in `DECISIONS.md` |
| Build, test, gate, sanitizers, worker-count check, compiler cache | [`BUILD.md`](BUILD.md) | `CLAUDE.md` keeps only the short rules |
| Performance measurements and scheduling results | [`PERFORMANCE.md`](PERFORMANCE.md) | Plans and research: [`OPT1_PLAN.md`](OPT1_PLAN.md), [`OPT_RESEARCH.md`](OPT_RESEARCH.md) |
| In-game probes: which kit asks what, where findings go | [`PROBES.md`](PROBES.md) | Kit READMEs under `tools/probe_mod/` say how to run; findings go in `docs/re/` |
| Roadmap rules and field list | [`ROADMAP_PROTOCOL.md`](ROADMAP_PROTOCOL.md) | The roadmap itself is `roadmap-store/` |
| Behaviour of the original game (specs recovered from the binary) | [`re/`](re/) | Clean-room: engine code is written from these, never from decompiled output. `re/AUDIT_2026-10-30.md` is the status of each file |
| Mod loading: what makes a mod loadable, file resolution, mechanisms, load reports, the no-mod-specific-code rule | [`MODS.md`](MODS.md) | The decision is in `DECISIONS.md`; status of each part on the roadmap (group `modc`) |
| Phase 4 (the game): step map and open per-step port choices | [`PHASE4.md`](PHASE4.md) | Finished-work notes and old build / fix logs: [`PHASE4_HISTORY.md`](PHASE4_HISTORY.md) |
| Phase 3 (renderer): results, assumptions, reference | [`PHASE3.md`](PHASE3.md) | Details of finished work: `archive/` |
| Phase 2 (architecture, headless core): results and design notes (determinism contract, file resolution order) | [`PHASE2.md`](PHASE2.md) | |
| Game asset catalog, file formats, shader catalog and porting notes | [`assets/`](assets/), [`formats/`](formats/), [`shaders/`](shaders/) | Generated catalogs: do not edit by hand |
| Mod compatibility: hard-coded names in `engine/` (audit, replacements, the name lint) | [`status/modc-audit.md`](status/modc-audit.md) | The mod-loading contract is `MODS.md` (PR #92); the audit becomes its section 9 when that lands |
| Playtest QA notes | [`qa/`](qa/) | |
| Dated one-off reports | [`PRODUCTION_AUDIT_2026-10-30.md`](PRODUCTION_AUDIT_2026-10-30.md), [`PERF_AUDIT_2026-11-07.md`](PERF_AUDIT_2026-11-07.md) (CPU / GPU performance audit and research, input for the deferred side goals) | History, not kept current |
| Old versions of moved or trimmed documents | [`archive/`](archive/) | Zips |

## Where a new fact goes

- A decision from Gabriel → `DECISIONS.md`.
- A timing, budget or speed-up → `PERFORMANCE.md`.
- A build, test or tooling rule → `BUILD.md` (and one line in `CLAUDE.md` only if agents must follow it every time).
- What the original game does → the matching `re/` spec (a new file only when no spec covers the subject).
- A port choice made where the spec was silent → the step's entry in `PHASE4.md`; when the step is finished, move it to `PHASE4_HISTORY.md`.
- How mods are found, resolved or reported, or a port rule that applies only with a mod loaded → `MODS.md`.
- A probe result → the `re/` spec the probe names (see `PROBES.md`), not only the kit README.
- Whether something is done or open → the roadmap, nothing else.

Repository-level files: [`../README.md`](../README.md) (public overview) and [`../CLAUDE.md`](../CLAUDE.md) (working rules for agents).

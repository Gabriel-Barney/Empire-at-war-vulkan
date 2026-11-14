---
name: writing-for-agents
description: Rules for writing or trimming anything an agent reads - CLAUDE.md, .claude/skills, .claude/agents, memory files, docs reached through a pointer. Use when creating or editing one of those files, when CLAUDE.md or a skill grows long or is ignored, or when a retro finds an instruction that changed nothing.
---
# Writing for agents

An agent runs the same process every time it reads a file, so the same levers apply to `CLAUDE.md`, a skill, an agent
definition and a doc behind a pointer. Adapted from `mattpocock/skills` (MIT). Used by the `retro` skill.

## Where a fact goes first

Per `docs/README.md` ("Where a new fact goes"): one place per kind of information. Before writing, find the existing home and add
there. `CLAUDE.md` gets a line only when agents must follow it on every task; everything else is a pointer.

| Material | Home | Loaded |
|---|---|---|
| Rule every agent follows every task | `CLAUDE.md` | always |
| Procedure for one kind of task, with its trigger | `.claude/skills/<name>/SKILL.md` (description always, body on use) | on trigger |
| Role definition for a subagent | `.claude/agents/<name>.md` | when spawned |
| Reference facts (how, why, measurements) | `docs/…` behind a pointer | when read |
| A check a tool can run | a script under `tools/lint/` or a hook in `.claude/settings.json`, not a sentence | always, for free |
| User preference or lesson | the memory directory, one fact per file, indexed in `MEMORY.md` | index always |

A mechanical rule written as a sentence is a no-op waiting to happen: make it a check when it can be one.

## Rules

1. **Pointers carry a trigger.** A pointer names the material and when to open it, trigger word first: "Clang check:
   `docs/BUILD.md` ..." not "see BUILD.md for more". One trigger per branch. If a must-read doc is skipped, sharpen the pointer's
   wording before pulling the content inline.
2. **Steps first, reference after.** Ordered steps at the top; frequently needed reference in the file; rare reference behind a
   pointer. Keep a concept's definition, rules and caveats together under one heading.
3. **Every step has a checkable finish condition**, high enough to need real work ("0 warnings on both compilers, `skips:` read",
   not "run the tests"). Vague conditions end early. Sharpen the condition before splitting a sequence; split only at a real
   context boundary such as a hand-off or subagent dispatch.
4. **State the wanted behaviour.** "Use `Edit`" beats "do not use sed". Keep prohibitions for hard guardrails (clean-room,
   determinism, no system-wide installs), and put the reason beside them.
5. **Leading words over explanations.** Use the project's established terms (tier 1/2/3, lane, group, golden, checkpoint, `[port]`
   entry, clean-room) and define a coined term once, where it is introduced.
6. **One home per meaning.** Do not restate what a config file, script `--help` or a lookup already shows (command flags, test
   names, directory listings). Document what an agent cannot discover: unwritten conventions and the reasons behind choices.
7. **Prune.** Delete a line that describes default model behaviour, that no longer matters, or that nothing in the last sessions
   obeyed or needed. If a sentence fails any of these, delete the whole sentence. Dated rows with names of who decided
   (`Gabriel, 2026-11-10`) belong in `docs/DECISIONS.md`; `CLAUDE.md` keeps the rule and a pointer.
8. **Avoid sprawl.** Long files dilute attention even when every line is true. When a file passes about 100 lines of always-loaded
   text, or a skill body passes about 150, move the rare branches behind pointers.

## Review a file you wrote or trimmed

Done when each check passes:
- every instruction can be traced to an observed need, a hard guardrail or a pointer;
- each step ends in something an agent can verify with a command or a file state;
- no fact appears in two files (search for a distinctive phrase);
- every pointer's target exists (`Glob`) and its trigger is the first words;
- frontmatter of a skill has a `description` that starts with what it does and lists the situations that trigger it.

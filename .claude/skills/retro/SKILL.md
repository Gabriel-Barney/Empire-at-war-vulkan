---
name: retro
description: Retrospective on a finished session or lane that proposes improvements to the agent environment (CLAUDE.md, skills, agents, hooks, lint checks, memory, docs pointers), not to the code. User-invoked; use when asked to run a retro, review how a session went, or find why agents keep repeating a mistake or wasting build runs.
argument-hint: [session id, group id, or "last N sessions"]
disable-model-invocation: true
---
# Retro

Find what in the environment made a session slower or worse, and propose the smallest change that fixes it. Output is a ranked
report; nothing is edited until Gabriel picks items. Adapted from `mattpocock/skills` (MIT).

## 1. Load the writing rules

Invoke the `writing-for-agents` skill. Every proposed text change is judged against it, including its "where a fact goes" table.

## 2. Gather evidence from primary sources

Argument: the session or group to review; with none, the current session. Read in this order and stop when the picture is clear:
1. The session transcript: `C:\Users\Gabriel\.claude\projects\<project folder>\<session id>.jsonl` (worktree sessions have their own
   project folder; `Glob` `C:\Users\Gabriel\.claude\projects\*\*.jsonl`, newest first, and match the worktree name). Read it with
   `Grep` for the signals below, not end to end. Never copy secrets, tokens or personal data into the report.
2. The build records and logs: `logs\`, `<tree>\build_state.json`, `roadmap-store\data\` build records (`failure_kind`,
   `golden_drift`, `real_failures`), plus the group's `items\<id>.json` and the activity log in `roadmap-store\data\`.
3. The git history of the branch (`git log origin/dev..HEAD`) and what the commit messages say went wrong.

Signals worth a `Grep`: repeated identical commands, exit 3 repeat refusals, `ALAMO_RERUN`, `C2001`, golden drift, hung test exes,
permission prompts, edits to a file reverted later, a long search for a path that `docs/README.md` already names, a rule in
`CLAUDE.md` the transcript shows being broken.

## 3. Check each category

For each, record the evidence (transcript quote, log line or commit) or drop the category; no evidence, no finding.

| Category | Question | Typical fix |
|---|---|---|
| Navigation | Did the agent hunt for something a pointer would have found? | One pointer, trigger first, in `CLAUDE.md` or the nearest skill |
| Automated checks | Which error would a script have caught? Is an existing check (`tools/lint/*`, hooks in `.claude/settings.json`, build scripts) unwired, slow or ignored? | Wire or extend the check; a repeated mechanical mistake becomes a hook or lint rule, not a sentence |
| Skills and agents | A step done by hand that a skill covers, a skill whose trigger did not fire, or a repeated procedure with no skill | Sharpen the `description`, or propose a new skill |
| `CLAUDE.md` | Rule broken or ignored, rule never needed, rule better as a check, text that belongs in `docs/` | Move, shrink or delete (`writing-for-agents` rules 6 to 8) |
| Tool economy | Whole-log reads, repeated full builds, `slow` tests run while iterating, wide searches that `Grep` or `changed` would shorten | Point at `build_summary` output, `changed`, a regex |
| Permissions | The same read-only command prompted repeatedly | Candidate for the allowlist (`fewer-permission-prompts` skill; read-only commands only) |
| Memory | A lesson the user gave twice, a memory that was wrong or stale | New, merged or deleted memory file (`consolidate-memory` skill) |
| Information access | Information the agent lacked: a log it could not read, a data path, a decision not recorded | A doc entry in its one home, or access |

Review-versus-implementation split: the implementing agent works under heavy context pressure and the reviewer sees only a diff,
so judgment-call standards belong where `code-review` and the `reviewer` agent read them, and mechanical ones in checks.

## 4. Report

Done when the report lists, ordered by severity (wasted hours or broken landings first, tidy-ups last), for each candidate:
the evidence, the proposed change with the exact file and wording or script, and the size of the fix. End with the three you
would do first. Do not edit files, memory or settings; do not land anything. Gabriel picks, and the picked items become a task
or backlog entry (`roadmap_store.py backlog add ... --from <group>`), following `docs/ROADMAP_PROTOCOL.md`.

# Workflow tips: Claude Code features that speed up lanes

Adopted 2026-11-13 (Gabriel) from a review of Claude Code features against this project. Features the project already uses
(`CLAUDE.md`, skills, subagents, `/code-review` in `finish-group`, the clangd plugin) are not repeated here. Agents follow the
short version in `.claude/agents/lane.md` ("Speed habits"); this page is the reference for Gabriel and for new sessions.

| Feature | Use here | Where it is wired |
|---|---|---|
| `/loop` | Unattended "build, read summary, fix, rebuild" until green: `/loop /green-fast-tier <modules>`. Self-paced (no interval): a build takes minutes. Never for CI or PR polling (the app's Auto-fix does that). | `.claude/skills/green-fast-tier/` |
| `/goal` | Gives a lane a checkable stop condition: the group's `exit` test plus a clean `roadmap_store.py lint`. Clear it once the exit test passed. | `start-group` step 6 |
| `/context`, `/compact` | Docs and logs are large. Check `/context` at lane start and between tasks; compact above half full, keeping group id, claim, exit test, open items and the last build summary. | `start-group` step 6, `finish-group` step 8, `lane.md` |
| `/btw` | A side question (what does this flag do, where is X) without adding to the lane's context or interrupting it. | `lane.md` |
| Shell mode (`!`) | Fixed commands with no model round trip: `! python roadmap-store/roadmap_store.py claims`, `! python -I tools/lint/test_slot.py --status`, `! git status`. | this page |
| `/teleport`, Remote Control | Watch or steer a running lane from a phone or another machine while a long build or gate runs; at most 3 lane sessions run at once (2026-11-13). `/teleport` pulls a cloud session into the local checkout. Remote Control is a per-session setting in the app. | this page |

## Rules for the loop skill

- It never regenerates goldens. A run that fails only on golden drift stops and asks (`regen-goldens` skill is the way to change them).
- It changes the code between runs: an identical failing rerun is refused (exit 3). `ALAMO_RERUN=1` only for a flaky test or a changed
  environment, and the report says so.
- It has an iteration cap (default 6) and stops when the same test fails three iterations running.
- Findings outside the group go to the backlog, not into the loop's fixes.

## Not adopted

`/voice` (no speed gain here), third-party workflow plugins (2026-11-13: they repeat the lane loop), `/init` (`CLAUDE.md` exists).

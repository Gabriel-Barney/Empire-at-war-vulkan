# Lua script output functions in retail FoC 1.121 (q2f t06)

Question (roadmap q2f t06): the Q2 Lua probe (`tools/probe_mod/q2_traces`, `-LuaProbe`, runs of 2026-10-30) wrote no
`Q2Trace_Lua.txt` anywhere. Does `_CustomScriptMessage` write a file in the retail exe, or did the probe script never
load?

Answer: **`_CustomScriptMessage` writes nothing in retail.** Its file writer is an empty function. The 2026-10-30 runs
could not show whether the scripts loaded; the probe now proves that separately (canary file and memory records, below).

Source: Ghidra decompile of Steam FoC x64 1.121 (`re_reference/ghidra/StarWarsG_decompiled.zip`, `Global.c`), RVAs
relative to the image base. Confidence [H] unless marked.

## 1. The functions

| Lua global | Lua_Call RVA | what it does in 1.121 | output |
|---|---|---|---|
| `_CustomScriptMessage(file, text)` | 0x255320 | needs exactly 2 string arguments (else the two `LuaCustomScriptMessage -- ...` errors). Inserts `file` into a global hash set (0xb0a020, insert helper 0x2542d0). **On the first insert of a name: `DeleteFileA(file)`**, relative to the process's working folder. Then calls 0x254b20(file, fmt 0x816db4, text): **an empty function (`ret`)**. Then passes `text` to the script-message path 0x248420 | none; deletes `file` once per name per process |
| `_ScriptMessage(text)` | 0x255620 | first argument must be a string; passes it to 0x248420 | none |
| `_OuputDebug(text)` | 0x255500 | first argument must be a string; calls 0x25760(0x802464, text): **an empty function** | none |
| `_MessagePopup(...)` | (vftable 0x857808) | Lua_Call body not traced. Empirical: no message box appeared in the TSW probe runs (2026-10-31) | none seen |
| `DebugMessage(fmt, ...)` (PGBase Lua) | Lua | formats, then `_ScriptMessage` + `_OuputDebug` (as `GameScoringMessage` does) | none |
| `Game_Message(text_id)` | | shows a localized text line on screen (works: S10 / TSW probes) | screen only, fixed text IDs |

The script-message path 0x248420 formats `LuaScript: "%s", Message: %s` (script name, text; the text is used as a
printf format) and hands the line to a callback pointer at 0xb09d38, but only if that pointer is non-null. The only
store to 0xb09d38 in the decompile is `= 0` in 0x499340 (an init routine that also clears 0xb09d40..0xb09d58). In
the shipped build no logger is installed, so the line is formatted and dropped. [H for the decompile; a store through
a computed pointer cannot be excluded, but no run has ever shown such output.]

Script errors take the sibling path 0x248190: with its callback 0xb09d40 null it only sets byte +0x121 of the script
object and returns. [M] That flag is likely the "script failed" mark, so a binding error (wrong argument types) may
stop the calling script. The probes therefore call every binding inside `pcall`.

## 2. Consequences for probes

- No retail Lua function writes a file or a log. Every probe output channel has to be outside Lua:
  - process memory: the probe keeps its lines as Lua strings in a table, and a read-only reader finds them by
    scanning the game's committed private memory (`tools/probe_mod/a1_tactical/a1mem.py` `Proc.scan`; proven in the
    A1 run of 2026-10-31, 175 records). Lua 5.0 strings are stored contiguously, so the ASCII text is found as is;
  - `Game_Message` on screen, with fixed text IDs only.
- **Load proof without a reader:** `_CustomScriptMessage` deletes the named file on its first call for that name.
  A probe that calls it once on a file the kit installed (for example `Mods\<probe>\canary.txt`, relative to the game's
  working folder `corruption\`) leaves proof that the script ran: the file is gone after the run.
- Plot-level story scripts (`<Lua_Script>` of a `Story_Mode_Plots` file) do run their `main()` state machine (TSW
  probe 2026-10-31: `PGStoryMode` states entered). GameScoring's `GameService` is never called, because its `main()`
  loop does not run (TSW probe, same day); only its event functions (`Game_Mode_Starting_Event`,
  `Tactical_Unit_Destroyed_Event`, ...) are called.

## 3. What changed in the Q2 probe

`tools/probe_mod/q2_traces` (README "Lua probe"):
- both probe scripts keep every line as `Q2L1|<G or T>|<sid>|<seq>|<KIND>|<text>|END` in `Q2L_Records`;
  `q2lua_reader.py` collects them during the run into `lua_records.txt`, `lua_perception.tsv` and `lua_summary.json`;
- each script calls `_CustomScriptMessage` once on its canary (`Mods\Q2Trace\Q2Trace_Canary_Galactic.txt`,
  `..._Scoring.txt`); `run_status.json` -> `lua_canary` says which were deleted;
- the galactic script now loads `PGStoryMode` (the story engine may call `Story_Event_Trigger` on the plot's
  script) and adds the shipped equation `Roaming_Empire_Fleet_Target` as a control that needs no mod equation file;
- the GameScoring hook samples on events and from a `Create_Thread` loop, since its `GameService` never runs.
- `selftest_lua.py` runs both scripts under stock Lua 5.0.2 and checks that a memory scan of that process finds every
  record.

Open until the next run: whether the mod's override of `Story_Plots_Sandbox_12U_Empire.xml` is picked up
(canary `galactic`), whether the Q2_* equations from the mod's `AI_Equations_ExpansionGeneric_Galactic.xml` evaluate
(`ERR` records vs the control column), and whether a GameScoring `Create_Thread` thread is serviced (`THREAD start`).

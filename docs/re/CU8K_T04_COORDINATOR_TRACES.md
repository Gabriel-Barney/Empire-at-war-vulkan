# cu8k t04: coordinator ctor draw traces (MOVEMENT §4.7.6), retail 2026-11-09

Kit: `tools/probe_mod/cu8h_probes/cu8h_dbg.py` (hardware write watchpoint on the synced LCG state), run directly with
`--arm-file` / `--stop-file` so the session could be driven by computer use (the `run_cu8h_trace.cmd` Enter prompt
needs a console). Vanilla game, no mod. Driven by Claude during the 2026-11-08 overnight probe session. Listing method:
the one-liner of MOVEMENT §4.7.6 (hits whose stack continues `40d11f,4f6fd4,`).

## Item 2: skirmish land load, `_MP_LAND_ENDOR` ((6) Endor Xpress)

Run `tools/probe_mod/cu8h_probes/out/20261109-001746-trace-cu8k-endor-land` (`trace.log`, `report.txt`). You EMPIRE,
Easy AI REBEL, defaults; armed just before Start Game, stopped at mode frame 120. 45977 hits.
`cu8h_dbg_parse.py` verdict: `VERDICT t01: A B C` (chunk-0x102 draws n503..n3360, one environment pick `int(0, 3)` = 0,
425 post-load SKIRMISH_FORCES draws, no weather).

Coordinator ctor draws:

| count | first mf | creator chain after the factory | reading |
|---|---|---|---|
| 1 | 0 (gf 0, n 3362, right after the environment pick) | `4f738a` degenerate ← `43b502` ← vf8 return `39bda0` ← **`395421`** ← `2b44a7` | one degenerate draw through an adopt whose caller returns at 0x395421 |
| 18 | 0 (gf 1, from n 4076) | `43e712` land composition ← `4f80a8` ← `426f28` coordinator update ← `363c83` common tick | 18 compositions at the first coordinator update |

**Differs from the §4.7.6 expectation** ("one draw per `Create_Team` chunk under `0x2a4fc0`, adopt return `0x2a61d5`"):
no hit has 0x2a61d5 or 0x2a4fc0 in its stack. The starting forces make **one** degenerate draw (caller 0x395421), and
the teams get their ctor draws from **land compositions in the first coordinator update** (gf 1), not at their creation.

## Item 3: space group move (partial: two fighter squadrons only)

Run `tools/probe_mod/cu8h_probes/out/20261109-002053-trace-cu8k-space-move`. Space skirmish Kuat, you EMPIRE, Easy AI
REBEL. A skirmish starts with only two TIE squadrons (no capital ship or frigate), so the move covered just those:
box-select both, arm, one right-click move, stop file 4 s later. 937 hits over mode frames 861..1191.

| count | mf | creator chain | reading |
|---|---|---|---|
| 1 | 933 | `5d5c19` space composition ← `4f80a8` ← `5d51f7` (space vf5) ← `426f28` ← `363c83` | the two squadrons merge into one role-0 group: **1 draw**, as §4.7.6 expects |

## Not run

- Item 1 (Kashyyyk space load) and item 4 (jetpack): no scenario was set up for them in this session.
- Item 3 with a capital ship and a frigate (expected 2 draws for the ships plus 1 per role-0 group).
- The Luke saber run with the extended memory watcher (CU8F §2.7 item 6): needs a kit change first.

## Follow-ups

- [RE] Name the adopt caller returning at 0x395421 (skirmish start forces) and correct §4.7.6 item 2 / §4.7.2.
- [port] Check that the port's land skirmish start draws one degenerate ctor draw plus one land-composition draw per
  team at the first coordinator update (gf 1), instead of per-`Create_Team` draws.
- [probe] Items 1 and 4, item 3 with ships (build a frigate and a capital ship first, or use a campaign space battle),
  and the Luke saber watcher run.
</content>
</invoke>

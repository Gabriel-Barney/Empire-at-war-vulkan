# cu9h t02: bombing-run HUD probe, retail run 2026-11-09

Kit: `tools/probe_mod/cu9h_bombing` (README "Steps in the game"). Run: `tools/probe_mod/cu9h_bombing/out/20261109-000853`
(`report.txt`, `marks.log`, `reader.log`, `dbg.log`, screenshots and zooms in `shots/` as `mcp-computer-use-blob-<id>.jpg`).
Driven by Claude with computer use during the 2026-11-08 overnight probe session (Gabriel's go-ahead).

Setup: GC "CU9H PROBE: BOMBING RUN", REBEL, Medium. The story invaded Tatooine by itself; Begin, Begin. Cooldown patched to
60 s (`rec_int=1800` frames). Battle from mf 0 to mf 6028; one bombing run fired at mf 3603.

## Answers to HUD_GAPS §9.9

| # | Question | Retail | Evidence | Port today |
|---|---|---|---|---|
| 1 | Dial at 1.0 when ready: full `i_button_build` overlay or nothing? | **Nothing.** At READY (`button=ENABLED dial=100`) the button looks like a plain enabled button (whole face bright, icon dark), the same as the other enabled buttons in the column. | zooms `1794201201288-ysj8q7`, `-7ngutn` (q1_ready), `1794201332046-7anea2` (q1_ready_2) | **MATCH**: no dial while ready (`hud_feeds.cpp:33-41`) |
| 1b | Dial during the cooldown | A clock wipe from 12 o'clock, clockwise: a **dark** sector covers `dial`% of the face and the rest stays bright. dial 9: a thin dark line at 12 o'clock; 22: small dark wedge; 52: right half dark; 71 / 79: dark except a small bright wedge at the top left. The dial value equals the §9.3 formula on all 701 compared samples (0 differ by more than 1). | `1794201146713-vklpuj` (22), `1794201256936-fnrxik` (9, RUN_IN_PROGRESS), `1794201275973-ompjn2`, `1794201290952-mcfbo8` (52), `1794201178855-wnfeij` (79) | value MATCH; check the drawn direction / which part is dark against the port's `build_fill` (`tactical_hud.cpp:934-944`, `hud_buttons.cpp:220-221`) |
| 2 | Blob colours | GREEN (0,1,0,1) on open ground, RED (1,0,0,1) on fogged ground; screenshots show green and red, so the RGBA order is as read. A left click on a fogged point keeps targeting on (`targeting=1` after `q2_click_fog`). | marks q2_open_1/2, q2_fog_1/2, q2_click_fog; shots `1794201212229-1do5wo`, `1794201219757-i6wjkq` (green), `1794201228157-a72sit`, `1794201234517-3cci8m` (red), `1794201240598-4od63s` | **MATCH** (`app_input.cpp:689`, `:718-719`) |
| 3 | Does a click on the disabled button run its callback? | **CALLBACK_NOT_RUN.** Two clicks during the cooldown (dial 22 and 42): no write to the targeting flag +0x20b2 while disabled, normal cursor, no blob. Control: the enabled click at mf 2289 wrote `flag:=1` via `targeting_toggle(0x2e7950) <- 0x34cf90 <- hud_mouse(0x2f6c30) <- input_dispatch(0x60c60)`. | report Q3; shots `1794201153263-w48v7j`, `1794201163249-gogxw4` | **MATCH** (`hud_buttons.cpp:97`) |
| 4 | Writer of the HUD ground point +0x26dc | `world_mouse_move(0x2ee40)` only (x and z, 240 writes each, 120 while targeting). | report Q4, `dbg.log` | port choice (ray hit re-intersected at terrain height, `app_input.cpp:676-687`); retail writes it from the per-frame world mouse update, as the static reading said |

Side observations:

- The run is in progress from mf 3603 to 3928; the cooldown then starts with `ready=5728` (= 3928 + 1800) and the button
  is enabled again at mf 5729. The first cooldown sample shows dial 15. While the run is in progress the button is
  disabled and still shows a dial (dial 9 at the q1_run_zoom mark, `dial_expected=-`).
- The tooltip over the button is "Call for Bombing Run".
- Item 3's "no Select_Target sound" was not listened for; the watchpoint answer replaces it.

## Follow-ups

- [port] Compare the drawn cooldown dial with item 1b (direction clockwise from 12 o'clock, dark part = `dial`%) and the
  dial during RUN_IN_PROGRESS (retail draws one; check the port's `bombing_run_button` feed for the in-progress state).
- [RE] The dial value while the run is in progress (`dial_expected=-` in the kit: the §9.3 formula does not cover it).
  **Answered (cu9h t04, HUD_GAPS §9.11):** the same 0x495250 rule with R = now + interval + int(longest remaining
  flight): dial = (now − start) / ((now − start) + interval + int(F)), a ramp that runs up from the start (full on the
  start frame only) and continues into the cooldown without a jump; all 17 in-progress samples here equal
  100 × (mf − 3603) / 2125. The port already computes it; the kit's `expected_dial` could use it.
</content>
</invoke>

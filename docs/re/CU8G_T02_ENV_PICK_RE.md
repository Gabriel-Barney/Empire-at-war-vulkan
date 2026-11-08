# CU8G t02 (+ t03 side questions): map-load environment pick, synced draw order, weather particles (RE, 2026-11-08)

Source: the decompiled retail exe (Ghidra output, `re_reference/`), the disassembly of `StarWarsG.exe` (FoC, Steam),
the 212 FoC `.ted` maps (`maps.meg` with `Patch.meg` / `Patch2.meg` overrides), the 5 weather particle ALOs
(EaW `models.meg`) and the FoC XML. Behaviour only, no code. Builds on CU8F_T03_T05_RE §2, MAIN_LOOP §2.2 / §4,
BATTLE_TRANSITIONS §2.1, REMAINING_GAPS §3.1, TACTICAL_GAPS §4.3, L1_S4_REMAINING §3.2, SKIRMISH_SETUP §2.
Confidence: [H] read in the code (and the disassembly where noted), [M] inferred from several consistent reads,
[L] hypothesis.

"Synced draw" = the lockstep LCG 0x140a13e24 (MAIN_LOOP §4.1). `int(lo, hi)` = 0x1ffb40 (rejection loop: one draw
can take several LCG steps), `float(lo, hi)` = 0x1ffbb0 (one step). "Free" = 0x140a13e20.

**Summary.**
- The environment pick is the **last synced draw of the map loader** (land 0x3b8cd0, space 0x4d80b0). It comes
  after every map-object draw of chunk `0x102` (object construction, `Map_Load_Spawn_Table`, the queued spawn-table
  objects), and before every post-load draw (skirmish start forces, campaign station-marker pick, skydome objects).
- The land-mode creation call 0x3b81b0 runs the same random pick, but on a scene that holds only the default
  environment (n = 1), so it **never draws**. Its weather bind finds no scenario and does not draw either.
- "Skip Cinematic" = draw over all n, then step **one** record forward if the drawn record's name is exactly
  `Cinematic` (case-sensitive `strcmp`), wrapping to 0. There is no redraw and no re-check.
- The pick writes RtsScene +0xa0. **A save game does not store it**: a loaded save reloads the map with the
  map's own start index (`0x100/8` mini `25`) and does not draw for it [M].
- The picked environment drives later synced draws: on the **first land tick** the weather service restarts
  for the new environment, and a restart with a scenario costs **two** synced float draws (new finding, §4).
- Weather particles: +0x10dc / +0x10e0 are the ALO emitter minis `43` `inherit_velocity` (bool) and `28`
  `inherit_velocity_factor` (f32). Step velocity = v + [43]·f28·E + [31]·5·W. The inherit term is off in all 5
  shipped weather emitters.
- `Set_New_Environment` makes no synced draw itself. It re-applies lighting, fog, bloom and shadow colour, and on
  land also clouds and wind. It does not respawn skydomes. Weather particles, lightning and weather sound follow at
  the next land tick through the weather service, and that restart draws (§4). The typed pick (land / space vf27)
  is a synced pick with no caller found.

---

## 1. The pick itself: 0x277030 (RtsScene `random environment`) [H, disassembly-checked call order]

Environment table: RtsScene +0x88 (DynamicVector, data +0x90, count n at +0x98, records 0x198 bytes, name =
std::string at record +0x00). Current index +0xa0. Saved "pre-cinematic" index +0xa4.

1. +0xa0 := 0.
2. **n == 1:** index 0, **no draw**. This holds even when that record is named `Cinematic`.
3. **n ≠ 1:** i = synced `int(0, n − 1)` (0x1ffb40 on 0x140a13e24). Then:
   - if record[i].name equals `"Cinematic"` (`strcmp`: exact, case-sensitive, no trimming), i := i + 1;
   - if i ≥ n, i := 0;
   - clamp i to [0, n − 1].
   There is **no second test**: the record after a `Cinematic` record is taken even if it is also `Cinematic`.
   The skip is therefore not uniform. The record after a `Cinematic` record (record 0 when `Cinematic` is last)
   gets double weight. An all-`Cinematic` table with n ≥ 2 still draws once and ends on a `Cinematic` record.
4. +0xa0 := i, then 0x2774f0 (rebuild the 3 light directions from the stored yaw/elevation) and 0x277830 (apply
   lights, SH, fog, bloom, shadow colour; TERRAIN_WATER_POST §5.3).
5. **n == 0** (a chunk `0x100` with no records; no shipped map has one) is undefined: the code draws `int(0, −1)`
   (one swapped draw), reads a record out of range and ends with index −1. A map without chunk `0x100` keeps the
   ctor's default record (n = 1).

Land vf26 (0x3bbcd0) then calls 0x3bbed0 (terrain clouds + scene wind, CU8F_T03_T05 §2.1), which does nothing
while the land mode has no terrain (+0x338 = 0). Space vf26 (0x4d9790) is only 0x277030.

**The draw:** range r = n − 1, mask = (smallest power of two above r) − 1, repeat LCG steps until
`((s >> 10) & 0x7fff) & mask ≤ r`. So n = 2 takes exactly one step. n = 3, 5 and 6 can take more than one.

**Shipped FoC data** (212 maps, patches applied):
- 144 maps have n = 1 (no draw).
- 68 maps have n ≥ 2: 67 land maps (n = 2…6) and one space map, `_MP_SPACE_KUAT` (`Environment 1`, `Cinematic`).
- Kuat therefore always ends on `Environment 1`, but still costs one synced step. Every other space map draws
  nothing.
- No other map has a `Cinematic` record, and no map has a case variant of the name.
- 43 of the multi-environment land maps give their records **different skydome types** (TEE `19` / `1A`), and
  most give them different weather scenarios (TEE `29`, e.g. Endor `Sunrise_Rain` / `Noon_Rain` → `RAIN_RANDOM`,
  the `_Clear` records → none).

## 2. Where the chosen index lives, and save games

- **Storage:** RtsScene (mode +0x328) +0xa0. The map file's start index `0x100/8` mini `25` is parsed into the
  same field (0x2734c0) and then overwritten by the pick. Mini `2A` goes to +0xa4 (−1 in the maps), the field
  that `Set_Cinematic_Environment` uses to remember the index it replaced. [H]
- **Map-load rule** (land 0x3b8cd0 tail, space 0x4d80b0 tail): if 0x140b15490 is clear **and** the loader's "keep"
  argument ≠ 1 → vf26 (random pick). Otherwise → vf25(+0xa0), the stored index, with no draw. `Load_Map`
  (0x56480) passes keep = 0 for world chunk `1`. It passes 1 only for a top-level chunk `0x201`, which no shipped
  map has. [H]
- **Save game:** the tactical save (0x362f60, mode chunk vf+0x80 = land 0x3ba070 / space 0x4d89a0, weather chunk
  0x14 = 0x469ff0) writes **no** RtsScene environment data. The only writer of `0x100/8` mini `25` is the map
  writer 0x275730, called from the map-save vfuncs (land vf+0x70 0x3bafd0, space 0x4d9300). Those are reached
  from no game-save path found. [H for the writers, M for "not saved"]
- **Save load:** the tactical load 0x35f970 calls mode vf+0x90 = 0x360970 ("Loading base map"). It reloads the
  map with keep = 1, so the environment becomes vf25(**the map's mini `25` start index**), with no draw.
  - The weather chunk load (0x468f80) reads the saved scenario name but never uses it, so it cannot restore the
    environment either.
  - A reloaded battle therefore shows the map's start environment, not the one picked at battle start. [M]
  - Draws on that path: 0x360970 rebinds the weather with force (0x46a2f0 → 0x468a50: 2 synced floats when the
    start environment has a scenario), then the chunk-0x14 load runs 0x468a50(force) again (2 more) and one
    advance (1). [H for the calls, M for the counts]

## 3. Synced draw order of a tactical map load [H per step unless tagged]

Common core, `Load_Map` 0x56480. All callers go through it. Top-level chunks are read in file order (`0`, `3`,
`2`, `1` in the shipped maps).

| # | Step | Synced draws |
|---|---|---|
| 1 | chunk `0`: map info minis | none |
| 2 | chunk `1` start: set game type (0x28ab50); **Start_Game(map type, no record)** 0x28e890 | **New session only** (stack empty: skirmish, MP, menu background, command-line / dialog / recording loads): the synced LCG is **seeded** here from GMM +0xe0 / +0xe4. Campaign, sandbox and story battles do **not** reseed: the stream continues from the galaxy (MAIN_LOOP §2.2) |
| 3 | inside Start_Game: mode init vf+0x08. **Land 0x3b81b0:** (SP, not MP, 0x140b15490 / 0x140b1574e clear) STORY_LAND_TACTICAL dispatch to the current story manager and loading of the queued battle script 0x140b30548 (0x329aa0: Story_Plots load, Lua definitions run, `main` threads created, not run). Then: new RtsScene (1 default record), **vf26 → 0x277030 with n = 1: no draw**; 0x3bbed0 skipped (no terrain); weather bind 0x46a2f0(force) → the default record's weather name is empty → lookup 0x489580 returns null → weather off, **no draw**. **Space 0x4d7820:** same story part, no environment call, fighter grid 0x4faf40 | none from the engine [H]. A plot script whose top-level code calls `GameRandom` would draw here [L, shipped scripts not checked] |
| 4 | Start_Game new-session player setup (0x292050 / 0x291f70 / 0x292590 / 0x291d80) | none for tactical modes (0x292050 only creates objects in galactic mode; the tech sweep 0x288980 reaches only audio, which uses the free RNG) |
| 5 | 0x364f00: map terrain type → weather audio | free RNG only |
| 6 | **mode load vf+0x78** (land 0x3b8cd0 / space 0x4d80b0), world children in file order. `0x100` environment set: none. `0x101` terrain, `0x103`, `0x10A`, `0x10B`, `0x10C`: none. **`0x102` objects** (vf+0x68 → 0x35f970 → GOM loader 0x2b0a20), see 6a–6c | see below |
| 6a | per object record (`0x459`), in file order: parse 0x39c3a0 → 0x39ce40 → Init_From_Type 0x3989a0 (**hardpoint ctor 0x381a90, SAP stagger 0x37c050**); base-level filter / branched discard (REMAINING_GAPS §3.1 items 3–4); finish 0x3a0930 (→ 0x3a4820, **behaviour-attach stagger**, anim start frames) and GOM add | construction draws, TACTICAL_GAPS §4.3. The parse draws run **before** the base-level / discard test, so a discarded object still consumes its hardpoint / SAP draws [M] |
| 6b | same record, when its type has `Map_Load_Spawn_Table` and 0x140b15490 is clear: 0x378d40 | synced float01, then float(0, W) only if it spawns (REMAINING_GAPS §3.1 item 5); the spawn is **queued**, the marker destroyed |
| 6c | after the last record: every queued spawn created in queue order (0x29f810) | construction draws of the spawned objects |
| 7 | loader tail: playable box, passability recompute, camera; **ENVIRONMENT PICK** (vf26 → 0x277030: **one synced `int(0, n − 1)` when n ≠ 1**; then lighting apply; land: 0x3bbed0 clouds + wind); 0x360d70 progress callback. Space: the pick, then the fighter grids 0x4faf40 | **the pick** |
| 8 | `Load_Map`: vf+0x248 (land 0x3b86f0), FOW dense mask 0x35e870 | none |
| 9 | post-load start vf+0x2a0 (land 0x3b7af0 / space 0x4d75a0). **Called directly** when `Load_Map`'s last argument is 1 (menu background 0x22f10, command-line 0x68b20, recordings 0x69040, LoadMapDialog 0xc0110, …). **Otherwise queued** with 0x13cad0 and run on the main thread by the load driver 0x8df00 → 0x13cb90 after the LoadThread ends. It runs: GUI setup, 0x35d540 → **vf59 environment skydomes** (land 0x3bc1c0 / space 0x4d98c0: object(s) of the **picked** environment's TEE `19` / `1A` types created through 0x29f810 for the neutral player), then mode vf+0x20 (0x35cc10, mode-starting signal 0x23 → Lua `Game_Mode_Starting_Event`) unless 0x1fecd0 | skydome creation draws, if the type has drawing behaviours / clips: the shipped skydomes are `SKY_DOME` decorations, probably 0 draws [M]; Lua handlers of signal 0x23 [L] |
| 10 | `Load_Map` tail: direct loads (last argument 1): 0x28bc80, GOM post-load 0x2a8730, object combining 0x29dc30 (can kill → death spawn tables). Skirmish / MP space: standalone space special weapons created (0x29f810) | creation draws |
| 11 | caller-specific post-load: see the table below | see below |
| 12 | **first tick**, GMM Update → land tick 0x3bb440: per-player AI data 0x527dd0, 0x59d270, then **weather service 0x46a0c0 → 0x468a50**. The index differs from the bound one, so the weather **restarts**. If the picked record has a scenario with ≥ 1 phase: **2 synced `float(0.3·D, D)`** (D = phase-0 `Duration`, §4). Then music (free RNG), then the common tick 0x3639d0 (object services …). Space: no weather system | 2 or 0 |

**What differs per load kind:**

| Kind | Seed | Before the pick | After the pick |
|---|---|---|---|
| Skirmish (SP lobby 0xeed80, MP 0xc9e10, demo 0xbd380) | reseeded in step 2 | step 6 only (no story dispatch in MP; SP skirmish dispatches to an empty current story) | 0xeed80 / 0xc9e10 / 0xbd380: owner remap 0x294fa0 (removals), 0x2a74f0, mode vf+0x250 / +0x248, credits, **starting forces 0x2a4fc0** (company creation draws), victory records, vf+0x20, 0x2b3e00 (SKIRMISH_SETUP §2). The order of these against step 9 was not traced [L] |
| Campaign / sandbox battle (0x28fda0 → GMM+0xa0 → BattleLoadDialog → LoadThread) | none (galactic stream) | step 3 story plot load (SP), step 6 | step 9 (load driver) [M for its order vs. the next row], then BATTLE_TRANSITIONS §2.1 step 4: **0x2a1eb0** (space-station marker pick `int(0, n − 1)` when ≥ 2, then structure / unit spawns: creation draws), 0x35cd90, 0x2aafb0 (anim variant draws through 0x501050 → 0x3a92f0), 0x2b3e00, victory conditions, spawns, story hand-off |
| Story tactical maps (`STORY_LOAD_TACTICAL_MAP`, LINK_TACTICAL, queued record 0x28f590) | as campaign | as campaign | as campaign. These are the same `Load_Map` path; only the map name differs |
| Direct map loads (menu background, command line, LoadMapDialog, recordings) | reseeded | step 6 | steps 9 and 10 run inside `Load_Map`, directly after step 8 |

The environment pick is therefore always: **after** every chunk-`0x102` draw and **before** every post-load draw
of the caller. Within one load it is exactly one `int(0, n − 1)`, or nothing when n = 1. 0x3b81b0 contributes
no draw.

## 4. Weather restart: two synced draws (new) [H, disassembly 0x468a50]

0x468a50(sys, force) does nothing when force = 0 and a scenario is bound and the stored index (+0x0) equals
RtsScene +0xa0 and the scenario name is unchanged. Otherwise it looks the record's `29` name up
(0x489580: upper-cased, CRC key; **an empty name returns null**):

- **no scenario:**
  - when a scenario was bound before: weather audio → CLEAR (0x323110);
  - emitter removed; no draw;
  - the stored index is **not** updated, so the lookup repeats every tick (harmless, no draws).
- **scenario with 0 phases:** returns, no draw.
- **scenario with phases:**
  - stored index := +0xa0; phase 0 / 1 chosen; type and intensity set; weather audio set;
  - start phase 0x468980 (call at 0x468deb): synced float; then the FOW-dirty pass when the weather type changed;
  - the old emitter is removed, the scenario's `emitter_name` particle is looked up;
  - **0x468980 again** (call at 0x468ef0): a second synced float that overwrites the phase timer;
  - the emitter is added to the scene.

  Each 0x468980 call: phase timer := `float(0.3·Duration, Duration)` × FPS (L1_S4_REMAINING §3.2), lightning
  := `Lightning_Intensity` (0x276fc0), fog-plane alpha, emitter intensity.

So every weather (re)start costs **2** synced draws. It happens:
- at the first land tick after a load whose picked record has a scenario;
- after every environment change that reaches the weather service with a different index;
- twice more on a save load (§2).

L1_S4_REMAINING §3.2 counts one draw per start phase. That stays right for 0x4690c0 (advance, `Force_Weather`),
which calls 0x468980 once.

## 5. t03: what an environment change re-applies, and its draws

| Call | Index change | Re-applied immediately, in order | Synced draws |
|---|---|---|---|
| Lua `Set_New_Environment(i)` 0x5448a0 → mode vf25 (needs a tactical mode and a scene; i = (int)double) | 0x276ff0: clamp(i, 0, n − 1) | 0x2774f0 light directions → 0x277830: light record (sun / fills / ambient / specular / intensities), SH9 + lightning flash (0x277570, **free** RNG, only while env +0x138 ≠ 0), fixed-function ambient, fog (enable / colour / range, or off by option), bloom cutoff / strength / size → alScene +0x214…+0x21c, shadow colour → +0x1e4, post block 0x1a20b0. **Land only** then 0x3bbed0: terrain cloud scale / velocity / texture, **scene wind** | **none** |
| `random environment of a weather type` = vf27 (land 0x3bbb30, space 0x4d9760) → 0x2770e0(type) | collect, in table order, the records whose `13` (+0x100) == type; m = 1 → that one; m ≥ 2 → **synced `int(0, m − 1)`** (1 draw, possibly several steps); m = 0 or type > 4 → 0x277030 (draw as §1 when n ≠ 1). **No Cinematic skip** in the typed branch | same as above (0x2774f0, 0x277830; land 0x3bbed0) | 0 or 1. **No caller found** (direct, vtable-indirect or Lua; the story reward `SET_WEATHER`, 0x4b, only logs its parameter error) [M] |

Not re-applied by either call:
- **Skydomes:** vf59 runs only from post-load 0x35d540 and the map writers, so the sky objects of the load-time
  environment stay. [H for the callers found]
- **Weather particles, lightning schedule, fog-plane alpha, weather sound:** these follow at the **next land
  tick**. The weather service sees the new index and restarts (§4): scenario lookup, audio fade (0x323110; its
  intermittent sounds use the free RNG), emitter swap, **2 synced draws** when the new record has a scenario with
  phases. When the scenario is unchanged but the index changed, it still restarts (phases from 0, 2 draws).
  Setting the same index again is a no-op.
- **Space:** there is no weather system, no clouds and no wind; only the lighting / fog / bloom block.
- **Music:** chosen from the map terrain type (mode +0x08, 0x28ee00 / 0x28f020, free RNG), not from the
  environment.

`Set_Cinematic_Environment(true)` (0x2769a0) also changes +0xa0, to the `Cinematic` record by exact `strcmp`, and
saves the old index in +0xa4. `false` restores +0xa4. On land this would also restart the weather at the next tick.
The only shipped `Cinematic` record is in a space map, so no shipped draw comes from it. [H]

## 6. t03: weather-particle inherit term and velocity [H]

Emitter desc fields, from the emitter property loader's mini → offset switch (0x15f900):

| desc offset | ALO mini (`700/02`) | SPEC §3.1 name | type |
|---|---|---|---|
| +0x10dc | `43` | `inherit_velocity` | bool |
| +0x10e0 | `28` | `inherit_velocity_factor` | f32 |
| +0x1104 | `31` | `affected_by_wind` | bool |
| +0x087f | `3D` | `is_weather_particle` | bool |

These are binary ALO particle properties. **There are no XML tags for them.** The WeatherScenarios XML only names
the ALO (`emitter_name`).

Weather integrator (0x163a90; the same loop in 0x1670f0), per live particle, with step d:

1. E = emitter-instance velocity (+0x188), refreshed at spawn time from the system:
   - system +0x1f8 = (world translation − previous translation) / dt (0x171120);
   - plus system +0x204, a second vector whose writer was not found [L: an externally set velocity, default 0].
2. W = the scene wind copied into system +0x21c each update (CU8F_T03_T05 §2.3).
3. Step velocity, per axis, in this float order:
   - u = v;
   - if `43`: u = f28·E + u;
   - if `31` (and the system has a scene): u = u + W·5.0.
4. Particle +0x18 := u. **The stored velocity v (+0x0c) is not changed.** Position += u·d.
5. Wrap into the weather cube (0x1a5b10, desc +0x464), then grow the bounds.
6. Age += d. When age > lifetime, age := fmod(age, lifetime) (CRT 0x782638).

There is no gravity, no acceleration and no ½ factor in this path.

**Shipped data:** the 5 weather emitters are
- `p_rain_med00`: `3D` 1, `31` 1;
- `P_SnowSleet`: `3D` 1, `31` 1;
- `p_rain_of_ash`: `3D` 1, `31` 0;
- `P_Snowblizzard` ×2: `3D` 1, `31` 0.

All have **`43` = 0** (`28` = 1.0), so the inherit term never applies in vanilla. `p_dust_storm` (Sandstorm) is
not a weather emitter (`3D` 0) and uses the normal integrator.

The normal integrators (0x1676a0, 0x166350) add the same f28·E term to the step velocity as well [M, not
transcribed further].

## 7. Probes (P2 and P3 run 2026-11-08 in cu8g t10; P1 run 2026-11-08 in cu8h t01)

- **P1, draw order:** in a debugger, break on 0x1ffb40 / 0x1ffbb0 / 0x532480 / 0x5324d0 when rcx = 0x140a13e24, and
  log the return address and the LCG state during a battle load on `_MP_LAND_ENDOR` (n = 4). Do it once as a
  skirmish and once as a galactic battle.
  - Expected: return addresses inside 0x381a90 / 0x37c050 / 0x3a4820 / 0x378d40 (chunk `0x102`), then exactly
    one hit returning into 0x277030;
  - then the caller's post-load sites (0x2a4fc0 creations or 0x2a1eb0);
  - then 2 hits from 0x468980 in the first tick, when the picked record is a `_Rain` one.
  - **No hit from 0x277030 before chunk `0x102`.**
  The same works with `_RandomHistory.txt` (set bit 0 of 0x140a13e10).
- **P2, save game:** skirmish on `_MP_LAND_ENDOR` (start index 0 = `Sunrise_Clear`). Restart until the battle
  opens in a rain environment (rain visible), save, reload.
  - Expected [M]: Sunrise lighting, no rain after the reload.
- **P3, Kuat:** many `_MP_SPACE_KUAT` loads always show `Environment 1` (never the Cinematic lighting).
- **P4, typed pick:** not reachable in retail; nothing to probe.

**Results (cu8g t10, 2026-11-08, Claude drove the retail game with the cu8g abil kit installed; no saves kept):**
- **P2: as expected [H for what is visible].**
  - Setup: skirmish "(6) Endor Xpress" (taken to be `_MP_LAND_ENDOR`, the only Endor land MP map [M]), human Empire
    against an Easy AI Rebel.
  - Load 1 opened clear and bright with short shadows. Load 2 opened in rain: rain streaks, darker light, the rain
    weather icon at top right.
  - Saved there (a temporary save `cu8g_p2_endor`, deleted afterwards) and loaded it at once. The reloaded battle had
    **no rain** and no weather icon, with low-sun light and long shadows, which is `Sunrise_Clear` (start index 0). That
    stayed so for 12 s after the load.
  - So a save does not keep the picked environment: the reload shows the map's start environment (§2) [H for P2, M
    for the cause].
- **P3: consistent with §1 [M].** Four `_MP_SPACE_KUAT` skirmish loads (07:34, 07:42, 07:54, 07:55) showed the same
  lighting every time: the same sun direction on the planet, the same nebula colour and the same station shading. If
  the pick could land on `Cinematic`, about half the loads would look different. A screenshot cannot name the record,
  so this only shows that the lighting never changes between loads.
- **P1: not run in cu8g t10** (no debugger then); run in cu8h t01, results below.

**P1 results (cu8h t01, 2026-11-08, Claude drove the retail game; kit `tools/probe_mod/cu8h_probes/`):**
- **Tool.** `cu8h_dbg.py` attaches as a debugger (Windows debug API, Python ctypes, nothing installed) and sets a
  hardware **write watchpoint on the LCG state** (0x140a13e24 + the ASLR slide) on every thread, instead of code
  breakpoints on the four draw functions. One write = one LCG step. Each hit logs the return address, a 24-frame stack,
  the state before / after and the mode / frame. `cu8h_dbg_parse.py` assigns each hit to a load phase by the functions on
  its stack (the exe's `.pdata`, chained entries resolved) and checks A (the pick comes after every chunk-`0x102` draw),
  B (the pick is one `int(0, n − 1)` and its replay gives the index), C (no post-load draw before the pick) and D (the
  first-tick weather draws). The game ran unmodified apart from the probe campaign below. Every run detached cleanly.
- **Loads traced** (from Begin to mode frame 120):
  - three skirmish loads of `_MP_LAND_ENDOR` ("(6) Endor Xpress", human Empire against an Easy AI Rebel);
  - one galactic land battle on the same map: probe campaign "CU8H PROBE: ENDOR GALACTIC", with Jabiim's
    `Land_Tactical_Map` patched to `_MP_LAND_ENDOR.ted` and Rebel infantry invading;
  - one galactic space battle at Kashyyyk (`_Space_Planet_Kashyyyk_01.ted`, Heart of the Maw campaign).

| Load | Seed | Chunk `0x102` draws | Pick | Then, load thread | Post-load (main thread) | First tick |
|---|---|---|---|---|---|---|
| Skirmish 1 | Start_Game 0x28e890 ← Load_Map 0x56480 ← LoadingThread | 2857 | 1 int → 1 `Noon_Clear` | 1 int (formation, below) | skirmish 0xeed80 → 0x295cf0 object creations (≈135–148 draws via 0x29f810), then starting forces 0x2a4fc0 (414) | no weather draw |
| Skirmish 2 | same | 2864 | 1 int → 0 `Sunrise_Clear` | 1 int (formation) | same (414) | no weather draw |
| Skirmish 3 | same | 2864 | 1 int → 3 `Noon_Rain` | 1 int (formation) | same (416) | **2 floats** from 0x468980 ← 0x468a50 ← 0x46a0c0 ← land tick 0x3bb440 at mode frame 0, before the common tick 0x3639d0; a 3rd from 0x46a0c0 at mode frame 111 |
| Galactic land (Jabiim) | **none** (galactic stream) | 2873 | 1 int → 0 `Sunrise_Clear` | 1 int (formation) | **none** before the first tick (0x2a1eb0 drew nothing) | no weather draw; 1 float from 0x3a6b80 (← 0x2be76b ← common tick) |
| Galactic space (Kashyyyk) | none | 42 | **none** (the map has one environment record, n = 1) | — | 101 from 0x2a1eb0 (fleet / station creation, with formation draws) | common tick |

  - Load paths: skirmish `Load_Map` ← 0x92940 (LoadingThread); galactic `Load_Map` ← 0x290920 ← 0x929e8.
  - Chunk-`0x102` sites, as §3 step 6 says: 0x4c3000 / 0x1341e0 / 0x62dd40 / 0x550ed0 and the behaviour-attach
    stagger 0x3a4820, under 0x3a0930 ← 0x2b0a20 ← 0x35f970 ← LandMode vf13 0x3b8950 ← 0x3b8cd0.
  - The pick was always the hit right after the last chunk-`0x102` draw. It returned into 0x277030, called from
    0x3bbcd0 inside 0x3b8cd0.
  - The picked indices match what the screen showed: skirmish 1 bright noon, skirmish 2 long sunrise shadows,
    skirmish 3 rain.
  - No skydome draw (vf+0x2a0 / 0x3bc1c0) was seen [H for these loads].
  - Before the seed, the menu's background battle draws on the same stream (skirmish loads only).
- **New: one formation draw at `Load_Map`'s tail [H].** In all four land loads, exactly one more `int` follows the
  pick on the load thread:
  - Stack: `Load_Map` (return 0x56cf5, after the mode load) → 0x1fe7c0 → 0x2b4440 → 0x3952e0 → 0x39bcb0 →
    LandMovementCoordinator vf8 0x43b470 → 0x4f7360 → 0x4f6fb0 → **FormationClass ctor 0x40d0d0**.
  - 0x1fe7c0 flushes the deferred calls. One of them is 0x2b4440, the GOM post-load pass: for each object, 0x3952e0
    finishes it (owner fix-ups, behaviour vf+0x58, …). It ends in 0x39bcb0, which puts a movable object that has no
    movement coordinator into a new one; that creates a formation.
  - The FormationClass ctor draws **`int(1, FPS)`** (synced): +0x1d8 := FPS + r + now, +0x1dc := now − 500.
  - Exactly one object qualified in each load [M which one].
  - So §3 step 8 is not draw-free on land: there is one formation-ctor draw per such object. The space post-load
    0x2a1eb0 also contains formation-ctor draws.
  - MOVEMENT §1.6's "synced random offset" in a formation's start frame is this ctor draw. Its range is `int(1, FPS)`
    [H, decompile and trace]; the port's movement takes `Get_Int(0, Max_Move_Frame_Delay)` [inferred] (follow-up).
- **Verdict:** the §3 order holds in every traced load:
  - [seed, skirmish only] → chunk `0x102` → exactly one pick when n ≠ 1 → post-load → first tick;
  - first-tick weather restart = 2 floats exactly when the picked record has a weather scenario (`Noon_Rain`);
  - no pick before chunk `0x102`, and none when n = 1;
  - additions: the Load_Map-tail formation draw, and the skirmish 0x295cf0 creations ahead of the starting forces.

## 8. Port facts

1. Draw order per tactical load: [new session: seed] → (SP story plot load, engine-side no draw) → chunk `0x102` in
   file order (per record: construction draws, then its spawn-table draws; then the queued spawns) → **environment
   pick: one synced `int(0, n − 1)` when n ≠ 1, else none** → post-load (vf+0x2a0 skydomes of the picked
   environment, caller steps: skirmish start forces / campaign 0x2a1eb0…) → first land tick: weather restart =
   **2** synced float draws when the picked record has a weather scenario with phases.
2. Skip rule: draw over all n records; if the result is named exactly `Cinematic`, take the next one (wrap to 0);
   no re-check. n = 1 never draws. Applies to land and space alike. The typed pick has no skip.
3. The mode-creation pick (0x3b81b0) never draws (n = 1). Do not add a draw there.
4. Store the index on the scene. Do not save it. On save load use the map's `0x100/8` mini `25` (retail behaviour
   [M]).
5. `Set_New_Environment`: clamp, no draw; re-apply lighting / fog / bloom / shadow and, on land, clouds and wind.
   Leave skydomes. Let the weather service restart on its next land tick (2 synced draws when a scenario
   applies).
6. Weather particles: u = v + (`43` ? `28`·E : 0) + (`31` ? 5·W : 0); position += u·d; v unchanged. Presentation
   only.
7. (cu8h t01, §7 P1 results) Each FormationClass creation draws one synced `int(1, FPS)`, including the
   creations at `Load_Map`'s tail after the pick: the GOM post-load pass puts each movable object that has no
   coordinator into a new one, which is one draw per formation; one draw on `_MP_LAND_ENDOR`.
   - The port's land load draws nothing between the pick and the post-load steps.
   - Its formation stagger uses `Get_Int(0, Max_Move_Frame_Delay)`.
   - The checked order elsewhere matches §3. Parity is checked by behaviour, so this is a port follow-up, not a
     failure.
   - Ported in cu8i t02 (2026-11-08, land): the move split's formations draw `int(1, FPS)`, and the Load_Map tail draws
     once per LOCOMOTOR object without a coordinator; the one object on `_MP_LAND_ENDOR` is its Is_Neutral
     `Land_Speeder` (port count 1). Space formation draws are still open (docs/PHASE4.md cu8i).

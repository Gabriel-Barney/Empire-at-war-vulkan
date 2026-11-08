# CU8F t03 / t05: campaign random map picker and scene wind (RE, 2026-11-07)

Source: the decompiled retail exe (Ghidra output, `re_reference/`), the disassembly of `StarWarsG.exe` (FoC, Steam),
the 212 `.ted` files of FoC `maps.meg`, and the FoC XML (`extracted/foc_data_text.zip`). Behaviour only, no code.
Confidence: [H] read in the code (and, where noted, checked against the shipped data), [M] inferred from several
consistent reads, [L] hypothesis.

**Summary.**
- **t03:** the candidates are every `.ted` file found at boot, described by the map-info chunk `0` of the file itself
  (not by planet XML or a list file). The campaign query keeps a map when its type, terrain, faction, max base level,
  player count and custom flag match. It then takes one uniform `int(0, n − 1)` from the **free** RNG. A miss returns an
  empty name, which also clears the name the planet had stored. With the shipped FoC data the query **never matches when
  the planet has a defender**: every multiplayer map has faction 3, and the defender index is never 3. Planet maps have
  player count 1, so they never qualify either.
- **t05:** scene wind = `wind_speed · (cos w, sin w, 0)`, with w = `wind_direction_deg · 0.017453292` (float). It
  is in world units per second, with 0° = +X and 90° = +Y. Only **land** mode writes it: at map load (after the
  random environment pick), on `Set_New_Environment`, and on the "random environment of a weather type" call.
  Space scenes keep (0, 0, 0). Weather phases and `Set_Cinematic_Environment` do not change it. Every particle
  system copies the wind of its scene on each update. A wind-affected particle gets `velocity += wind · 0.5 · dt`.
  Weather particles move with `velocity + 5 · wind`.

---

## 1. t03: the random map picker

### 1.1 Building the table (boot, 0x47cb40, table object 0x140b311a8) [H]

Called once from the boot init 0x4b250. It enumerates three sources with the VFS finder 0x217e80 / 0x218e40:

| list | pattern | kept |
|---|---|---|
| A | `.\Data\Art\Maps\*.TED` | every file |
| B | `Data\CustomMaps\*.TED` (pointer 0x140a2a168), then for each mod path (list 0x140a14cc8, count 0x140a14cd0; the `MODPATH=` mounts [M]) `<mod>\Data\Art\Maps\*.TED` | only when the entry's custom flag (mini `0B`, entry +0xb4) is exactly 1 |

- Within a directory the finder returns loose files first, then archive entries (SPACE_AND_OPEN.md, finder order).
  0x47c8b0 skips a name that is already in the list (exact, case-sensitive compare of `dir\name`) [H]. The stored
  name is the pattern's directory plus the found file name, e.g. `.\Data\Art\Maps\_MP_LAND_HOTH.TED`. The name is
  upper case when it comes from a MEG [M].
- The table is a flat array of 0xf0-byte entries. Order: all of list A in finder order, then the kept entries of B.
  An earlier table is freed first. The table is never rebuilt during a game [M]: no other caller of 0x47cb40 was found.

### 1.2 Per-map entry (0x47b030: opens the file, logs `   Parsing found map '%s'`) [H]

The parser reads the file's top-level chunks only until it has chunk `0` (and chunk `3` when mini `12` asks for it).
It does not load the world chunk `1`. Chunk `0` is a mini-chunk list (TERRAIN_WATER_POST §1.1).

| entry offset | source | default when absent | meaning (shipped values) |
|---|---|---|---|
| +0x00 | the path string from §1.1 (narrow) | — | the picker's result and the "preferred" key |
| +0x20 | mini `08` (UTF-16 text key), or the path widened when `08` is empty | path | display-name source |
| +0x40 | `"(%d) %s"` % (player count, localized `08`; the key itself if the lookup gives `[MISSING]`) | — | lobby label; only when `08` is non-empty |
| +0x60 | mini `09` (UTF-16) | empty | planet name |
| +0x80 | mini `0A` (UTF-16), **upper-cased** | empty | mode tag: `CONTROL` on most MP land maps |
| +0xa0 | mini `01` u32 | −1 | **map type**: 1 land, 2 space (all 212 files) |
| +0xa4 | mini `06` u32 | 0 | **terrain**: TEMPERATE 0, ARCTIC 1, DESERT 2, FOREST 3, SWAMP 4, VOLCANIC 5, URBAN 6, SPACE 7 (Hoth/Ilum/Polus 1, Geonosis 2, Endor/Kashyyyk 3, Dagobah/Jabiim 4, Kessel/Mustafar 5, Coruscant/Bespin 6, every space map 7) |
| +0xa8 | mini `05` u32 | 0 | **faction**: 3 on every MP map and every `_LAND/_SPACE_PLANET_*` map; 0/1/2/5 only on 11 story/demo maps and `_SPACE_PLANET_NABOO_D` (200 of 212 files have 3) |
| +0xac | mini `02` u32 | 1 | **player count**: 2..9 on `_MP_*` maps; 1 on all planet, story and demo maps |
| +0xb0 | mini `03` u32 | 1 | **max base level** (compared ≥ the planet's base level; 5 on almost all maps, 0 on a few) [M for the name] |
| +0xb4 | mini `0B` u8 | 0 | **custom-map flag**: 0 in every shipped file; list B keeps only 1 |
| +0xb5 | mini `12` u8 | 0 | **has start positions**: 1 on the `_MP_*` maps that carry chunk `3` |
| +0xbc / +0xc0 | minis `10` / `11` f32 | 0 | world width / height |
| +0xc8 | chunk `3`: u32 count, count × (f32, f32) | empty | start positions (DynamicVector<Vector2>) |
| +0xe0 / +0xe8 | chunk `0x13`: raw blob, pointer + size | none | not used by the picker (preview data [L]) |

Minis `00` (format 513), `04` (u32, default 1) and `07` (u32) are read but not stored.

### 1.3 The query (0x47bf40) [H]

Arguments: (table, game type, terrain, faction, base level, min players, preferred name, out list, custom flag,
use exclusion list, need start positions).

- **Game type → map type** (0x28ab20): 1 → 0, 2 → 1, 3 → 2, 4 → 1, else −1. **Mode tag** (0x28a9c0, table
  0x140a16df8): empty for types 1–3, `CONTROL` for type 4. A non-empty tag must occur in entry +0x80 (substring
  search, wide).
- **Campaign call** (0x28fda0, BATTLE_TRANSITIONS §2.1 step 1.2): game type 2 + (kind = space) gives map type 1
  (land) or 2 (space). Terrain is the planet type's `Terrain` string (`SPACE` → 7; an unknown string fails the
  battle start). Faction is the defender's faction name: `Rebel` 0, `Empire` 1, `Pirates` 2, `Underworld` 4,
  `Hutts` 5, other 0. It is **−1 only when the defender id is −1**. Base level = planet data +0x130 (ground) or
  +0x148 (starbase). Min players = 2. Preferred = planet data +0x1e0 (the name stored by an earlier pick).
  Custom flag = 0, exclusion = 0, start positions = 0.
- **Filter**, per entry in table order. All of the following must hold:
  1. +0xa0 == map type;
  2. terrain: the query's bit 31 inverts the test. Bit 31 clear: +0xa4 == terrain. Bit 31 set: +0xa4 ≠
     (terrain & 0x7fffffff). So `0xffffffff` means any terrain. **This corrects TACTICAL_GAPS §4.10, which reads
     bit 31 as "any".** The campaign always passes a plain index;
  3. query faction == −1, or +0xa8 == faction. A candidate value is never a wildcard;
  4. base level ≤ +0xb0;
  5. min players ≤ +0xac;
  6. +0xb4 == the custom-flag argument (campaign: 0, so CustomMaps and mod-path maps are never campaign picks);
  7. if "need start positions": +0xb5 ≠ 0;
  8. if "use exclusion list": the upper-cased +0x00 contains none of the strings in 0x140b271c8 (count
     0x140b271d0; the `Quickmatch_Map_Exclusion_List`, game type 4 only);
  9. if the mode tag is non-empty: +0x80 contains it.
  Each match appends its index to the survivor list (and the entry to the out list when one is given). **If the
  preferred name is non-empty and equals +0x00 (same length, byte compare), the scan stops after adding it.**
- **Choice**: n survivors → index `int(0, n − 1)` = 0x1ffb40 on the **free** LCG 0x140a13e20 (QPC-seeded at boot,
  MAIN_LOOP §4.1). The draw is skipped when n = 1. The draw: s = s·0x41C64E6D + 0xBDF, r = (s >> 10) & 0x7fff
  masked to the next power of two above the range, redrawn while r > range. Uniform, unweighted.
- **No survivor** → the static empty entry 0x140a2a170 (empty name).

### 1.4 What the caller does with the answer [H]

The answer is copied into the battle's map name **and into planet data +0x1e0** every time, even when it is empty. So
a miss clears an earlier stored pick. The planet does not simply keep its map. On the next battle the stored name
only stops the scan. The draw is still uniform over the survivors that come before it in table order, the stored
map included. After that, BATTLE_TRANSITIONS §2.1 steps 1.3–1.5 apply: story override, conflict record map, then
`Data\Art\Maps\_Desert_L5_01.ted` / `_Space_Temperate1.ted`. **Neither fallback file exists** in the FoC or EaW
`maps.meg` or on disk [H for the absence; what the loader then does was not traced].

### 1.5 Consequences for the shipped data [H from the data, M for the gameplay reading]

- The picker runs only when the planet type's `Land_Tactical_Map` / `Space_Tactical_Map` is empty. In FoC
  `Planets.xml` that is the land map of Alderaan, The_Maw and VergessoAsteroids (all three
  `Planet_Surface_Accessible` No or space-only), plus `Galaxy_Core_Art_Model`. So vanilla never reaches it in
  normal play.
- Even when it runs, players ≥ 2 leaves only 46 `_MP_*` maps (22 land, 24 space; `_MP_LAND_DATHOMIR` and
  `_MP_LAND_UTAPAU` have player count 1), and all of them have faction 3. Correction (cu8g probe kit, 2026-11-08,
  unverified in game): the copies of `_MP_LAND_DATHOMIR` / `_MP_LAND_UTAPAU` in corruption `Patch2.meg`, which win
  over the base archives, have player counts 4 and 6, so they are candidates too (still faction 3). With a defender
  (faction 0/1/2/4/5) nothing matches, so the result is empty. A space battle at a planet whose `Terrain` is not
  `Space` also finds nothing: every space map has terrain 7.
- Mods that ship planets without tactical maps fall into the same filter. A map is a campaign candidate only if
  it has players ≥ 2, the planet's terrain index, the defender's faction index, max base level ≥ the planet's base
  level, and `0B` = 0.

### 1.6 Port notes (against `campaign_battles.hpp` `MapCandidate` / `pick_random_map`)

- Build `map_candidates` from chunk `0` of each `.ted`, in the order of §1.1. `space` = (`01` == 2),
  `terrain` = `06`, `faction` = `05`, `players` = `02`, `max_base_level` = `03`, `excluded` = (`0B` ≠ 0). Use the
  retail defaults when a mini is absent: players 1, max base level 1, type −1 (matches nothing). The current struct
  defaults (2 and 5) differ.
- In the current `pick_random_map` comment, "candidate faction −1 = any" and "candidate terrain −1 = any" are not
  retail. The wildcards are on the query side: faction −1, and terrain with bit 31 set.
- Add the "stop after the preferred name" rule and the store-back (also when empty) for parity of the planet's
  stored string. The draw itself is free-RNG in retail, so any deterministic choice among the survivors is
  behaviour-equivalent.

### 1.7 Probe (only if a mod-side check is wanted) [L]

What the binary does not settle is what the loader does with a missing fallback map, and whether the defender id
can really be −1 in a campaign battle. Probe: a mod copy of a planet with `Land_Tactical_Map` emptied and
`Terrain` = Temperate, owned by the Empire, then invade it. Observe the loaded map (log line
`Loading map %s for level %d base`) or the failure. Then repeat with an MP map whose `05` is patched to 1, which
should then be picked.

---

## 2. t05: scene wind for particles

### 2.1 Source and conversion (land 0x3bbed0) [H]

Environment record (0x198 bytes, RtsScene +0x90, current index +0xa0): `2B` → +0x168 direction in degrees, `2C` →
+0x16c speed. Ctor defaults (0x271d20): direction 0, speed 2.0.

0x3bbed0 runs only when the land mode has a terrain (+0x338 ≠ 0):
1. terrain clouds: velocity = cloud speed (+0x174) · (cos w, sin w), scale (+0x170), texture (+0x178) → terrain
   (TERRAIN_WATER_POST §5.3 step 4);
2. **scene wind**: w = direction · 0.017453292 (float multiply). Then (x, y) = (speed, 0) rotated by w:
   x = speed·cos w − 0·sin w, y = speed·sin w + 0·cos w. z = 0. The result goes to the alScene (RtsScene vf 0x78)
   +0x1ac / +0x1b0 / +0x1b4. `cos`/`sin` are CRT `cosf` 0x776150 / `sinf` 0x776650. The usual tiny-angle shortcut
   applies: when the exponent bits of w are ≤ 0x1d000000, cos = 1 and sin = w (FIGHTER_FLYING §0).

So the vector points **where the wind blows to**. 0° is +X and 90° is +Y, counter-clockwise about +Z (up) in world
XY. Units are those of `2C`, taken as world units per second (§2.3 uses it as a velocity). There is no other scale.

Shipped values: 289 of 341 FoC environments keep (0°, 2.0). Others are e.g. Tatooine sunrise (45°, 10), Kamino
(32°, 10), Ryloth (33°, 10) and Hoth/Polus snow (0°, 7.0–8.0). Two environments lack `2C`.

### 2.2 When it is written [H]

The alScene ctor 0x199480 sets the wind to (0, 0, 0). After that, the only writer is 0x3bbed0. It is reached only from
the three land-mode vtable slots (LandModeClass 0x8673a0):

| slot | land wrapper | environment change first | callers |
|---|---|---|---|
| vf25 (+0xc8) | 0x3bb840 | 0x276ff0: index := clamp(arg, 0, n − 1) | Lua `Set_New_Environment(int)` (0x5448a0); map load with "keep index" (see below) |
| vf26 (+0xd0) | 0x3bbcd0 | 0x277030: index := synced `int(0, n − 1)` (0x140a13e24; no draw when n = 1); if that record is named `Cinematic` take the next one, wrapping to 0 | normal map load; land-mode creation 0x3b81b0 |
| vf27 (+0xd8) | 0x3bbb30 | 0x2770e0(type): for type ≤ 4, a synced uniform pick among the environments whose `13` value (+0x100) equals type; none, or type > 4 → as vf26 | no caller found through the current-mode pointer [M] |

- Map load (land 0x3b8cd0, space 0x4d80b0): when 0x140b15490 is clear (always, in the shipped game) and the
  loader's "keep" argument is not 1 (normal `Load_Map` 0x56480 passes 0), the mode calls **vf26, a random
  environment**. Otherwise it calls vf25 with the stored index (`0x100/8` mini `25`). The base-map reload 0x360970
  passes 1.
- Environment mini `13` is the **weather type**: CLEAR 0, RAIN 1, SNOW 2, SANDSTORM 3, ASH 4. This matches the
  environment names in every shipped map (`Noon_Rain` 1, `Sunrise_Snow` 2, `Morning_Ash` 4…). It fills the gap
  "`13` reader not traced" in TERRAIN_WATER_POST §5.2.
- **Space** mode's vf25/26/27 (0x4d9750 / 0x4d9790 / 0x4d9760) change the environment but never write the wind.
  Space particles therefore see (0, 0, 0).
- **Not** changing the wind: the per-frame environment re-apply (RtsScene vf10 0x2754f0 → 0x277830); weather
  phases and transitions (L1_S4_REMAINING, weather service: only lightning, fog-plane alpha and emitter intensity);
  `Set_Cinematic_Environment` (0x2769a0, which switches the lighting to the `Cinematic` record and back without
  calling 0x3bbed0). No Lua or XML wind setter exists (strings: only `WIND_BEND_VECTOR`, `WIND_GRASS_PARAMS`,
  `Wind_Disturbance_*`).

### 2.3 Reader: the particle system [H]

- On each update, `alParticleGroup::vf7` 0x171120 copies its scene's wind (scene found by 0x13fe60 walking the
  parent chain) into system +0x21c / +0x220 / +0x224. Without a scene it copies (0, 0, 0).
- Normal integrator (0x1676a0): first velocity += acceleration · dt (gravity, object-space acceleration, outward
  acceleration; ANIM_PARTICLES B.4). Then, when the emitter has `affected_by_wind` (emitter +0x1104, SPEC field
  31): **velocity += wind · dt · 0.5**. The wind is in world space and is not transformed. The add is cumulative,
  so it acts as an acceleration of half the wind vector. The order is: x = dt·wind.x·0.5 + v.x; y and z =
  wind·dt·0.5 + v.
- Weather particles (0x163a90, and the same loop in 0x1670f0): the step velocity is v + the emitter's +0x10dc /
  +0x10e0 term (system velocity × factor, inherit velocity [M]) + **5 · wind** (when +0x1104 is set). The particle's stored velocity is not changed. Position += that · dt.
- Shaders: `WIND_BEND_VECTOR` = wind · 0.5 · (1 + sin(phase)) (0x1997f0) and `WIND_GRASS_PARAMS` = the normalized
  wind (0x1ada40) read the same alScene +0x1ac (RENDER.md §3).

### 2.4 Port notes

- `Environment::wind_vector()` (ted_read.cpp) already matches §2.1. Wire `ParticleRenderer::set_wind` from it:
  - **land**: after the map-load environment choice, after `Set_New_Environment`, and after any typed pick;
  - **space**: always (0, 0, 0);
  - **cinematic switch**: leave the wind unchanged.
  The value is presentation only. It is never read by the sim, so the CRT `cosf`/`sinf` difference has no
  determinism impact.
- The port should use the same map-load rule: vf26 is a synced random pick, so it is part of the synced RNG
  stream. That matters for parity of later synced draws in land battles. It is outside this task, but the draw
  is real: one synced `int(0, n − 1)` per land/space map load when the map has more than one environment.

### 2.5 Probe (optional) [L]

The convention is settled by the code. A visual check would need a mod copy of a land map with one environment
(`2B` = 90, `2C` = 100) and a wind-affected smoke emitter. Smoke should drift toward world +Y. Repeat with `2B` = 0:
the drift should be toward +X, and weather particles should run at 5× that speed.

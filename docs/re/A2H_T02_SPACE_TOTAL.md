# A2h t02: player-level `SpaceTotalUnnormalized` in Heart of the Maw (port vs retail, 2026-11-06)

Question: why the port evaluates `Needs_More_Space_Force_Globally` = `Variable_Human.SpaceTotalUnnormalized >
Variable_Self.SpaceTotalUnnormalized` as 1 for the Empire from frame 1 of `Sandbox_Heart_Of_The_Maw_Rebel` (human
Rebel), while retail evaluates it as 0 (`A2H_T02_FLUSH_PROBE.md`, run `20261106-040725`, MJ records).

Sources: `AI_GALACTIC_TOKENS.md` §3 (0x66b1c0), `AI_GALACTIC_2.md` §4.1, `A2F_RE_READS.md` §2, `A2GAL_READINGS.md` §2.1,
`OBJECT_TYPES.md` (type mask words, §6.2 squadron / company power), `GALACTIC_RNG_DRAWS.md` row 2 / 3,
`GALAXY_MAP_PRESENTATION.md` §7, game XML (FoC). Clean room: no `re_reference/`.

## 1. Answer

1. **The port adds the starbases' own `AI_Combat_Power` to the player-level total; retail does not** [H].
   In galactic mode the starbase is not a store (GOM) object, only a type pointer on the planet (PD +0x140,
   `A2GAL_READINGS.md` §2.1 [H]; the Q2 retail traces carry no starbase objects either, `GALACTIC_RNG_DRAWS.md` row 2).
   0x66b1c0 walks the player's GOM objects, so it never meets a starbase. The port's walk (`player_objects`) adds
   `ps.starbase`, and `player_total` adds `power(starbase)` for `UnitKind::StarBase` whenever `cat & Capital` is non-zero
   (always, with no `Parameter_Category`). Rebel 4 starbases = 40000, Empire 4 starbases = 35000. This is the whole
   2.6x factor.
2. **The port run that showed SST = FRT had no story session** [H]. `--economy --galactic-ai` starts the story plots only
   with `--story` (`engine/apps/headless/ai_run.cpp:234`). With `--story` the port spawns the Accuser (Empire) and
   Home One, Sundered Heart and Rogue Squadron (Rebel) in tick 1; they are in the walk from frame 2.
3. **With both fixed the port matches retail exactly**: Rebel HST = 21135 (retail 21135), Empire SST = 21600 before its
   first space builds and FRT = SST − 4000. NMS = 0 from frame 1.

## 2. Per-object breakdown at the start (port trace, frame 2)

Trace: env-gated `fprintf` per walked object inside `player_total` (scratch export of HEAD d0087c4 + `a2g_trace_patch.py`,
`ALAMO_A2H_T02_TRACE=1`), `alamo_headless --economy --galactic-ai [--story] --campaign Sandbox_Heart_Of_The_Maw_Rebel
--human Rebel --workers 0`. Kinds: 1 SpaceUnit, 2 Squadron, 3 GroundUnit (companies, hero companies), 4 GroundStructure,
5 StarBase. Powers checked against the XML (squadron / company power = sum of the members, `OBJECT_TYPES.md` §6.2).

### 2.1 Empire (Self, p1, tech 3)

| object | kind | count | power each | port adds | retail-fitting |
|---|---|---|---|---|---|
| Empire_Star_Base_3 (Kuat) | 5 | 1 | 15000 | 15000 | 0 (not a GOM object) |
| Empire_Star_Base_2 (Corellia) | 5 | 1 | 10000 | 10000 | 0 |
| Empire_Star_Base_1 (Kashyyyk, Tatooine) | 5 | 2 | 5000 | 10000 | 0 |
| Victory_Destroyer | 1 | 2 | 3800 | 7600 | 7600 |
| Acclamator_Assault_Ship | 1 | 2 | 2500 | 5000 | 5000 |
| Tartan_Patrol_Cruiser | 1 | 4 | 1250 | 5000 | 5000 |
| Accuser_Star_Destroyer (story Spawn_Piett; Capital, SpaceHero) | 1 | 1 | 4000 | 4000 with `--story`, else absent | 4000 |
| Boba_Fett_Team, General_Veers_Team, Emperor_Palpatine_Team, Grand_Moff_Tarkin_Team (story) | 3 | 4 | 500 / 500 / 600 / 0 | 0 | 0 (DUMMY_GROUND_COMPANY) |
| ground companies (Anti-Infantry Brigade x3, Stormtrooper Squad x4, Heavy Scout Squad x1) | 3 | 8 | — | 0 | 0 |
| ground structures (E_Ground_Barracks x4, Light Vehicle Factory x2, Research Facility, Mining Facility) | 4 | 8 | — | 0 | 0 |
| per-tech members (exact list, tech 3) | — | — | — | 0 | 0 |
| **total, no story** | | | | **52600** | 17600 |
| **total, `--story`** | | | | **56600** | **21600** |

FRT = SST − SST{SpaceHero}: port 52600 (no story) / 56600 − 4000 = 52600 (story); retail-fitting 17600.

### 2.2 Rebel (Human, p0, tech 3)

| object | kind | count | power each | port adds | retail-fitting |
|---|---|---|---|---|---|
| Rebel_Star_Base_3 (MonCalimari) | 5 | 1 | 15000 | 15000 | 0 |
| Rebel_Star_Base_2 (Kamino, Naboo) | 5 | 2 | 10000 | 20000 | 0 |
| Rebel_Star_Base_1 (Yavin) | 5 | 1 | 5000 | 5000 | 0 |
| Alliance_Assault_Frigate | 1 | 1 | 2500 | 2500 | 2500 |
| Nebulon_B_Frigate | 1 | 2 | 2200 | 4400 | 4400 |
| Marauder_Missile_Cruiser | 1 | 1 | 1400 | 1400 | 1400 |
| Corellian_Gunboat | 1 | 1 | 1350 | 1350 | 1350 |
| Corellian_Corvette | 1 | 3 | 1250 | 3750 | 3750 |
| A_Wing_Squadron (5 x 80) | 2 | 2 | 400 | 800 | 800 |
| Rebel_X-Wing_Squadron (5 x 60) | 2 | 3 | 300 | 900 | 900 |
| B-Wing_Squadron (3 x 75) | 2 | 2 | 225 | 450 | 450 |
| Y-Wing_Squadron (3 x 75) | 2 | 1 | 225 | 225 | 225 |
| Home_One (story Spawn_Ackbar, tech 3) | 1 | 1 | 4000 | 4000 with `--story` | 4000 |
| Sundered_Heart (story Spawn_Antilles, tech 1) | 1 | 1 | 1000 | 1000 with `--story` | 1000 |
| Rogue_Squadron_Space (story, tech 2) | 2 | 1 | 360 | 360 with `--story` | 360 |
| Han_Solo_Team, Obi_Wan_Team, Yoda_Team, Droids_Team, Mon_Mothma_Team (story) | 3 | 5 | — | 0 | 0 (DUMMY_GROUND_COMPANY) |
| ground companies (14) and structures (9) | 3 / 4 | — | — | 0 | 0 |
| per-tech members (exact list, tech 3) | — | — | — | 0 | 0 |
| **total, no story** | | | | **55775** | 15775 |
| **total, `--story`** | | | | **61135** | **21135** |

Underworld (p5, for reference, no story): 54850 = starbases 30000 + Kedalbe 4250 + Vengeance x2 4400 + Interceptor4 x3
4200 + Crusader x4 5000 + StarViper x5 2000.

Category cross-check (exp build, `ALAMO_A2H_EXTRA_EQS`, frame 1, no story): Empire {Capital} 35000, {Frigate} 12600,
{Corvette} 5000, {Fighter} 0, {Bomber} 0, {SpaceHero} 0; Rebel {Capital} 40000, {Frigate} 6900, {Corvette} 6500,
{Fighter} 2150, {Bomber} 675, {SpaceHero} 0 (with `--story`: Empire {SpaceHero} 4000, Rebel {SpaceHero} 5000, Rebel
{Fighter} 2510). Every number is the table above.

## 3. The composition that gives retail's numbers

Rule (0x66b1c0 as read, with the starbase not being a GOM object):
- only objects whose resolved type (0x3973b0) is DUMMY_STARSHIP or DUMMY_SPACE_FIGHTER_SQUADRON add their own power
  (mask +0x1648 against `Parameter_Category`, default all bits) [H];
- a squadron counts once, at the summed member power (300 for an X-wing squadron); member fighters seen as their own
  objects (retail stores them, `GALACTIC_RNG_DRAWS.md` row 2) add nothing, as they are neither DUMMY_STARSHIP nor carry a
  Capital-matching +0x1658 mask [M];
- a company's transport resolves to the company type (`STRATEGIC_GAPS.md` §1.7 via `galactic_ai_tokens.cpp` HasUnit
  note), DUMMY_GROUND_COMPANY: hero companies with a space form (Han_Solo_Team, Boba_Fett_Team) add nothing, and
  neither do the Millennium Falcon / Slave I objects (they would show as +1500 SpaceHero) [M];
- the other branch (+ power when mask +0x1658 matches, default Capital) adds 0 for every vanilla type: +0x1658 is the
  type's `Garrison_Category` (`OBJECT_TYPES.md`, third mask word) and the shipped XML uses only Infantry / Vehicle there.
  Read as the +0x1648 mask instead it would still add 0 by default (no non-starship GOM type has Capital once starbases
  are out), but with `{Parameter_Category = SpaceHero}` it would add Boba_Fett_Team (CategoryMask LandHero | SpaceHero,
  500) and retail's SST − FRT would be 4500, not 4000. So the +0x1658 = Garrison_Category reading fits [M] (assumes
  Boba Fett was alive at frame 1435; he spawns at start, story tech-1 event);
- per-tech members: exact list (0x373fd0 / 0x373ee0, no fall-back). No vanilla type has a
  `Starting_Spawned_Units_Tech_1..5` list (grep of the FoC XML: none), so at tech >= 1 members add 0 for every object
  [H data]. A fall-back list would add the Acclamator / Victory / Accuser TIE complements (3615 + 1080 for the Empire),
  which retail does not show.

Arithmetic:
- **Rebel HST 21135** = units 15775 (frigates 6900 + corvettes 6500 + squadrons 2375) + story space heroes 5360
  (Home_One 4000 + Sundered_Heart 1000 + Rogue_Squadron_Space 360) [H: exact fit]. It stays 21135 from 1435 to 51511:
  the human built no space units and lost none. The Rebel's tech-4 story heroes (Garm Bel Iblis, Luke) are ground
  companies, so the value is the same at tech 3 or 4.
- **Empire SST 24250** = start units 17600 + Accuser 4000 + 2650 built by frame 1435. **FRT 20250** = 24250 − 4000. The
  2650 is most likely Tartan_Patrol_Cruiser 1250 + Broadside_Class_Cruiser 1400 [M]: the port's own first
  `Build_Space_Forces` pick at frame 1 is two units for 3050 credits = Tartan (1250) + Broadside (1800). Other sums
  (Acclamator + 2 TIE_Scout_Squadron = 2500 + 150) are possible but less likely [L].
- **Frame 51511**: SST 26750, FRT 22750: +2500 against 1435 (one more Acclamator, or an equivalent composition change)
  [L]; SpaceHero still 4000 (the Accuser).

Rebel start: the port has Rebel credits 13000 and tech level 3 (campaign `Starting_Credits Rebel, 13000`,
`Starting_Tech_Level Rebel, 3`, `Max_Tech_Level Rebel, 4`). The retail start screen's "tech 4" is the displayed level:
the Rebel faction has `Displayed_Tech_Level_Adjustment +1` (FACTIONS.XML; REMAINING_GAPS.md campaign-start row), so
internal 3 shows as 4. The port start matches [M: display rule from the field name and the data].

## 4. Port change

File `engine/ai/src/galactic_ai_tokens.cpp`, `GalacticSimTokenResolver::Impl` (HEAD lines 1179-1215):

1. `player_objects(int32_t pl, F&& fn)`: drop the starbase line
   `if (ps.starbase && st.alive(ps.starbase) && st.owner(ps.starbase) == pl) fn(ps.starbase);`
   Reason: in galactic mode the starbase is a planet field, not a GOM object (`A2GAL_READINGS.md` §2.1 [H]), so the
   original's per-owner walk (0x2acb70) never sees it. The walk also feeds `GroundTotalUnnormalized` (starbase per-tech
   members, which only matter at tech 0) and `HasUnit` (a starbase type would no longer count; no shipped equation asks
   HasUnit for a starbase type, and retail would not count it either [M]). If the HasUnit side should stay out of
   this change, put `if (kind == gal::UnitKind::StarBase) return;` at the top of the `player_total` lambda instead
   (skips both the power and the members).
2. `player_total(int32_t pl, bool space, uint64_t cat)`: delete the branch
   `else if (kind == gal::UnitKind::StarBase && (cat & capital) != 0) s += power(t);` and the `capital` local.
   The spec's structure branch (power(T) when the type's +0x1658 `Garrison_Category` matches `Parameter_Category`, default
   Capital, and T+0x1E set or T not DUMMY_ORBITAL_STRUCTURE) adds 0 for every vanilla type (section 3), so a comment is
   enough; implement it only if the type info gets a garrison mask [M].
   The comment at the branch should cite this doc and `A2GAL_READINGS.md` §2.1 instead of "[H] structures by +0x1658".

What it should compute (vanilla): Σ over the player's fleet units of power(type) for SpaceUnit / Squadron kinds with
`type_mask & cat`, plus the exact per-tech members of every walked object (0 at tech >= 1 in vanilla). Space heroes
(UniqueUnit with Hyperspace) are SpaceUnit kind and count; hero companies are GroundUnit kind and do not.

Measured with the change (env-gated `ALAMO_A2H_T02_FIX=1` in the scratch build, 1600 ticks):

| run | frame 1 | NMS = 1 samples / all p1 samples | first `Flush_MajorItem_Budget` plan for p1 |
|---|---|---|---|
| current, no story | NMS 1, FRT 52600 | 12 / 12 | frame 42 |
| current, `--story` | NMS 1, FRT 52600 | 13 / 13 | frame 42 |
| fixed, no story | NMS 0, FRT 17600 (HST 15775) | 13 / 18 | frame 446 |
| fixed, `--story` | NMS 0, FRT 17600 (HST 21135, SST 21600) | 6 / 18 | frame 1046 |

The NMS = 1 samples left after the fix are not this token: the port's Empire loses ships early (with `--story`: an
Acclamator in the Underworld attack on Kashyyyk, auto-resolved at galactic frame 867, and Kashyyyk itself at 876;
FRT 17600 → 15100 at frame 922, so SST 19100 < HST 21135). Retail's Empire kept FRT 20250 to frame 17179 and lost its
first planet at 17551. That is an AI-behaviour difference (Underworld aggression / Empire defence), for another task.
Goldens: the change moves every galactic AI run that evaluates these equations (NMS, `Has_Enough_Space_Force_Globally`,
`Allow_Blind_Space_Production`), so the coordinator regenerates them.

Related, not part of this change: the planet-level leaf 0x6dbd50 (`galactic_ai_planet_force`, HEAD line ~104) applies
the same "`mask & Capital`" reading to ground structures with `special_weapon_space` (+0x1E). By the Garrison_Category
reading of +0x1658 those add 0 in vanilla too [L]; check against a planet-level probe before changing it.

## 5. The Accuser in the port

- The port spawns it only when the story session runs: `alamo_headless --economy --galactic-ai --story ...`
  (`ai_run.cpp:234`, q2f t03). The runs behind the "port, frames 1..205" column (`scratchpad\a2h_nms\run.cmd`, and the
  same command line in `A2H_T01_RERUN.md`) had no `--story`, so no hero existed.
- With `--story`, `Story_Plots_Empire_AI_Heroes_EXP.xml` fires Universal_Story_Start, the tech-1..3 events and
  `Spawn_Piett` (UNIQUE_UNIT Accuser_Star_Destroyer at Coruscant) in tick 1. Coruscant is not on the map, so
  `story_session.cpp` (UNIQUE_UNIT / SPAWN_HERO, line ~978) anchors on the Empire's lowest-index owned planet
  (Corellia, `GALAXY_MAP_PRESENTATION.md` §7.2). The walk at frame 1 (before the story service) has no Accuser; from
  frame 2 on it does (trace). The Rebel heroes (Home One, Sundered Heart, Rogue Squadron, and the ground hero teams)
  appear at the same time.
- So, well before frame 1435: yes, with `--story`. Comparisons with retail sandboxes should pass `--story`.

## 6. Confidence

| claim | tag |
|---|---|
| port per-object breakdown (trace + category cross-check + XML powers) | H |
| starbase own power not in retail's player total (not a GOM object; exact numeric fit for the Rebel) | H |
| Rebel 21135 = 15775 units + 5360 story space heroes | H (exact fit) |
| Empire 2650 = Tartan + Broadside built by frame 1435 | M |
| frame 51511 +2500 composition | L |
| +0x1658 = Garrison_Category reading of the structure branch (adds 0 in vanilla) | M |
| hero-company transports and squadron member fighters add 0 | M |
| per-tech members add 0 at tech >= 1 in vanilla (no Tech_1..5 lists in the data) | H |
| Rebel start tech 3 = retail's displayed 4 (Displayed_Tech_Level_Adjustment +1) | M |
| port spawns the Accuser in tick 1 with `--story`, never without | H |

## 7. Reproduction (scratch, not in the repo)

Scratchpad `a2h_t02\`: `x\` = `git archive HEAD` + `tools/analysis/a2g_trace_patch.py` + `patch_t02.py` (trace
`ALAMO_A2H_T02_TRACE`, fix `ALAMO_A2H_T02_FIX`; `save_run.cpp:148` stubbed for the reduced module list), `build_x.cmd`,
`run_tr_story.cmd` / `run_tr_nostory.cmd` / `run_fix_story.cmd` / `run_fix_nostory.cmd`; category runs `run_story.cmd` /
`run_nostory.cmd` with `eqs.txt` on the existing exp build; XML helpers `calc.py`, `types.py`.

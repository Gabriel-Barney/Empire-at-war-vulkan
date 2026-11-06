# A2k t07: story load / CONSTRUCT answers (RE, 2026-11-06)

Answers to the five open questions of `A2J_T05_STORY_LOAD.md`. StarWarsG.exe FoC x64 (Steam), RVAs. Tags: [H] read from
code, [M] one inferred link, [L] unverified. No decompiled code is reproduced.

## Summary for the port

| # | Question | Answer | Port today (A2j t05) |
|---|---|---|---|
| 1 | L8 walk order | planets in `Locations` order; for each planet, the `Starting_Forces` entries of that planet in file order [H] | entry order: same result only when the file groups entries by planet in `Locations` order |
| 2 | company CONSTRUCT | **one** CONSTRUCT, name = the company type; transport and members raise none. A `+0x2018` chained type gets its own CONSTRUCT through the recursive call [H] | company type: matches |
| 3 | CONSTRUCT_LEVEL value | not a planet level: the event compares Param2 with the **constructed type's `Base_Level`** (type +0x8A0). Raised only for DUMMY_GROUND_BASE / DUMMY_STAR_BASE types; ground structures (0x5F) never raise it [H] | wrong for ground structures (raises it with level k) and uses the planet level |
| 4 | CONSTRUCT for run-time spawns | yes: the dispatch is inside 0x400b40, so every caller raises it (production, story UNIQUE_UNIT / spawn rewards, Lua `Spawn_Unit`, AI HIJACK_UNIT, ...) [H] | load pass and production only |
| 5 | owner filter | the **planet's current owner** (planet data +0x6C, faction compare), never the creating player [H]. At L8 that owner is still neutral (set only after all starting forces), so default FILTER_FRIENDLY_ONLY starting-force CONSTRUCTs do not fire [H order, M initial value] | creating player: differs at L8 and for spawns on a planet the player does not own |

## 1. L8 order (0x292050, 0x3fae10) [H]

1. Pass 1: for each `Locations` entry (campaign +0x128, count +0x130, 16-byte entries) in list order, create the planet
   (0x29f810).
2. Pass 2: for each `Locations` entry again in the same order, look up the planet object of that type (0x2a88f0; skipped when
   absent or object +0x335 == -1) and call 0x3fae10(planet behaviour, planet, campaign).
3. 0x3fae10 walks the campaign's starting-force list (+0x190, count +0x198, stride 0x70) **from the first entry**, and handles
   each entry whose planet type (+0x60) is this planet's type, whose unit type (+0x68) is set, and whose owner name (+0x40)
   resolves to a faction (0x3303d0) with a player (0x294d30) whose +0x180 flag is set. Otherwise the entry is skipped.
4. For a handled entry, in this order: planet data D (obj +0xB8) +0x74 = that player's id, D +0x70 = 1.0f; then
   0x400b40(planet behaviour 6, unit type, planet, player id). D +0x70 / +0x74 is a second owner record, **not** the current
   owner D +0x68 / +0x6C that the story filter reads (§5).
5. Only after pass 2, 0x292050 calls 0x3fa160 for every planet object: owner := 0x3f6af0(...) into D +0x6C, +0x74 and +0x9C
   (weight into +0x68, +0x70, +0x98), sets the object owner (0x38e5a0) and sends signal 0xE (PlanetFactionChange).
   Planets are created in pass 1 with the neutral player (first faction with +0x108 set, 0x331cc0) as owner.
6. Before the starting-force loop, 0x3fae10 walks a second campaign list (+0x1a8, same stride) for the same planet and only
   appends types to planet data +0x108 (no creation, no story event) [H; list meaning L].

Result: global order = (Locations index, Starting_Forces file index). The port's entry order equals it only when the file
lists entries grouped by planet in `Locations` order.

## 2. 0x400b40 (create type T at planet P for player id O) [H]

Dispatch by T's behaviours (0x370320): DUMMY_GROUND_BASE 0x5B, DUMMY_STAR_BASE 0x5C, DUMMY_GROUND_STRUCTURE 0x5F,
DUMMY_ORBITAL_STRUCTURE 0x60, DUMMY_GROUND_COMPANY 0x5D, DUMMY_SPACE_FIGHTER_SQUADRON 0x5E, DUMMY_STARSHIP 0x61.

- Guard: if 0x375220(P's type, P) is false and T has 0x5B or 0x5F, return null at once (no chain).
- 0x5B (ground base): **nothing is created and no story event** (the ground base type is kept in step with the structures
  elsewhere; `GROUNDBASES.XML`: "Ground Bases are not directly built anymore").
- 0x5C (starbase): **first** CONSTRUCT dispatch 0x326cd0, **then** planet data +0x148 = T `Base_Level` (+0x8A0), +0x138 = 0,
  +0x13C = O, +0x140 = T; 0x4b7450 when the level < 2. Returns null (lVar stays 0).
- 0x5F ground structure (sets planet owner +0x74 = O when it is -1; skipped when planet +0x130 > 5), 0x60 orbital structure
  (skipped when planet +0x148 > 5), 0x5D company (0x400f30), 0x5E squadron (0x400650), 0x61 starship (0x401660): create, and
  **if the result is non-null**, CONSTRUCT dispatch 0x326cd0 after the creation.
- None of these behaviours: log "Unknown type", no creation, no event.
- Last, in every case except the guard: if T +0x2018 (the resolved `Create_Additional_Object` type [M]) is set,
  0x400b40(same planet, that type, same owner) recursively, with its own CONSTRUCT by the rules above.

The CONSTRUCT target is the story manager of GMM mode 0 (0x28a950(GMM, 0) +0xA0) [H call, M that slot 0 is galactic], so the
dispatch gate of `A2I_T01_FRAME_ORDER.md` §3.2 applies (immediate when the galactic mode is current, else queued to T9).

### 2.1 Company (0x400f30) [H]

The transport and every `Company_Units` member are created with the raw object create 0x29f810, not 0x400b40, so none of
them raises a story event. The only CONSTRUCT is the one 0x400b40 raises with T = the company type, after 0x400f30 returns
non-null (it returns null when the type has no `Company_Transport_Unit`).

## 3. CONSTRUCT / CONSTRUCT_LEVEL dispatch 0x326cd0(story, planet, type) [H]

1. Log "STORY - CONSTRUCTED %s"; dispatch type 4 (CONSTRUCT), **no player scope** (every player's active plots), name =
   type name (+0xF8), object = planet.
2. If planet is non-null and the type has 0x5B or 0x5C: dispatch type 5 (CONSTRUCT_LEVEL), no player scope, data =
   (planet, type). **No level value is passed.**

Only three call sites exist (both 0x400b40 branches and tactical build completion 0x42e890, which passes planet = null, so no
CONSTRUCT_LEVEL). No other code dispatches type 4 or 5. So in galactic play CONSTRUCT_LEVEL is raised only for starbase types.

### 3.1 CONSTRUCT_LEVEL evaluator 0x4543b0, parser 0x464760 [H]

- Param1: planet type names (upper-cased, checked against the type manager at load: "invalid planet name" logged).
  Param2: `atoi` -> +0x360. Param3: story enum name -> +0x364 via table 0x140a28d70 (**GROUND = 0, SPACE = 1**). Params 4+ are
  ignored by the parser (so `Event_Param4 EMPIRE` in `STORY_TUTORIAL_IV.XML` has no effect).
- Fires (0x455db0) when planet's type name is in Param1, and the category matches (+0x364 == 0: type has DUMMY_GROUND_BASE;
  == 1: DUMMY_STAR_BASE; other: no test), and **Param2 <= type `Base_Level`** (unsigned).
- No owner filter at all: any player's starbase at a listed planet fires it.
- Correction to `STORY.md` §5 row CONSTRUCT_LEVEL: Param3 is 0 = GROUND (ground base), 1 = SPACE (starbase), not the
  reverse. The value of an absent or unknown Param3 (ctor default, 0x45c3f0 miss) is [L].

Because the dispatch for a starbase comes before planet +0x148 is written, a Lua or reward that reads the planet's starbase
level inside that CONSTRUCT chain sees the old level [M: no shipped plot does this].

## 4. Run-time spawns [H]

Every 0x400b40 caller raises CONSTRUCT (and CONSTRUCT_LEVEL for starbases) inside the creation call:

| Caller | What |
|---|---|
| 0x3fae10 | L8 starting forces |
| 0x402840 | production queue completion |
| 0x4626a0 | story UNIQUE_UNIT / spawn rewards: one call (and one CONSTRUCT) per spawned copy (`Reward_Param3` count, default 1) |
| 0x6a2520 | Lua `Galactic_Spawn_Unit` (Lua `Spawn_Unit` in galactic) |
| 0x648370 | AI HIJACK_UNIT (MagicPlan) when it spawns the unit |
| 0x6a0400 | Lua Reinforce [M: which mode reaches 0x400b40] |
| 0x3fc750 | spawn with "Trying another planet" retry [L: who calls it] |
| 0x3f6650, 0x281240 | [L: purpose] |

So T9 reward spawns, Lua spawns and AI magic spawns all need CONSTRUCT at the moment of creation (synchronous, inside the
reward / Lua call / AI service), with the dispatch gate of §2.

## 5. Owner filter of CONSTRUCT (0x4555f0 -> 0x455ce0) [H unless tagged]

- 0x4555f0: name must equal a Param1 entry; with no object, fires at once; else filter, then decrement that name's counter
  (fires at <= 0); a filter miss logs "Event didn't trigger because filter did not match".
- 0x455ce0(event, object, filter): owner record = 0x3956c0(object) (object +0xB8 data +0x68 when the object has that data, as
  planets do; else a static {1.0, object +0x58}); its faction = 0x331b70 (owner id -> player +0x68; owner -1 -> the first
  faction with +0x108 set, the neutral faction). Plot side: the plot owner player's faction (+0x228 -> +0x638 -> +0x68).
  Filters (names from table 0x140a28d70): 0 NONE true; 1 FRIENDLY_ONLY same faction; 2 NEUTRAL_ONLY owner faction neutral;
  3 ENEMY_ONLY not same and not neutral; 4 FRIENDLY_AND_NEUTRAL; 5 ENEMY_AND_NEUTRAL; 6 FRIENDLY_AND_ENEMY (not neutral).
- For CONSTRUCT the object is the planet, so the filter reads the **planet's current owner** (D +0x6C) at dispatch time. The
  creating player is not an argument of 0x326cd0.
- **At L8** the current owner is not set yet (§1 step 5): D +0x6C still holds its creation value, the neutral player or -1,
  and both resolve to the neutral faction [H order; M that 0x29f810 leaves +0x6C at the neutral id or -1]. So every
  starting-force CONSTRUCT is filtered as a construction on a **neutral** planet: the default FILTER_FRIENDLY_ONLY (and
  ENEMY_ONLY, FRIENDLY_AND_ENEMY) fail for every non-neutral plot; NONE, NEUTRAL_ONLY and the *_AND_NEUTRAL filters pass.
- At run time D +0x6C is the planet's real owner, so a spawn on a planet the player does not own is judged by that owner.

## 6. Open questions

1. Planet data +0x6C right after 0x29f810 (neutral id or -1; both give the neutral faction, so the L8 result does not depend
   on it). A probe would confirm the L8 consequence: a test plot with an armed, prereq-free `STORY_CONSTRUCT` on a starting
   unit, once with the default filter (expected: no fire at load) and once with `FILTER_NEUTRAL_ONLY` (expected: fire at load).
2. Absent / unknown CONSTRUCT_LEVEL Param3 value (§3.1): ctor default of +0x364 and the 0x45c3f0 miss value.
3. Callers 0x3fc750, 0x3f6650, 0x281240 and Reinforce's mode (§4): which game features they are.
</content>
</invoke>

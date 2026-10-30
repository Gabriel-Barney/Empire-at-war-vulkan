# Strategic and presentation gaps (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Clean-room behaviour notes from the Ghidra 12.2 decompilation of the Steam `StarWarsG.exe` (all addresses are RVAs),
disassembly of the same exe and of `PerceptionFunctionG.dll`, static initial values read from the exe image, and the
shipped FoC XML plus Thrawn's Revenge 3.3 XML (workshop 1125571106: loose `Data\XML` and `Config.MEG`). No decompiled
code is reproduced. Confidence: [H] read directly, [M] inferred from structure or naming, [L] guess. "Probe" items
need Gabriel to run the game (prepare under `tools/probe_mod/`; nothing was launched).

Written 2026-10-28 for the open items of `docs/PHASE4.md` (A2, A2 gal, S2, S3, L1 stage 3, R2, R4, P1, P2, P3) and the
open lists of AI_GALACTIC_3, GALACTIC_ECONOMY, STORY, ANIM_PARTICLES, RENDER, UI and AUDIO. TACTICAL_GAPS.md (written
in parallel) covers tactical combat; this file does not repeat it.

## 0. Status per question

| # | Question | Status | Unblocks |
|---|---|---|---|
| 1.1 | Survival marshaller 0x588a90 | answered [H] | engine/ai (galactic) |
| 1.2 | Type flag +0x29; the 0x5C / 0x5B test | answered [H] | engine/ai, engine/galactic |
| 1.3 | Buildability 0x2804d0 (prerequisites, Required_Special_Structures OR groups) | answered [H] (two flag meanings [M]) | engine/galactic, engine/ai |
| 1.4 | Galactic victory monitor, Super_Weapon_Destruction | answered [H] for the tests, [M] for some player filters | engine/galactic, engine/apps |
| 1.5 | MagicPlan veto 0x375270 | answered [H] | engine/ai |
| 1.6 | Delayed-spawn list | already specified (P4_SIM_SYSTEMS §6.2); one extra branch [M] | engine/galactic |
| 1.7 | **Galactic company transports** (why AI saboteurs are immobile) | answered [H] | engine/galactic, engine/ai |
| 2.1 | Fleet speed multiplier M | **answered [H]: M = `Object_Max_Speed_Multiplier_Galactic` (FoC 5.0, TR 11.34)** | engine/galactic, engine/ai |
| 2.2 | Galactic stealth / visibility | mechanism [H], detection details [M] | engine/galactic |
| 2.3 | Upkeep | answered [H]: no deduction exists | engine/galactic (none) |
| 3 | PerceptionFunctionG grammar and precedence | answered [H] | engine/ai |
| 4 | STORY §7 items 2, 3, 8; plot activation; LOCK_UNIT | answered [H] | engine/lua (story) |
| 5 | Is_Obsolete writer, LuaScriptWrapper creator, opportunity-fire consumer | answered [H] / [M] / [H] | engine/lua, engine/ai |
| 6.1 | R2 galactic map "Needs RE" | 4 of 7 answered, 3 open (probe/capture) | engine/render/galactic |
| 6.2 | R4 invisible-system throttle, system scalar writers | answered [H] | engine/render/particles |
| 7.1 | P1 scroll acceleration / deceleration, Camera_Stop_* | answered [H] | engine/ui (camera) |
| 7.2 | P2 HUD list selectors, Health_Bar_Scale use sites | answered [H] | engine/ui |
| 7.3 | P3 video alpha loops | answered [H] (HUD placement [M]) | engine/video, engine/ui |
| 7.4 | AUDIO §8 music modes, excluded set, rolloff | modes [H], excluded set [M], rolloff probe-only | engine/audio |
| 8 | SPACE_AND_OPEN §D.7 item 4 (240 s) | fixed (956 s for Coruscant, see the edit there) | — |

---

## 1. A2 / A2 gal (galactic AI and galactic sim)

### 1.1 Survival marshaller (0x727cd0 → 0x588a90) [H]

- The candidate type is the plan's unit type, or the first `Company_Units` member when the type is a company whose
  first member is not `Create_Team` (type +0x958 list, member +0x67).
- Score = the AI player's learning system (player +0x360 → AI object → +0x58) **per-type survival ratio**: a hash
  table keyed by the **type-name CRC** (type +0x11c) with entries {successes +0x14, attempts +0x18}. Score =
  successes / attempts when attempts ≠ 0, else **1.0** (never seen). Lookups only; keying by CRC means no container
  iteration order is involved.
- Writer 0x58a9a0 (attempts += 1; successes += 1 when the "survived" argument is true). Two callers:
  - **object removal** 0x3a8380 (every object leaving a mode): skipped in multiplayer (global 0x140a7bed5), for objects
    without the UNIT_AI behaviour slot (+0x344) or with the TRANSPORT slot (+0x337), and for owners without an AI
    (player +0x360 = 0). Type = the object's "original type" (0x3973b0: team / transport / container type, else the
    object's own type). Survived = health fraction > 0 and not (damage-tracking pointer +0x110 present with +0x18 = 0)
    [M for the second clause]. So a unit that leaves a battle alive counts as a success, a destroyed unit as a failure;
  - **auto-resolve survival roll** 0x60a140 (BATTLE_TRANSITIONS §4.4 step 7): same company-leader substitution;
    the survived flag is the roll's outcome [M: register argument not visible in the decompile].
- Score is added like the other marshaller scores (AI_GALACTIC_2 §1.4), so it is a bounded 0..1 bonus favouring types
  that survive. Port: replace the A2 stand-in "Survival 1.0" with this table, fed by the two callers.

> **Status 2026-10-30:** The stand-in is replaced: `LearningSystem::record_survival` / `survival_ratio` (`engine/ai/src/ai_learning.cpp:70`, `:76`, keyed by type-name CRC) feed `SurvivalM` (`engine/ai/src/galactic_pp.cpp:776-783`), which is registered as the last galactic marshaller (`galactic_pp.cpp:977-978`).

### 1.2 Type flag +0x29 and the 0x5C / 0x5B test [H]

- **+0x29 = "is a squadron member"**: the type derivation 0x377500 sets it on every `Squadron_Units` member (type +0x978
  list), exactly as +0x2A marks `Company_Units` members (OBJECT_TYPES §6.2 items 3/4). The field-table names `Is_3D` /
  `Is_2D` belong to the SFX-event parser, not to GameObjectType. Consequence: MagicPlan (AI_GALACTIC_2 §2.3) and the
  buildability test (§1.3 step 13) refuse squadron and company **members**; only the container types are buildable or
  spawnable. This replaces A2's stand-ins (IsDeathClone / no cost / TRANSPORT).
- **0x5C / 0x5B are behaviour ids**: 0x5B = DUMMY_GROUND_BASE (GROUNDBASES.XML), 0x5C = DUMMY_STAR_BASE
  (STARBASES.XML) (OBJECT_RUNTIME dummy table). The test means "this is a generic base-level type" (a starbase / ground
  base level object), not a unit-kind filter. Related ids: 0x5D DUMMY_GROUND_COMPANY, 0x5E DUMMY_SPACE_FIGHTER_SQUADRON,
  0x5F DUMMY_GROUND_STRUCTURE, 0x60 DUMMY_ORBITAL_STRUCTURE, 0x61 DUMMY_STARSHIP.

### 1.3 Buildability 0x2804d0(player P, type T, planet X, check_credits, …, out reason, out detail) [H]

Returns true only if every step passes, in this order. "Reason" codes are written to the optional out-argument (used
by the GUI tooltip and Lua); steps without a code just fail.

1. T and X exist; X has the PRODUCTION behaviour (slot +0x338); T is not `Cinematic_Object_Only` (+0x21).
2. T's `Affiliation` list (+0xEF0) contains P's faction (0x3751a0).
3. Planet-adjusted cost (0x3711c0: the planet production behaviour's cost rule, clamped to ≥ 1 when positive, else
   `Build_Cost_Credits`) ≥ 1, and planet-adjusted build time (0x371230; default `Build_Time_Seconds` × a global factor)
   ≥ 0.
4. T is not DUMMY_GROUND_BASE (0x5B). (So the later 0x5B branch, step 18, is dead code.)
5. T is "ground-buildable" (cost ≥ 1 and (`Required_Ground_Base_Level` ≠ 0 or DUMMY_GROUND_STRUCTURE)) **or**
   "space-buildable" (0x282400: cost ≥ 1 and (`Required_Star_Base_Level` ≠ 0 or (DUMMY_STAR_BASE and `Base_Level` = 1))).
6. Credits: when check_credits is set, P's credits (+0x70, as int) ≥ cost unless P+0x62 is clear or the global
   0x140b15b20 is set [M: +0x62 reads as "human/local-controlled", 0x140b15b20 as a free-build cheat].
7. `Build_Advances_Tech_Level` (+0x894) ≠ 0 ⇒ P's tech level (+0x84) must equal T's `Tech_Level` (+0x89C) and P's max
   tech (+0x88) ≥ `Build_Advances_Tech_Level`.
8. T is not in P's **locked list** (+0x1C8, 0x282550; written by LOCK_UNIT / Lua `Lock_Tech`, §5.1).
9. `Build_Initially_Locked` (+0xC1) ⇒ T must be in P's unlocked list (+0x1B0); else reason **7**.
10. X's owner (0x3956c0 record +4) = P, and the record's +0xC equals the same id [M: "controlling" vs "owner"].
11. T is neither a company member (+0x2A) nor a squadron member (+0x29) (§1.2), and has no TRANSPORT behaviour (5).
12. X's planet type does not restrict T (0x373490: `Planet_Restricted_Unit_Categories` +0x808 ∩ T `CategoryMask`, or T in
    `Planet_Restricted_Unit_Types` +0x810); else reason **8**.
13. If X has a PLANET behaviour and T is space-buildable: the starbase owner (planet data +0x13C) must be −1 or P.
14. **`Required_Special_Structures`** (0x3706f0; +0xF18 names, resolved pairs at +0xF20, count +0xF28, OR-marker int list
    +0xF38..+0xF40): walk the entries; an entry whose index is in the marker list starts an OR chain with the next
    entry, the chain continuing while the current index is marked. A chain passes if **any** member is present on
    **this planet** (0x4b82c0: a live object of that exact type in planet data +0x158 (ground structures) or +0x170
    (orbital structures)); an unresolvable (null) member passes the chain. All chains must pass; else reason **4**
    with detail = the first type of the failing chain. The parser puts a marker before each `|` (OBJECT_TYPES §3
    code 0x53): `A | B, C` = (A or B) and C. FoC never uses `|`; **TR does** (e.g. `AI_Category_Dummy |
    Story_Category_Dummy` in NR_Government.xml / Research_Dummies.xml), so the OR rule is parity-relevant.
15. **`Required_Orbiting_Units`** (0x285860; resolved list +0xF78, count +0xF80): every listed type must be present, alive
    (+0x3A0 bit 2 clear), among the units in X's orbit, looking into fleets, squadron containers and transport cargo
    (0x3965f0 expansion); else reason **4** with detail = the missing type.
16. P's tech level ≥ `Tech_Level`; else reason **1** (detail = required level).
17. Ground base level (planet data +0x130) ≥ `Required_Ground_Base_Level` (else reason **2**), starbase level (+0x148)
    ≥ `Required_Star_Base_Level` (else reason **3**).
18. Slot capacity: a DUMMY_GROUND_STRUCTURE needs (structures on the surface (+0x160, 0 when no ground base) + queued
    0x5F items in both queues) < max ground structure slots (+0x230 / type +0x92C); a DUMMY_ORBITAL_STRUCTURE the same with
    the orbital count and +0x234 (0x372760).
19. `Required_Planets` (+0xF50, count +0xF60) non-empty ⇒ X's planet type must be listed; else reason **5**.
20. DUMMY_STAR_BASE: `Base_Level` must equal the next starbase level (0x3f8f20: 1 when there is no starbase and the
    planet's max (+0x238 / type +0x668) ≠ 0; else current + 1 when current ≠ 5 and ≤ max; else 0) and max ≥ that level.
    Then any non-P, non-removed, non-stealthed (§2.2) object in X's orbit list (planet data +0xC8/+0xD0) whose fleet data
    has flag +0x94 blocks it: reason **10** [M: "hostile fleet present"].
21. `Build_Max_Instances_Per_Planet` (+0x87C ≥ 0) for 0x5F / 0x60 types: count on the planet (0x3f94f0) + queued
    (0x401ac0) < max; else reason **6**. A DUMMY_ORBITAL_STRUCTURE also needs starbase level ≥ 1.
22. `Build_Limit_Lifetime_Per_Player` (+0x888): 0 ⇒ fail; > 0 ⇒ lifetime built count (P+0x198 map, counting company,
    transport and squadron members recursively, 0x281c70) < limit.
23. `Build_Limit_Current_Per_Player` (+0x880): 0 ⇒ fail; > 0 ⇒ live count (0x281df0) + queued (0x281ac0) < limit, else
    reason **6**. (−1 = unlimited for both.)
24. Final: (planet surface accessible (0x375220: `Planet_Surface_Accessible`, overridden by planet data +0x2C8) **or** T is
    not 0x5B / 0x5D / 0x5F **or** `Is_Generic_Hero` (+0x14)) **and** (no planet data, or not
    `Build_Requires_Initial_Placement` (+0xC3), or T is in planet data +0x110 list).

This closes GALACTIC_ECONOMY open item 12 (tech gate = step 16, a plain ≥ compare) and the A2 gal "0x2804d0 untraced"
note. GalacticSim::buildable should follow this order exactly (the AI copies in galactic_ai_tokens.cpp become
redundant).

### 1.4 Galactic victory monitor [H unless tagged]

`VictoryType` names (parser 0x341ff0), in enum order 0..22: Galactic_All_Planets_Controlled,
Galactic_Opponent_Controls_No_Planets, Galactic_Credits_Accrued, **Galactic_Super_Weapon_Destruction (3)**,
Galactic_Percentage_Control, Galactic_Conquer_Enemy_Homeworld_And_Leader, Galactic_Super_Weapon_Destroys_Last_Enemy_Planet
(6), Galactic_Super_Weapon_Kills_Enemy_Leader (7), Galactic_Kill_Enemy_Leader, Galactic_Control_Named_Planets,
Galactic_Cycles_Elapsed, then the SUB_TACTICAL_* and SKIRMISH_* tactical types. Each campaign test entry is 0x30 bytes:
{type, player id, +8, +0xC int threshold, +0x10 float threshold, +0x14 "met" flag, +0x15, +0x18 name list}.

Polled tests (0x344710, per mode tick; a player whose faction is not `Is_Playable` (+0x20A8) never wins these):

| type | passes when |
|---|---|
| 0 All_Planets_Controlled | every planet's owner is P or allied with P (0x39a2c0), checked over all PLANET objects |
| 1 Opponent_Controls_No_Planets | no planet is owned by an enemy (relation table = 1) with a playable faction; when the name list is non-empty only those factions count (case-insensitive) [M for the human/AI guard] |
| 2 Credits_Accrued | P's credits ≥ threshold (+0xC) |
| 4 Percentage_Control | owned planets / all planets ≥ threshold (+0x10); the local player gets a "TEXT_GALACTIC_PERCENTAGE_CONTROL" progress message while below |
| 5 Conquer_Enemy_Homeworld_And_Leader | P's faction +0x3D8 enemy-faction record: its home planet (+0x2AB8) owned by P and its leader type (+0x2AD8) absent from every planet (0x3fa300) |
| 8 Kill_Enemy_Leader | for each enemy player: its leader type (faction +0x2AD8 → first company member) is on no planet; or the leader object is dying (+0x3A0 & 0x46) with final-blow player (+0x330) = P |
| 9 Control_Named_Planets | every named planet's objects belong to P (galactic only) |
| 10 Cycles_Elapsed | fiscal cycles (galactic mode +0x370) > threshold (galactic only) |
| 3, 6, 7 | **not polled**: event-driven, below |

Event-driven super-weapon victories (all go through 0x343b90, which logs "VICTORY - Victory type %d…" and starts a
countdown in frames):

- **3 Super_Weapon_Destruction** (mode common tick 0x3639d0, when a battle result is reported): if the conflict record
  names a super-weapon object (record +0x28 id ≠ 0x3FFFFF) that is alive and not owned by the battle winner W, and W
  owns **any** object whose type is `Is_Super_Weapon_Killer` (+0x1F; FoC: Rogue Squadron, a named hero, a unique unit),
  the super weapon is killed (0x391310) and, if W has condition 3 in the galactic monitor, W wins (countdown 5).
  It does fire in the original; the A2 gal note "never fires" is wrong for this path. Used by 19 FoC Good campaigns.
- **6 Super_Weapon_Destroys_Last_Enemy_Planet** (same place, when no killer fired): if the super weapon's owner O has
  condition 6 and O's opponent (playable) owns no planet whose planet data +0x2C8 ("destroyed") is clear, O wins
  (countdown 30).
- **7 Super_Weapon_Kills_Enemy_Leader**: `PlanetDestructionAbility` (0x707670, galactic) and the tactical super weapon
  (0x42a7b0) declare it when the attacker has condition 7 and the destroyed planet holds the victim faction's leader
  type (faction +0x2AC8) among its units (countdown 30).

Thrawn's Revenge campaigns use only Galactic_All_Planets_Controlled. Port: implement 0 / 1 / 2 / 4 / 8 / 9 / 10 as
polls, 3 / 6 at battle resolution, 7 in the planet-destruction ability; keep the countdowns.

### 1.5 MagicPlan veto 0x375270 [H]

True iff T's `CategoryMask` (+0x1648) intersects the category named **"Super"** (resolved lazily once through the
category converter and cached in 0x140b2c308). MagicPlan therefore never spawns Super-category types (Death Star,
Executor-class and similar). Auto-resolve uses the same helper for its Super checks (BATTLE_TRANSITIONS §4.3).

### 1.6 Delayed-spawn list (planet data +0x240, service 0x3fc750)

Already specified in P4_SIM_SYSTEMS §6.2 (hero respawn scheduling) and AI_GALACTIC_3 §4.3 (cancellation). The service
also has a **`Respawn_Tied_To_Structure`** branch (type +0x2000 name, resolved +0x2008) for the hero's first
company/squadron member: when the owner has more objects of that structure type than live heroes of the member type,
the hero respawns at a planet holding such a structure (object search with callback 0x3ffdf0, structure type and owner
in globals 0x140b2ef88 / 0x140a288fc) [M]. No FoC/TR XML sets the tag (grep), so it is not parity-relevant now.

### 1.7 Galactic company transports — how ground units move between planets [H]

**Every ground company exists in galactic mode as a TRANSPORT object.** There is no "ground unit in a fleet" without a
transport. This is why the port's saboteurs and heroes look immobile.

- **Creation** (0x400b40, the common "construct type at planet" used by production, starting forces, story / Lua
  spawns, Reinforce, the AI build task and planet setup): dispatch by dummy behaviour — 0x5C starbase, 0x5F ground
  structure, 0x60 orbital structure, **0x5D DUMMY_GROUND_COMPANY → 0x400f30**, 0x5E fighter squadron (orbit "fleet of
  one" container, 0x400650), 0x61 DUMMY_STARSHIP (fleet of one, 0x401660), else "Unknown type" (nothing). A type
  chained at +0x2018 is constructed after it, recursively.
- **0x400f30 (company):** needs `Company_Transport_Unit` (type +0x930, resolved +0x938 / count +0x940; first entry
  used); without it nothing is created. It creates the transport object (that type, at the planet position, owner P),
  gives it the company's display name (+0x140 string → transport data +0x20), records the **company type** in the
  transport data (+0x48; this is the "original type" of §1.1 and of build counts), then creates each `Company_Units`
  member and loads it as cargo (0x4f33a0: cargo vector at transport data +0x08, cargo parent = transport).
  - **Surface or orbit:** the transport is linked to the planet surface (0x4f3bd0) when the surface is accessible
    (else log "surface not accessible"), the planet's surface forces count (0x3f9530) < `Max_Ground_Forces_On_Planet`
    (+0x10C8; FoC 10, TR 10 / Hyper 14) (else "slots full"), and no member is `Always_Spawn_In_Orbit` (type +0x20)
    while (the transport is stealthed (§2.2) or no hostile non-stealthed object is in orbit) [M for the last clause].
    Otherwise a new **`Galactic_Fleet`** object is created at the planet ("TEXT_OBJECT_GALACTIC_FLEET n", a global
    counter 0x140b2a084), the transport is added to it and the fleet is placed in an orbit slot.
- **Transport behaviour data** (object +0xC8): cargo +0x08 (vector +0x10 / count +0x18), name +0x20, **landed planet
  +0x40** (null in orbit), company type +0x48. A transport counts as stealthed / removed (§2.2 predicate) only when all
  its cargo does.
- **Land** (0x4f3bd0; FleetManagement op 5, AI `Land_Units`): refuses if the transport is already landed or the planet
  has no PLANET behaviour; in galactic mode refuses when surface forces ≥ `Max_Ground_Forces_On_Planet` unless the
  planet is the current battle planet. Then: append to the planet's surface list (planet data +0xD8), signal, landed
  planet := X, parent := X, notify (0x4f3430, 0x328720).
- **Launch** (0x4f4690; op 4, AI `Launch_Units`): first the reveal pass 0x4f40c0 (§2.2: `Galactic_Influence_Range`),
  then remove from the surface list, landed planet := null, no parent, position := planet position, and object
  +0x3B0 := −1. The free transport is then put in a fleet by **op 0** (new `Galactic_Fleet`) or op 1/3.
- **FleetManagement executor** (0x5af260; payload: planet +0x50, unit +0x4C, fleet +0x48 (0x3FFFFF = new), op +0x60,
  slot +0x5C, extra ids +0x54 / +0x58, issuer +0x24). Ignored while the galactic pause / battle-pending counter
  0x140b1540c > 0, and in SP while another conflict is pending. Ops [H for behaviour, names M]:
  0 = add unit to fleet (create `Galactic_Fleet` at the planet's orbit slot when new; a new fleet arriving in a hostile
  orbit starts a space conflict, `Initiate_Space_Conflict`); 1 = add to the fleet that contains +0x48, or land if that
  container is a planet; 2 = detach unit from its fleet; 3 = merge fleets (or re-slot when no fleet); 4 = launch;
  5 = land; 6 = new fleet + move toward planet +0x54 (refused for the local player when the planet behaviour's
  0x3fa270 test fails); 7 = like 6 with fleet flag 0x435680; 8 = set orbit slot. Each op emits FleetMergeResult
  signals 5 (merge), 6 (detach), 7 (merge fleets), 8 (landed).
- **Speed**: a fleet's base speed is the minimum `Hyperspace_Speed` (+0x648) over hyperspace-capable (+0x24) members;
  only FLEET-behaviour containers (squadrons, slot +0x336) are looked through to their contents. **Transports are not
  looked through**: they count with their own type (FoC: every Company_Transport_Unit — Gallofree_Transport,
  Imperial_Landing_Craft, Underworld_Hero_Shuttle, Underworld_Infantry_Droid_Shuttle, Shuttle_Tyderium, Alliance_Shuttle
  — has `Hyperspace` yes and `Hyperspace_Speed` 1.5; hero companies use their ship, e.g. Millennium_Falcon, Slave_I).
  The AI's travel estimate (AIBuildTask 0x648e80) likewise uses the Company_Transport_Unit's speed for companies.
  (GALACTIC_ECONOMY §3.1 step 3 "container members are looked through" means fleet containers only.)
- **Saboteur example**: `Underworld_Saboteur_Team` (MINOR_HEROES_EXPANSION.XML) = DUMMY_GROUND_COMPANY,
  `Company_Units` Underworld_Saboteur, `Company_Transport_Unit` Underworld_Hero_Shuttle → a hyperspace-capable transport.
- **Invasion** is the separate `Land_Invade` path (0x2b0520, AI_GALACTIC §1.12); unloading into a tactical battle uses
  `Landing_Transport_Variant` (tactical, TACTICAL_GAPS / BATTLE_TRANSITIONS).

Port model for engine/galactic: represent each company as a transport unit (type = Company_Transport_Unit[0], cargo =
company members, company type kept for counts and the learning system); surface membership = landed transports;
launch / land / form-fleet as the ops above; fleet speed from transports' own Hyperspace_Speed. With that, AI
saboteurs and hero companies can form fleets and jump.

---

## 2. S3 (galactic sim)

### 2.1 Fleet speed multiplier M (object +0x2B8 → +0x20 → +0x230) [H]

- Object +0x2B8 is the object's GameObjectManager; its +0x20 is the owning **GameMode** (set by 0x2af130 when the mode
  creates its managers). Mode +0x230 is rewritten **every mode tick** (0x3639d0, common to all modes):
  **M = D × mode+0x234 × GMM+0x98**, where
  - mode +0x234 = `Object_Max_Speed_Multiplier_Galactic` (GameConstants +0x15BC) for the galactic mode (set in
    `GalacticModeClass::vf1` 0x4b21f0); land uses `…_Land` (+0x15C4), space `…_Space` (+0x15C0); default 1.0 (mode ctor
    0x35a5e0);
  - D (0x140a28568) is a debug game-speed factor, statically 1.0, changed only by debug keys (+0.5 / −0.5, min 1.0,
    gated by the debug flag 0x140a61ab7) in the keyboard handler 0x61fe0;
  - GMM+0x98 (0x140b15478) is set to 1.0 at GameModeManager construction and never written again.
- **Shipped values: FoC `Object_Max_Speed_Multiplier_Galactic` = 5.0, Thrawn's Revenge = 11.34.** The port's M = 1.0 makes
  fleets 5× (FoC) / 11.34× (TR) too slow. GALACTIC_ECONOMY §3.2's worked example (v = 0.525 units/frame) becomes
  2.625 units/frame for FoC.
- **[port] 2026-10-28 (playtest):** after Gabriel's playtest (fleets "way too fast" vs the base game), engine/galactic
  `fleet_speed_multiplier()` returned 1.0 and ignored this constant.
- **Settled 2026-10-29 (G1 hop timing, from the existing Q2 reference trace; tools/probe_mod/g1_hops/):** the original's
  fleets move 0.525 units per tick on an unrouted hop and 2.625 on an HS 5 route (Hyperspace_Speed 1.5 x 0.07 x M x R),
  i.e. **M = 5.0 [H, measured]**; the original's Corellia -> Kuat hop (72.11 units, route HS 5) takes **26 ticks** (0.87 s at
  30 ticks/s). `test_retail_hops` / `test_foc_corellia_kuat_hop` lock the formula to these numbers. The port still applies
  M = 1.0: applying the constant is `kApplyModeSpeedMultiplier = true` in `strategic.cpp`, which moves shared goldens
  (alamo_game_smoke_{skirmish,campaign,land}_golden, alamo_headless_galactic_ai{,_human}, alamo_headless_skirmish_ai_foc{,_land},
  galactic_foc_crc) and reverses the playtest tweak, so it waits for Gabriel. The playtest's "too fast" impression was
  therefore not M (the original has the same ticks per hop); what is still unmeasured is the wall-clock length of that hop in
  the original (stopwatch, see tools/probe_mod/g1_hops/README.md).
> **Status 2026-10-30:** `engine/galactic/src/strategic.cpp:39` now has `kApplyModeSpeedMultiplier = true` and line 40 scales fleet speed by `rules_->constants.fleet_speed_multiplier`, so the port applies M (FoC 5.0) and the "port's M = 1.0" / "still applies M = 1.0" / "waits for Gabriel" statements above are out of date. The comment at `engine/galactic/include/alamo/galactic/economy.hpp:67` still repeats the old claim.
- D also multiplies several type speed getters (0x370fd0, 0x372440, 0x3724d0, 0x372560, 0x3728f0, squadron derivation)
  — always 1.0 in release play.

### 2.2 Galactic stealth and visibility [H mechanism, M details]

- **Stealth predicate 0x3a9760** ("hidden"): true if the object is being removed (+0x3A0 bit 2), or, for a FLEET, if
  every content is hidden (0x4357d0), or, for a TRANSPORT, if every cargo unit is hidden (0x4f4610), else the object's
  stealth byte (component +0x100 → +0x98). Uses: piracy pool (GE §3.1 step 7 skips hidden fleets), the starbase
  blocker (§1.3 step 20), company spawn placement (§1.7), starbase / reveal logic.
- **Writer:** `Galactic_Stealth_Ability` (GalacticStealthAbilityClass): activation (vf30 0x6ff260) sets the byte to 1
  and calls 0x3a86d0 with the ability value; deactivation (vf31 0x6ff330) clears it. Shipped as `Activation_Style`
  `Galactic_Automatic`, `Initially_Enabled` yes, `Evade_Detection_Chance` 0.0 on FoC heroes (Han, Chewbacca, Boba Fett,
  Mara Jade, Kyle Katarn, IG-88, Jabba, Silri, Tyber, TIE Phantom…) and 83 TR abilities.
- **Detection:** `DetectHeroAbility` (vf4 0x6f6e30), per service: candidates = stealthed objects in the detector's
  planet orbit and on its surface; a non-allied candidate is discovered when **not** (detector `Is_Generic_Hero` (+0x14)
  and candidate `Is_Named_Hero` (+0x13)) and either (detector named and candidate generic) or (**one synced draw**
  (0x5324d0) < `Detection_Chance` (ability +0x17C) and 0x3a0b70(candidate, detector)) — then "%s has been discovered by
  %s!" (0x3a0130) and the discovery mark (0x3a0110). With `Detect_Credit_Siphon_Effects` (+0x180) it also detects a
  credit-siphon effect on the planet (draw < chance, 0x3fb010 / 0x3facc0). The draws make detection a synced-RNG
  consumer: order matters for determinism.
- **Visibility refresh** 0x3a51e0(object, player): for the local player, the object is shown only if every behaviour
  agrees (behaviour vf +0xA0), then every behaviour is notified (vf +0xA8). Galactic "fog" itself is the planet
  revealed bytes (PHASE2_ANSWERS Q6, FOG_OF_WAR §0).
- **Launch reveal** (0x4f40c0): R = max(`Galactic_Influence_Range` (+0x1A00)) over the transport and its cargo; R ≤ 0 →
  only the launch planet is revealed for the owner; else every planet within R (squared distance, per unit's own range)
  gets the reveal mark (0x3fd9e0: planet data +0xF0 set) and a visibility refresh for the owner.
  Correction: +0xF0 is a per-planet influence set, not a reveal byte, and the trigger is transport lift-off
  (REMAINING_GAPS §6.1).
- `Is_Stealth_Company` is read only by the AI delivery path (0x48d5e0 is not its reader), and `Planet_*_Stealth` is UI only:
  REMAINING_GAPS §6.2 / §6.3. `See_Minor/Major_Stealth_Heroes` is still not traced.
- Unlock sources for `Build_Initially_Locked` (step 9 of §1.3) and the tech-level sweep: REMAINING_GAPS §1.4–§1.5.

### 2.3 Upkeep [H]

There is **no maintenance deduction**. `Maintenance_Cost` (type +0x798) is read only by the AI perception tokens
`Maintenance` / `MaintenanceUnnormalized` (0x669420, 0x6daf20); `Fleet_Maintenance_Update_Delay_Seconds` (+0x110) has
no reader outside its parser. TR sets Maintenance_Cost on 626 types; it still only feeds the AI tokens. Closes
GALACTIC_ECONOMY open item 4 (no probe needed).

---

## 3. S2 — PerceptionFunctionG equation grammar [H]

Recovered from the boost::spirit (classic) rule types in the DLL's RTTI (full template names at type descriptors
0xf43f0…0xf9fc0) plus the grammar-definition constructor (DLL 0x120b0), which stores each composite parser into its rule
slot and carries the literal characters as immediates. Lowest to highest precedence:

```
expression  := additive ( ( "<=" | ">=" | "!=" | "==" | ">" | "<" ) additive )*      (left-assoc chain)
additive    := term ( ( "+" | "-" ) term )*                                            (left-assoc)
term        := factor ( ( "*" | "/" | "#" ) factor )*                                  (left-assoc)
factor      := number | lookup | indexed | call | "(" expression ")" | "-" factor
number      := real literal (spirit real_p: optional sign, fraction, exponent)
lookup      := ident ( "." ident )+ [ "{" binding ( "," binding )* "}" ]
binding     := ident "=" ( expression | string )
indexed     := ident "[" text "]"          text := alpha ( alnum | "_" | " " | "|" )*
call        := ident "(" [ expression ( "," expression )* ] ")"
ident       := alpha ( alnum | "_" )*      string := '"' ( alnum | "_" | " " )* '"'
```

- Whitespace is skipped everywhere (space_p skipper).
- `#` (random range, AI_GALACTIC_3 §7.3) binds like `*` and `/`: `a * b # c` = `(a*b) # c`.
- Comparisons are the loosest and chain left to right: `a < b < c` = `(a < b) < c`; `X > 0 * Y` = `X > (0*Y)`.
- Unary minus binds tightest; a leading sign on a literal is part of the number (`2 * -3` parses either way).
- A bare identifier is not an operand: lookups need at least one dot (`Variable_Self.IsDefender`,
  `Function_X.Evaluate`). `indexed` is what `GameObjectCategoryType[Fighter | Bomber]` and `HardPointType[Shield_Generator]`
  use inside bindings.
- `call` covers `clamp(x, lo, hi)` (the DLL compares the name with "clamp"; case rule not traced) [M].
- The operator alternatives are ordered `<=`, `>=`, `!=`, `==`, `>`, `<`, so two-character operators win.

Port: if engine/ai's parser gives `*`/`/` and `#` the same level and comparisons the lowest level with left
association, the 11 shipped equations that depend on precedence evaluate as in the original.

---

## 4. STORY open items 2, 3, 8; plot activation; LOCK_UNIT [H]

- **Item 2** — `StoryPlotWrapper.Activate` (0x7243a0) / `Suspend` (0x7247a0): plot = wrapper +0x38; when non-null,
  plot +0x660 := 1 / 0; return 0 values. Nothing else. (Confirms STORY §2.2.)
- **How suspended plots become active**: only three writers of plot +0x660 exist — the Story_Plots loader (tag
  `Active_Plot`), the STORY_ELEMENT reward (0x4624e0) and Lua `Activate` / `Suspend` — plus save/load (below).
- **Item 3** — the T9 time argument is **(float)(current mode frame (mode +0x10)) × seconds-per-frame (0x140b0a344)**,
  computed at the call site (0x5f33b…0x5f352 inside 0x5d990); without a current mode it is the ratio of two frame-rate
  ints (0x140b0a320 / 0x140b0a340). "Paused" = galactic mode and the galactic pause / battle-pending counter 0x140b1540c
  > 0 (STORY §4.1 step 2, unchanged).
- **Item 8** — plot-level save (0x52fe20, called by the story manager save 0x32b380), chunk 0xB: micro 0xC plot pointer
  (fix-up id), 0xD active flag (1 byte), 0xE owning player id; then per event (plot event table walk order): chunk 0xF
  {0x10 event name, 0x2F event type}, chunk 0x11 = the event's own save (vf +0x48; base 0x462fd0: pointer, triggered
  +0x4C, active +0x4D, disabled +0x4F, prereq-satisfaction strings, 14 parameter strings…; ELAPSED adds LastTime
  +0x360, accumulated +0x364, initialised +0x36C, 0x463580); chunk 0x1D/0x1E = the plot's Lua script name (+0x68) and
  script state (0x247e50) when it has one; chunk 0x12 = the event table {key CRC, event pointer} pairs. So the active
  flag, every event's flags and the plot script globals are saved.
- **LOCK_UNIT (reward 0x4D)**: plot's player (plot +0x638); `Reward_Param1` type name → type; missing param →
  "Error! Lock Unit - No unit specified"; unknown type → nothing; else append to the player's locked list (+0x1C8,
  0x27f7c0, no duplicates) and log "Locking production of %s". The unlocked list is not touched. Effect: §1.3 step 8.
  (Already implemented 2026-10-28; this confirms it.)

---

## 5. Is_Obsolete, LuaScriptWrapper, opportunity fire

### 5.1 Is_Obsolete writer [H]
`GameObjectTypeWrapper.Is_Obsolete(player)` reads the player's list +0x1C8 — the same **locked list** that
buildability step 8 checks. Its writer is 0x27f7c0, called by: story reward LOCK_UNIT (0x456970 case 0x4D), the
combined unlock/lock story reward 0x45e8b0, Lua `Lock_Tech` (0x603b20), `HackSuperWeaponAbility` (0x61b790) and
`SpecialAbilityEvent` (0x61b9f0). "Obsolete" = locked from production; no automatic obsolescence exists.

### 5.2 LuaScriptWrapper creator [M]
Created only by 0x258250 (type id 0x42 at +0x20, script at +0x38), reached from `LuaScriptClass` vtable slot 12
(0x247040, a tail call) and `LuaScriptWrapper` slot 12. Slot 12 is the LuaUserVar "convert into a value for another
script VM" virtual: when a script object (or a wrapper of one) is pushed into a different VM — e.g. through GlobalValue
tables or Call_Function parameters — a wrapper is created, cached per (script, target VM) in the map 0x140b0a0a0 when a
VM is given. No shipped script obtains one except via such copies (L1_S4_REMAINING §2.11). Port: create the wrapper in
the cross-VM copy path.

### 5.3 Opportunity-fire consumer [H]
- A hard point that acquires an opportunity target (0x387400, tactical) sends signal **0x21** with
  `OpportunityTargetAcquiredDataClass` {target, hard point} to its owner (or, for a team member, to the team object).
- The task force signal handler (0x5fe5d0, case 0x21) checks the hard point's type (hard point data +0x20 → +0x48)
  against the TF's subscription list (TF +0x108..+0x110, ints 5..10 written by
  `Add/Remove/Clear_Opportunity_Fire_Event_Subscription(s)`, saved in TF vf14 chunk 0xD) and, if subscribed, raises
  plan event **17 Hardpoint_Opportunity_Target_Acquired** with parameters (unit wrapper, target wrapper).

---

## 6. Render (R2, R4)

### 6.1 R2 galactic map "Needs RE"

| item | answer |
|---|---|
| background content | **Scenes** (mode vtable slots +0x100…+0x118): main = mode +0x320 (holds the light record), background = +0x328 (`In_Background` types), midground = +0x330 (`In_Midground`), midforeground = +0x338 (`In_Midforeground`), created in 0x4b1d60 [H]. FoC: `Galaxy_Core_Art_Model` (w_galaxy00.alo, IDLE, looping idle anim) has no flag → **main scene**; midground = `Light/Heavy_Galactic_Corruption_Particles`, midforeground = `Galactic_Corruption_Line_Particles`; no galactic star dome or other background type is shipped [H data]. **TR** marks Galaxy_Core_Art_Model `In_Background` yes (scale 4.6, `Camera_Aligned` yes) → background scene in TR. The common tail draws the three non-main scenes first (near 10 / far 100000, depth cleared after the first only, RENDER §1.2), so corruption particles render before the main scene [M visual consequence]. |
| galactic light record | Main scene +0x320, set by 0x4b1d60 at mode init: ambient **(0.05, 0.05, 0.05)**; sun = light 0, colour (1,1,1), intensity **0.5**, direction from `Galactic_Zoom_Out_Light_Angle` elements [1] = yaw, [2] = elevation (FoC 160°, 58°; TR 0°, 90°; element [0] unused); fill lights 1 and 2 intensity 0 [H]. Direction (0x276d80): (sin yaw · cos elev, −cos yaw · cos elev, sin elev) [M for which of sin/cos is which]. During a planet zoom (0x3c3bf0, wall-clock ms / `GMC_ZoomTime`, clamped 0..1) intensity lerps to `Galactic_Zoom_Light_Level` (FoC/TR 0.5) and the angles to `Galactic_Zoom_In_Light_Angle` [1]/[2] (FoC 235°, 18°; TR 235°, 90°); at the end the story/GUI event "zoomed_in" / "zoomed_out" is sent (0x32edd0) [H]. Replaces the port's ambient 0.12. |
| Camera_Shift_X / _Y / Camera_Distance (CampaignDef +0xCC/+0xD0/+0xD4) | Parsed; no reader found in the galactic mode or camera code [M]. FoC data never sets them; **TR sets them on its "divider" campaigns** (Conquest\Dividers\Shell.xml: 40 / 0 / 1200), which suggests the campaign-selection galaxy preview. Probe: screenshot TR's campaign menu. |
| TradeRouteLines Zoomed_In vs Zoomed_Out | Parser layout [H] (0x5c20b0, 0x5c2a10): `<Settings_For_Faction Name=…>` blocks of 0x90 bytes indexed ENEMY 0, ALLY 1, any other name 2 (neutral), GOOD 3, EVIL 4, CORRUPT 5; defaults Color_Zoomed_Out = Color_Zoomed_In = (0.5,0.5,0.5,1), Width 0, Render_Mode "ADDITIVE", flag 1, Texture `W_galaxy_line_alpha.tga`, spark `w_galaxy_dot.tga`. The renderer-side switch between the two colours was not found [open; capture]. |
| starbase icon size / placement | not traced [open; capture] |
| `w_planet_fleet.alo` Fleet_ALT0 / ALT1 | not exe strings; selection is the generic `_ALTn` proxy filter (ANIM_PARTICLES §B.7) driven by the object's alt index, whose galactic writer was not traced [open; capture] |
| ownership / political-control display | not traced beyond the planet-label path (UI §1.6) [open] |

> **Status 2026-10-30:** §6.1 "galactic light record" row: the port no longer uses ambient 0.12; `engine/render/galactic/src/galactic_view.cpp:1140` sets the ambient to (0.05, 0.05, 0.05), as the row says.

### 6.2 R4 throttle and system scalars [H]

- **Throttle global 0x140a12648 is statically 1** in the exe's initialised data and nothing writes it (no code reference
  other than reads, no pointer to it). So the invisible-system throttle is **on**: a system that was updated before
  (+0x31E) and was not drawn last frame (+0xC9 clear) accumulates d in +0xCC and updates only once ≥ 0.1 s has gathered,
  using the accumulated d (alParticleGroup vf7 0x171120). The same global also forces the per-emitter update paths
  regardless of system flag +0xC8. Corrects ANIM_PARTICLES §B.4 "throttle is off". Particles-enabled 0x140a12649 = 1 and
  Particle Detail 0x140a12570 = 1.0 statically.
- **Scalar setter** 0x173220(system, index, value*, smoothing seconds s; s = 0 → 0.01 and snap): index 0 → spring
  target +0x2D4 (value +0x2D0, rate 1/s at +0x2E0) and re-evaluates every emitter's active slots (0x1731a0) = the
  detail scale (ANIM_PARTICLES "scalar 2"); index 1 → target +0x2AC (value +0x2A8, rate +0x2B8) = the emission scale
  ("scalar 0") [M for the doc's numbering]; index 2 → +0x2BC/+0x2C0 immediately; index 4 → colour RGBA target
  +0x2F4…+0x300 (value +0x2E4…, rate +0x318). Both targets default to 1.0 (ctor 0x170e10).
- **Writers:** galactic planet corruption (PlanetaryBehavior vf6 0x3fe1b0): colour := lerp of planet data
  +0x308/+0x30C, index 1 := lerp +0x310/+0x314, index 0 := lerp +0x318/+0x31C, all by the corruption transition fraction
  (0x4b7e20) — these pairs come from `Particle_Brightness / _Scale / _Energy_Per_Corruption_Level` [M for the
  pair→tag mapping]; weather (LandMode 0x46a0c0 / 0x468980: index 0 = intensity lerp); SurfaceFX (0x5ee570: colour fade,
  index 0 = intensity, s = 0.1); Lure (0x62b6f0: colour fade to 0 over 0.6 × lure time); TransportLanding (0x642c70 /
  0x643c30: index 0 = 0, or x/2 + 0.5 for x ≥ 0.5); model API 0x266630 (colour for emitters whose name matches a
  prefix); 0x5edab0 (colour alpha).
- Corruption fraction formula, transition state and the pair→tag mapping (now [H]: brightness → colour, scale →
  index 1, energy → index 0): GALACTIC_REVEAL_CORRUPTION.md §4 (2026-10-28).

---

## 7. Presentation (P1, P2, P3, AUDIO)

### 7.1 P1 camera scrolling [H]

Per input update (0x2ee40, called once per main-loop iteration — frame-rate dependent in the original):
- Target scroll vector T (0x3c360: edge/keyboard scroll, scaled by `Push_Scroll_Speed_Modifier` +0x298).
- If T = 0 and the current velocity v ≠ 0: v *= 1 / `Scroll_Deceleration_Factor` (FoC/TR 1.5) per update; when |vx| < 0.1
  and |vy| < 0.1, v := 0.
- Else: v += `Scroll_Acceleration_Factor` (0.08) × T per update, clamped per component so it never passes T (toward T's
  sign). The tactical camera (0x2d0820) or the galactic camera then scrolls by v.
- **`Camera_Stop_Left` / `_Right`** are read only by 0x3c8d0 (clamp the space camera's x to [−50, 50]), which has **no
  caller and no pointer** → unused. `Camera_Z_Position` has no reader. `Galactic_Scroll_Plane` (+0x1C8) is copied into
  the galactic camera (0x3c2c00, camera +8) [M: scroll plane height].
- Answered in **P1_CAMERA_INPUT.md** (2026-10-28): the scroll plane is the look-at z, T is in speed × dt units with the
  tactical scale 0.0003 × distance, mouse modes and units, SmoothCD smoothing, wheel / galactic planet zoom, deselect, keymap.

### 7.2 P2 HUD list selectors and health-bar scales [H]

- The HUD layer draw (0x2ffcb0) submits each list with 0x13d4e0(prim, **selector**, z-always). The selector picks the
  2D effect in 0x182ac0 (fixed-function fallback in brackets when shaders are off):
  0 `Engine\PrimOpaque` (no blend), 1 `PrimAdditive` (ONE/ONE, tex×diffuse), 2 `PrimAlpha` (SRCALPHA/INVSRCALPHA,
  alpha = tex×diffuse), 3 `PrimModulate` (ZERO/SRCCOLOR, colour = tex+diffuse), 4 `PrimDepthSpriteAdditive`,
  5 `PrimDepthSpriteAlpha`, 6 `PrimDepthSpriteModulate`, 7 `PrimDiffuseAlpha`, 8 `StencilDarken` / `StencilDarkenToAlpha`,
  9 `StencilDarkenFinalBlur`, 10 `PrimHeat`, 11 `PrimParticleBumpAlpha`, 12 `PrimDecalBumpAlpha`, 13 `PrimAlphaScanlines`
  (effects loaded in 0x17e1a0). Z write off, z-func ALWAYS for these calls.
- So the four quad lists are: plain quads → **0 PrimOpaque**, fill dials → **2 PrimAlpha**, alternate dials →
  **1 PrimAdditive**, extra quads → **3 PrimModulate**. PrimOpaque is a fixed-function opaque pass (docs/shaders
  CATALOG); whether alpha testing from earlier state survives is visible only in a capture [M].
- `Health_Bar_Scale` / `Land_Health_Bar_Scale` / `Min_Health_Bar_Scale` (GameConstants +0x118A4/+0x118A8/+0x118AC): only
  the GameConstants constructor touches them (defaults 20, 20, 0); **no reader** → unused. Keep fixed-size bars.

### 7.3 P3 video alpha loops [H]

All Bink playback goes through 0x44d920 (Lua `Play_Bink_Movie`, hologram speech, advisor loops, full-screen movies):
- Movie record (Movies.xml): `Movie_File` +0x28, `Commandbar_Offset` +0x98, **`Alpha` +0xD2**, `Cannot_Skip` +0xD5,
  `Loop` +0x3D; flags +0xD0 (movie music), +0xD1, +0xD3 (pause the mode), +0xD4 (stop music).
- Bink opens at 30 fps with flags 0x801000, **| 0x100000 (alpha plane) when `Alpha`**, | 0x2000 when looping. Frames are
  copied as 32-bit BGRA with alpha (copy type 5) into a texture created with an alpha format when `Alpha`.
- The movie quad is a 2D sprite with colour (1,1,1,1) and selector **2 PrimAlpha** (straight SRCALPHA/INVSRCALPHA)
  for alpha movies, **0 PrimOpaque** otherwise. Without the HUD flag (argument 6) the sprite goes into the full-screen
  2D scene (0x25c080, or the overlay add 0x25c220 when `Alpha` or +0xD1); with it, the HUD component (Movie_tactical /
  Movie_galactic) shows the texture [M for the HUD quad's selector].
- Per frame (0x44dea0): frame-keyed SFX (0x5dbd60), **music cues (music mode 17)** (0x5dbc40) and speech (0x5dbe70)
  from the movie database.
- FFmpeg's Bink decoder must output the alpha plane (yuva) for these files; blend straight alpha, no premultiply.

### 7.4 AUDIO §8 [H unless tagged]

Music modes (callers of Play 0x2d21f0 / Play-by-name 0x2d2070):

| mode | user |
|---|---|
| 0 | debug SFX/music event list dialog (0x5bcd0) |
| 1, 5, 6, 7, 9, 10, 12 | as AUDIO §2.3 |
| 4 | production complete `Build_Music_Completed` (resume previous) |
| 6 (default) | Hutt Palace / Cantina structure selection, forced |
| **8** | tactical super weapon activation (TSW type +0x21A0) |
| **11** | splash screen `Splash_E3_Music_Event` |
| **13** | front-end menus (`Main_Menu_Music_Event`, staging / campaign / internet dialogs), by name |
| **14** | movie music at movie start (movie flag +0xD0; resume previous) [M for the name] |
| 15 | battle load screen |
| 16 | battle end dialog (win / lose) |
| 17 | music cues embedded in a playing movie |

- **Lock flag** (manager +0x80, 0x140b27e40): set to 1 when the attract/demo loop starts (0x22f10), cleared by any
  front-end action (0x22bf0 and its callers). While set, mode-13 requests are refused unless forced. So "front end
  music" = mode 13 (not 8) and battle end = 16 (not 11): correct the port choices in PHASE4 P3.
- **Unit-response "excluded set"** (mode +0x30, 0x3400c0) is the mode's **retreat coordinator**: true when a retreat is
  in progress (+0x1C ≠ −1) and the object id is in its retreating list (+0x38/+0x40). Retreating units give no unit
  responses (also used by Take_Damage and the Select events) [H structure, M naming].
- Miles rolloff law: not visible in the exe (library) — probe-only (capture levels at known distances).
- GUI-only flag owner (SFX manager +0x94): not traced.

---

## 8. Probe list (prepare under tools/probe_mod/, Gabriel runs)

| id | question | observe |
|---|---|---|
| G1 | Fleet speed M = 5.0 (FoC) | **answered in ticks 2026-10-29** from the Q2 trace (tools/probe_mod/g1_hops/analyze_hops.py: M = 5.0, Corellia -> Kuat 26 ticks); only the wall-clock stopwatch check is left (g1_hops/README.md) |
| G2 | Survival ratio | AI-vs-AI galactic; Lua cannot read it — debugger watch on 0x58a9a0 hits per type |
| G3 | Stealth detection draws | detector hero vs stealthed hero at one planet; count discoveries over N services |
| R1 | TR campaign-menu camera (Camera_Shift_*) | screenshot TR's campaign selection screen |
| R2 | Trade-route zoom colours, starbase icons, Fleet_ALT | galactic map captures zoomed in and out (RENDER §7.2 plan A/B) |
| U1 | PrimOpaque HUD quads | capture of the tactical HUD (alpha-test state on list 1) |
| A1 | Miles rolloff | play a 3D SFX at 3 distances, record levels |

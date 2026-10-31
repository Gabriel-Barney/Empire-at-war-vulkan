# Battle transitions: galactic conflict → tactical battle or auto-resolve → galaxy

Clean-room behavioural spec for roadmap **S10 "Battle transitions"**. Source: the Ghidra 12.2 pass of
StarWarsG.exe (FoC Steam x64 1.121), decompiled output and the CSV exports in `docs/re/ghidra/`. RVAs
are relative to 0x140000000. Tags: **[H]** read directly from the code, **[M]** inferred from code
structure or names, **[L]** a guess or only partly traced.

Builds on, without repeating: GALACTIC.md (path finder: planets in battle are impassable),
GALACTIC_ECONOMY.md §3.3 (fleet arrival and interception) and §4 (ownership), MAIN_LOOP.md §1.5 and
§2 (pause counter, mode stack, Start_Game/End_Game_Mode, RNG), OBJECT_RUNTIME.md, COMMANDS.md
(Retreat command 22), TACTICAL_COMBAT.md, MOVEMENT.md, SPACE_AND_OPEN.md.

Globals used below:

| name | address | meaning |
|---|---|---|
| GMM | 0x140b153e0 | GameModeManager. +0x18 game type, +0x2c galactic pause counter, +0x38 current mode, +0x48/+0x50 mode stack, +0xa0 battle-load callback, +0xa8 alternate battle-start callback, +0xf0..+0x17a queued battle record, +0x1f4/+0x1f8/+0x1fc super-weapon result [L] |
| current mode | 0x140b15418 (= GMM+0x38) | +0x10 mode frame, +0x18 GOM, +0x28 victory monitor, +0x30 retreat coordinator, +0xa0 story manager |
| GameConstants | 0x140b15800 | XML fields at the offsets of `xml_field_tables.md` (e.g. +0x1588 Overrun_Auto_Resolve_Multiple) |
| AutoResolve | 0x140b31400 | the single `AutoResolveClass` (0x2f8 bytes, ctor 0x490de0, created by 0x4913a0 at boot) |
| sync RNG | 0x140a13e24 | MAIN_LOOP §4.1; `int(lo,hi)` = 0x1ffb40 |
| FS frame | 0x140b0a320 | global logic frame; FS+0x20 (0x140b0a340) = 30 |
| GUI hooks | 0x140b155f8 … 0x140b15660 | function pointers registered by the GUI (battle-pending dialog, auto-resolve dialog, "is dialog busy" tests). The sim only calls them; a headless port treats them as absent |

---

## 0. Summary for the S10 implementer

1. **Conflicts are queued records, not events.** The galactic GOM keeps two FIFO vectors of 0x58-byte
   `ConflictInfoStruct`: space conflicts at GOM+0x468 and land invasions at GOM+0x480. A space conflict
   is queued when a fleet arrives at a planet held by an enemy (fleets in orbit or a starbase); a land
   conflict is queued **only by an explicit invade order** (InvadeEvent, Lua `Invade`, story). [H]
2. **One conflict of each kind advances per galactic tick**, always the head of its queue, space before
   land, at the end of the galactic GOM service (0x2be640). A per-GOM state machine at GOM+0x618 drives
   the head through validation → choice (fight / auto-resolve / retreat) → tactical battle or
   auto-resolve → result. [H]
3. **The galaxy is frozen while a conflict is being decided** (galactic pause counter GMM+0x2c raised:
   mode frame stops, global frame and RNG continue). A tactical battle pushes a Land/Space mode on the
   stack; the galactic mode does not tick until the battle mode is popped. [H]
4. **Auto-resolve is deterministic given the sync RNG**: per-category strength vectors from
   `AI_Combat_Power`, an AI-contrast-weighted exchange of fire, a winner by remaining strength, then a
   per-unit survival roll ordered by sync RNG draws against a "remaining strength budget" scaled by
   the winner/loser attrition constants. Formula in §4. [H] for structure, [M] for some edge rules.
5. **Results write back to the galaxy through the parent mode's GOM**: losers' fleets retreat to a
   neighbouring planet, starbases are destroyed or downgraded, production queues are cleared on
   capture, non-playable losers are wiped, story events "battle won/lost/retreat" fire. §3. [H]/[M]

---

## 1. Conflict detection on the galactic map

### 1.1 Conflict records [H]

`ConflictInfoStruct` (0x58 bytes, vector element; `DynamicVectorClass<ConflictInfoStruct>` RTTI):

| off | type | meaning |
|---|---|---|
| +0x00 | int | planet object id |
| +0x04 | int | attacker player id |
| +0x08 | int | defender player id |
| +0x10 | DynamicVectorClass<int> | extra object ids (land: the invading units; space: empty) |
| +0x28 | int | object id, 0x3fffff = none (a story/"lead" object, read by the battle-end code §3.1) [M] |
| +0x30 | std::string | explicit tactical map name (story override) |
| +0x50 | bool | flag [L] |

Space queue: GOM+0x468 (data +0x470, count +0x478, capacity +0x47c). Land queue: GOM+0x480 (data
+0x488, count +0x490). Getters: 0x2ad0e0 (space head or null), 0x2ac5a0 (land head or null).

### 1.2 Space conflict creation [H]

1. A fleet reaching a hop planet calls the orbit-entry function 0x4338d0 (GALACTIC_ECONOMY §3.3). It
   returns **conflict = true** when, and only when the "suppress" argument is false and either:
   - the planet's starbase level (planet data +0x148) > 0 and the starbase owner (+0x13c) is an
     enemy of the fleet owner (`Is_Enemy` 0x2824f0; logged `!!! Conflict! Fleet owner = %s and
     starbase owner = %s`), or
   - another fleet in orbit (planet data +0xc8 list, count +0xd0) that is not in transit (0x3a9760)
     has an alive member and its owner is an enemy (0x39a450).
   It also posts a fleet-arrival story event (0x327560 on mode+0xa0) in both cases.
2. On conflict the arrival code (`FleetLocomotorBehavior` vf6 0x56a1e0, and the other callers listed in
   §6) calls **`GameObjectManagerClass::Initiate_Space_Conflict` 0x2af7c0**(GOM, planet, attacker,
   defender = −1, map = null, at_front = 0):
   - the defender, when not given, is the owner of the first orbiting fleet not in transit whose owner
     ≠ attacker; otherwise the starbase owner if the starbase level > 0 and that owner ≠ attacker;
     otherwise **no conflict is queued**;
   - the record is appended to the space queue, or inserted at index 0 when `at_front` is set
     (0x29bb40; used by story/queued replays, §2.7);
   - if the attacker is the local player and a story "battle hologram" flag matches (0x140b155d4/d8),
     a speech/movie is queued (UI only).
3. Interception: an intermediate-hop conflict stops the fleet there, except for factions with
   `Benefits_From_Corruption` (GALACTIC_ECONOMY §3.3). A planet with any queued conflict is
   impassable for the path finder (GALACTIC.md §3, check 4).

### 1.3 Land conflict creation [H]

Land battles are never automatic. **`GameObjectManagerClass::Land_Invade` 0x2b0520**(GOM, planet id,
unit id list, attacker, defender = −1, map, at_front) is called by the invade command (InvadeEventClass,
event factory id 9; via 0x48ff50), Lua `GalacticTaskForce.Invade` 0x6ce600 /
`GameObject.Invade` 0x57ada0, the story reward `INVADE_PLANET` (0x45f5c0) and the queued-battle replay
(0x28f590). Lua `Invade` refuses when the fleet is not at a planet, the planet owner is not an enemy,
or the fleet has no land units/transports.

Defender when not given: the ground-base owner (+0x124) if the ground-base level (+0x130) > 0;
otherwise the planet owner from 0x3956c0 if it is set and is not the neutral player (+0x4c of
0x294aa0); otherwise the owner of the first ground unit on the surface (0x397120). The record is queued
only when attacker ≠ defender.

### 1.4 Per-tick processing order [H]

At the end of the galactic GOM service 0x2be640 (MAIN_LOOP §3.3 step 8, "conflict detection"):

1. States 2/3 of a battle that was already chosen (pre-battle cinematic finished → state 3, release one
   pause level (0x28c720), close the dialog (0x28c6c0)).
2. **Story block**: in single-player, when a story/tutorial sequence is active (0x140b154d0 or a
   non-empty list at 0x140b2b360) and the local player is attacker or defender of any queued conflict,
   nothing below runs this tick.
3. **Space head**: 0x2ad100 splits the units in orbit of the head's planet into attacker units and
   defender units (by owner id, skipping dead objects, flag +0x3a0 bit 1). If any exist and no GUI
   dialog is busy, the space state machine 0x2b92e0 runs.
4. **Land head**: if the land queue is non-empty and no GUI dialog is busy, the land state machine
   0x2b76a0 runs.

So conflicts are resolved strictly in queue order, at most one space and one land head per tick; a
conflict queued this tick is handled from the next GOM service on (the queue is appended during
object services earlier in the same service pass, so in practice it can start in the same tick) [M].

### 1.5 The state machine (GOM+0x618) [H for the transitions, M for the names]

| value | meaning |
|---|---|
| −1 | new head: validate, tag every participating unit (signal 0x11 "in conflict", 0x39fe00 per unit/fleet member), → 4 |
| 4 | decide fight / auto-resolve / ask (§1.6) |
| 0 | waiting for the MP battle vote (`PENDING_TACTICAL_BATTLE_VOTE`) |
| 5 | waiting for the player's choice in the battle-pending dialog (GUI hook) |
| 6 | auto-resolve chosen → set up AutoResolve, → 7 |
| 7 | auto-resolve running; timer at GOM+0x30 (space) / +0x28 (land) |
| 2 | tactical battle chosen, pre-battle cinematic playing |
| 3 | start the tactical battle (0x28fda0) |
| 9 | sandbox linked battle set up from the queued record (§2.9); no branch handles it (idle until the battle end resets −1) |

**Validation** (space 0x2befe0, land 0x2becb0) runs first on every pass. The head is dropped when
attacker == defender, or when one side has no alive unit that `Can_Participate_In_Space_Battle`
(type +0x2e) and whose combatant can fight, and the defender has no starbase. Dropping: signal 0x11
to the planet, remove the head (shift the vector), `0x2b4660` releases the units of both sides,
release the pause level if one was taken. Validation always passes while an auto-resolve is active.

### 1.6 Choosing fight, auto-resolve or retreat (state 4)

Inputs:

- **Game type** (`Is_Multiplayer` 0x28af60: game type 1, 2 or 4).
- **MP auto-resolve option** GMM+0xc8 (0x140b154a8) with the "option present" bit 0x20 of GMM+0xdc
  (0x140b154bc). Values (comment in GameConstants.xml, `MP_Default_Allow_Auto_Resolve`): 0 = always,
  1 = never, 2 = ask, 3 = overwhelming odds. [H] for the reads, [M] for the meaning.
- **Story lock** 0x4472a0 (story manager 0x140b30350): "this planet's battle must be fought" (global
  flag +0x41 or the planet in the list at +0x308; story rewards `DISABLE/ENABLE/RESTRICT_AUTORESOLVE`).
- **Overrun**: `Overrun_Auto_Resolve_Multiple` (GameConstants +0x1588; vanilla line is commented out,
  so 0 = disabled).

Single-player rules (space; land 0x2b76a0 is the same with ground strengths):

1. If the local player is attacker or defender, a story "battle pending at planet" notification is
   posted (0x449f10) and the battle is *human-involved*.
2. Overrun check, only when the multiple ≠ 0 and the battle is not story-locked:
   - defender strength = 0x2ab880 (orbit units of the defender + starbase power);
   - attacker strength = Σ over attacker units of 0x395d10 (unit/fleet combat power, §4.1);
   - if max > min × multiple, the weaker side is the overrun loser (GOM+0x638) **unless that side has a
     named hero** (`Is_Named_Hero`, 0x38fa20, recursive through fleets/transports; land logs
     `Planet %s - Overrun autoresolve not possible because %s is present.`); then GOM+0x635 = 1.
3. **Auto-resolve without asking** (state 6, GOM+0x634 = 1) when the battle is not human-involved
   (AI vs AI), or an overrun was found, or the global flag 0x140a7c46f is set [L: demo/attract mode].
4. Otherwise (human involved, no overrun), in this order:
   - option bit set and option 0 ("always"): state 5 through GUI hook 0x140b15608 (space) /
     0x140b15628 (land), timer = `Space/Land_Auto_Resolve_Delay_Seconds` (GameConstants +0x118 /
     +0x11c) — the auto-resolve presentation [M];
   - story-locked, or no option bit (normal single-player), or option 2 ("ask"): build the force lists
     (0x2bce30, §2.3). Then, when `Strategic_Queue_Tactical_Battles` (GameConstants +0x118a1, vanilla
     True) is set and the option is not 0: if the local player is not a participant (MP: unless both
     players are human with player flag +0x180) the tactical battle starts at once (0x28fda0);
     otherwise state 0: raise the pause, story notification 0x28d7f0, and open the vote
     `PENDING_TACTICAL_BATTLE_VOTE` (0x28adb0 → vote manager GMM+0x180). This vote **is** the
     battle-pending dialog (`pending_battle_shell`: Fight `TEXT_BUTTON_BEGIN`, Auto-resolve
     `TEXT_BUTTON_RESOLVE_BATTLE`; **no Retreat button**, corrected 2026-10-28 in GALACTIC_DIALOGS.md §1) in single-player too; its resolution (GMM::Update,
     MAIN_LOOP §3.1 step 4) sets state 6 (auto-resolve) or 2/3 (fight) [M]; "retreat" calls
     `AutoResolve.Set_Retreating(player)` (vf5, §4.6) [M]. With the constant false the state stays 4
     [L];
   - remaining case (option bit set, option 1 or 3, not story-locked): GUI hook 0x140b15638(kind), state
     5, force lists built.
   [H] for the branches, [M] for the dialog meanings.

Multiplayer: if either player is AI (not `Is_Human`, player +0x62), auto-resolve; option 0 forces
auto-resolve; option 1 or a story lock forces tactical; option 2 asks both humans through the vote
manager (GMM+0x180, MAIN_LOOP §3.1 step 4) with `AutoResolveVoteDefaultTimeOut` (15 s, wall clock,
MAIN_LOOP §4.4) and `AutoResolveVoteDefaultToTactical`. The vote result is applied by a scheduled
`ResumeGameEvent`, so all peers switch on the same frame. [M]

Galactic pause: every path that waits (states 0, 5, 6/7, 2) raises GMM+0x2c once (0x28bb80, only if
it is 0), and every exit releases it (0x28c720). [H]

---

## 2. Battle setup

### 2.1 Starting the battle — 0x28fda0(GMM, kind 1 land / 2 space, planet, defender, map, flag) [H]

1. **Map name**, first match wins (later steps overwrite earlier ones):
   1. the planet's current map string (planet data +0x1e0);
   2. if the planet type has `Land_Tactical_Map` (+0x690) / `Space_Tactical_Map` (+0x670), use
      `Data\Art\Maps\<name>`; otherwise ask the random map picker 0x47bf40 (object 0x140b311a8) with
      (2 + space, terrain index from the type's `Terrain` +0x848 — TEMPERATE 0, ARCTIC 1, DESERT 2,
      FOREST 3, SWAMP 4, VOLCANIC 5, URBAN 6, SPACE 7 —, defender faction index — Rebel 0, Empire 1,
      Pirates 2, Underworld 4, Hutts 5, other 0 —, base level, 2) and store its answer back into
      planet data +0x1e0, so the planet keeps the same map for later battles;
   3. a story override (`SET_TACTICAL_MAP` / `STORY_LOAD_TACTICAL_MAP`: string at 0x140b30450 land,
      0x140b30430 space) as `Data\Art\Maps\<name>`; it is consumed (cleared) [M];
   4. the conflict record's explicit map (+0x30) as `Data\Art\Maps\<name>`;
   5. if still empty: `Data\Art\Maps\_Desert_L5_01.ted` (land) / `_Space_Temperate1.ted` (space).
   A planet with an unknown `Terrain` string fails the start (returns 0).
   `Destroyed_Space_Tactical_Map` and `Corruption_1..3_Tactical_Map_Name` exist as fields but were not
   seen in this path [L].
2. **Base level** = ground-base level (+0x130) for land, starbase level (+0x148) for space; it selects
   the base layout in the map (`Loading map %s for level %d base`).
3. The battle-load callback GMM+0xa0 is called with (map, base level, defender id — or −1 and game
   type 3 when GMM+0x1e5 is set —, game type GMM+0x18, flag). This is
   `BattleLoadDialogClass::Activate` → `Load_Map` → **Start_Game(1|2)**: push of the tactical mode
   (MAIN_LOOP §2.4). The planet is stored in 0x140b30428 for the loader.
4. After the load: GOM post-load steps (0x2a1eb0, 0x35cd90, 0x2aafb0, 0x2b3e00) with load-progress
   0.75/0.9/1.0, then the victory conditions (§2.5), then optional object spawns (§2.6), then the
   story tactical hand-off (§2.8).

An alternate entry GMM+0xa8 (used for MP/replays [L]) receives a record {kind, planet, defender, map}
instead.

### 2.2 Which units take part [H]

- **Space**: every object in orbit of the planet (0x395900 list) owned by the attacker or the defender
  (0x2ad100). Fleets are expanded into their members. The planet itself takes part as the starbase /
  space defences holder.
- **Land**: the invading unit ids in the record (+0x10) and the ground units/structures of the defender
  on the surface. Garrisons (`Garrison_Reinforcement_Delay_Seconds`, `Additional_Garrison_Units`) are
  spawned by the tactical map/base code, not by this setup [L].

### 2.3 Starting force vs reinforcement pool — 0x2bce30 / 0x2bd500 (space) [H]

0x2bce30 fills two per-player maps on the **galactic** GOM, keyed by player id, of
`ObjectPersistenceClass::tPersistentUnit` id lists:

- GOM+0x498: **units placed at battle start**;
- GOM+0x4d8: **reinforcement pool** (units that can be called in during the battle).

It also snapshots the planet (GOM+0x518) and its starbase fields (+0x138..+0x150 → GOM+0x520..+0x530).
The tactical side reads these maps through 0x28aaa0 (parent GOM+0x498) and resolves ids with
0x28aa50.

The split 0x2bd500(units, start, reserve, forced_type, flagship_flag), applied to the defender first,
then to the attacker:

1. Units whose type has property bit 0x2c in table 2 (0x370320; always-deployed, e.g. structures [L])
   always go to *start*.
2. If `forced_type` is given: the first unit of that type goes to *start*, all others (except rule 1)
   to *reserve*, and the split ends.
3. If `flagship_flag`: the first unit whose data +0x1b0 has byte +0x32 set (the fleet's lead ship;
   logged `Setting pathfinder for space battle - %s`) goes to *start*, the rest to *reserve*; end.
4. Otherwise a **population cap** split. Cap = the owner faction's `Space_Tactical_Unit_Cap`
   (faction +0x2cd0, float). Pop(t) = `Space_Override_Population_Value` if ≥ 0, else
   `Population_Value` (0x373500(t, 2)).
   - Container units (squadron/team holders, behaviour slot +0x337 ≠ −1) take one cap slot each, in
     list order, until the cap is used; the rest go to *reserve*.
   - Plain units are grouped by type key (+0xec) in a list ordered by first appearance.
   - Pass A, per group: Pop < 1 → the whole group to *start*; else if Pop ≤ cap left → one unit (the
     last) to *start*, cap −= Pop.
   - Pass B, if units are left (n): per group, take k = min(count, ⌊(count / n) × cap + 0.5⌋ / Pop)
     (integer division) from the back; used += k × Pop; then cap −= used.
   - Pass C, per group, while Pop ≤ cap: one more to *start*, cap −= Pop.
   - Everything left goes to *reserve*.

Land uses 0x2bc6a0 (called with (GOM, 1, lead)); not traced. Land reinforcement in the battle is the
tactical reinforcement-point system (`ReinforcementPointBehaviorClass`, `Reinforcement_*` fields,
Lua `Spawn_From_Reinforcement_Pool`) [L].

### 2.4 Starting positions [M]

Space maps carry marker objects; `ATTACKER ENTRY POSITION` is looked up by type name (0x533220,
0x49e060) to orient arrivals and retreats. Defender units start at their map/base positions; attacker
reinforcements use `Space_Reinforcement_Collision_Check_Distance` and reinforcement points on land.
Not traced further.

### 2.5 Victory conditions registered at setup [H]

For every player that is not `Is_Neutral` (faction +0x108), the battle's victory monitor (mode+0x28,
event 0x341f00) gets:

- type 0xb `SUB_TACTICAL_ALL_ENEMY_UNITS_DESTROYED` and type 0xc
  `SUB_TACTICAL_ALL_CAPABLE_ENEMY_UNITS_RETREATED` (record kind 0xb / 0xc);
- for AI players, a type-0x11 entry copied from the player's AI victory data [M];
- land with `Land_Base_Destruction_Forces_Retreat` (GameConstants +0x11e6d): type 0xd
  (`..._LAND_ALL_ROGUE_FACTION_BASE_COMPONENTS_DESTROYED`) for every player and type 0xe
  (`..._LAND_ALL_ENEMY_BASE_COMPONENTS_DESTROYED`) for the attacker;
- space with `Space_Station_Destruction_Forces_Retreat` (+0x11e6e): type 0xf
  (`SUB_TACTICAL_SPACE_ENEMY_STARBASE_DESTROYED`) for the attacker.

How and when these are evaluated (event-driven 0xb, hold counter, story rules): §3.1a.

VictoryType values (enum table 0x341ff0): 0 `Galactic_All_Planets_Controlled` … 10
`Galactic_Cycles_Elapsed`, 0xb..0x11 as above plus 0x10 `SUB_TACTICAL_STORY_MODE_TRIGGERED_VICTORY`,
0x11 `SUB_TACTICAL_ENEMY_LEADER`, 0x12..0x16 skirmish types.

### 2.6 Galactic → tactical object mapping

Tactical objects are created by the map loader from the persistent id lists (§2.3); the link back is
the galactic object id (`tPersistentUnit`). Fleets are not objects in tactical; their members are.
Companies/squadrons are expanded by the tactical spawner (`Company_Units`, `Squadron_Units`). [M]
Planet data +0x2c8 (space): extra objects from the planet type list +0xd20 are spawned for the local
player at a found marker (0x2a8b50, 0x29f810) [L].

### 2.7 Queued battle record (GMM+0xf0) [H for fields, M for use]

0x28f590 starts a battle from the one stored "linked battle" record, written by 0x28bd70 from the story
reward LINK_TACTICAL and the corruption-mission start (not by `STORY_LOAD_TACTICAL_MAP`, see L1_S4_REMAINING
§4.4): kind (+0xf4: 1 land, 2 space), attacker player id (+0xf8), planet id (+0xfc), map (+0x100),
**defender** faction name (+0x120), forced/hero type (+0x140), Story_Plots script (+0x148), sandbox flag
(+0x168), presentation options (+0x16c..+0x171), post-battle resolution (+0x174, copied to GOM+0x61c),
"banner shown" (+0x178), "show banner first" (+0x179), space flagship split (+0x17a). It re-queues the
conflict at the front (`at_front` = 1) and then **always starts the tactical battle at once** (0x28fda0);
the sandbox flag only chooses between state 9 + a human-only force split (0x2bce30 / 0x2bc6a0) and
clearing both force maps (no galactic units carried in). The state machine never decides such a battle.
Callers: the LINK_TACTICAL handler, the corruption mission, every battle end (0x3639d0 tail), the
state 1/2 path of 0x2be640 after the pending banner, and the post-load path 0x28bc80. Full detail §2.9.

### 2.8 RNG and story hand-off

- The sync RNG is **not reseeded** when the tactical mode is pushed (MAIN_LOOP §4.1). [H]
- The battle start happens inside the galactic GOM service (state 3) or a callback from a GUI command;
  the map load then runs on the load thread while no tick runs (FS+0x34). Whether the loader draws from
  the sync RNG is unknown (open item 3). [L]
- If the story allows tactical story mode (0x140b30544 and the local player's flag +0x3c0 clear), the
  new mode's +0x2cf is set and the story manager is told the defender (0x329530); the tactical story
  scripts then run on the tactical mode's +0xa0 (T9). [M]

### 2.9 Story-linked battles (LINK_TACTICAL) and the queued-record start

Answers how a story mission battle starts, including a LAND one with no invading units. Sources: handler
0x45f9c0, 0x28bd70, 0x28f590, 0x2b0520, 0x2af7c0, 0x2bc6a0, 0x2b76a0, 0x2be640, 0x28bf10 → 0x2a7580 →
0x2aa510 / 0x2aa8e0, 0x87770, shipped FoC Story XMLs.

**2.9.1 Reward params (reward 0x3e, handler 0x45f9c0)** [H]. Param N is the string at reward +0x50 +
0x20·(N−1); the handler's own log line names them "Planet, Mode, Attacker, Map, Defender, Hero, Script,
sandbox". P1–P5 must be non-empty, else the reward silently does nothing.

| P | meaning | conversion | default |
|---|---|---|---|
| 1 | planet | object **type** name → first object of that type in the galactic GOM (0x407e00, 0x2a87d0); unknown → log `Invalid planet %s in Link_Tactical` and continue with a null planet (null dereference in the original, [M]; port: refuse) | — |
| 2 | mode | story keyword map 0x140b30728 (0x45c3f0): GROUND 0 → kind 1 (land), SPACE 1 → kind 2 (space); any other mapped value (EITHER 2) → log `Invalid combat location`, no battle. **A word missing from the map returns 0 = land**, so the shipped `LAND` works | — |
| 3 | attacking faction | faction (0x3303d0) → its player (0x294d30) → id (+0x4c); unknown → log `Invalid attacking faction`, no battle | — |
| 4 | map file | string, used as `Data\Art\Maps\<P4>` by 0x28fda0 (§2.1 step 1.4) | — |
| 5 | defender faction | kept as a **string** (+0x120); resolved to a player only when the battle starts | — |
| 6 | hero / forced type | object type (0x407e00), null when absent or unknown | none |
| 7 | Story_Plots file | string → +0x148 | empty |
| 8 | sandbox | `atoi ≠ 0` | false |
| 9 | arrival options | int → 0x2cbb50(0x140b27a10, v): 0 → +0x34e = 0, +0x34f = 0; 2 → both 1; anything else → +0x34e = 1 only. +0x34f is the reinforcement **hyperspace-hold flag 0x140b27d5f** (L1_S4_REMAINING §3.4 "Hyperspace in"; cleared by Lua `Resume_Hyperspace_In`; this is its writer); +0x34e (0x140b27d5e) is read with `In_Game_Cinematics` by the formation code 0x4fbb10 [M: arrival cinematic on]. Also stored in GMM+0x30 | 1 |
| 10 | presentation flag A | bool → overlay object 0x140a28c10 +0xf, 0x140a28c93 = !v; when v and P9 ≠ 1 also 0x140b27ee0 = 0, 0x140b27ee4 = 6, 0x140a28c20 = 1 [L: letterbox / fade setup]; stored in GMM+0x34 | false |
| 11 | presentation flag B | bool → 0x140a28c10 +0xe and the cinematic input-block flag 0x140a28c91 (UI.md) [M: battle opens in cinematic mode] | false |
| 12 | post-battle resolution | keyword: RETREAT_PLAYER 1, RETREAT_AI 2, DESTROY_AI 3; unknown (0) → log `Unknown resolution`, use 3 | **3 (DESTROY_AI)** |
| 13 | show the pending banner first | bool | **true** |
| 14 | space flagship split | bool (0x2bd500 rule 3) | true |

The handler then calls **0x28bd70**(GMM, planet id, kind, attacker id, P4, P5, P6 type, P7, P8, P9, P10,
P11, P12, P13, P14) and, **if the current mode is galactic** (mode vf +0xe0 == 0), calls 0x28f590 at once.
It ignores 0x28bd70's result.

**2.9.2 0x28bd70: write the record** [H]. Refused (returns 0, nothing written) when planet id = 0x3fffff,
kind ∉ {1, 2}, attacker = −1, P4 or P5 empty, **a record is already present (GMM+0xf0)**, or no current
mode. Otherwise: reset the record (0x28c640: present 0, ids −1 / 0x3fffff, strings empty, +0x168 0,
+0x16c 1, +0x170/+0x171 0, +0x174 0, +0x178 0, +0x179 1, +0x17a 1), then present := 1 and the fields as
listed in §2.7; the script is copied only when non-empty. Also sets **0x140b30545** (linked-battle flag) and
**0x140b305ed** (skip the battle-end dialog, §2.9.6). Rewards run synchronously (STORY.md §1), so the
record is written in the same tick as the triggering story event.

**2.9.3 0x28f590: start the record** [H]
1. **Wait while a pending-battle banner is up**: if `Strategic_Queue_Tactical_Battles` (GameConstants
   +0x118a1 = 0x140b270a1), the galactic pause counter > 0 and the galactic GOM state is 0, return 0; the
   record stays.
2. **Banner stage** (record present, +0x178 = 0, +0x179 = 1): galactic GOM state := 0, raise one pause
   level (0x28bb80 logic inline), 0x28d7f0(GMM, record planet, kind == land) shows the pending banner (the
   one-button `story_pending_battle_shell` is chosen because 0x140b30545 is set, 0x3ca480), +0x178 := 1,
   return 0. The player's Begin (0x28c7e0) sets state 1 in SP (state 3 in MP); the GOM service 0x2be640
   then plays the zoom-in (state 1 → 2, record planet GMM+0xfc) and, once 0x140b303c0 is clear: state 3,
   release the pause, hide the banner, and **because the record is present: state −1 and 0x28f590 again**
   (now +0x178 = 1, so it starts). That path also wins over a normal conflict whose banner was up when a
   record arrived (the normal conflict stays queued behind it) [M].
3. Copy the record locally and clear it (0x28c640: GMM+0xf0 = 0, the §6.4 story block ends here). Stop
   (return 0) unless kind ∈ {1, 2}, attacker ≠ −1, planet id ≠ 0x3fffff, map non-empty and the planet object
   exists.
4. Defender = the player of faction name +0x120 (−1 if unknown). GOM+0x61c := resolution;
   0x140b30548 := script (or empty: read and cleared by the tactical mode init 0x3b81b0 / 0x4d7820, which
   loads it as the battle's story plot for the local player, 0x329aa0); apply P9–P11 (table above);
   GMM+0x30 / +0x34 := P9 / P10 (re-applied after a save load by 0x52d10); GMM+0x1e3 := 0.
5. **Space**: `Initiate_Space_Conflict`(planet, attacker, defender, P4, at_front 1). Not sandbox → clear both
   force maps (0x2b6f90 = 0x4f2fb0 on GOM+0x498: start map, reserve map +0x4d8, planet snapshot +0x518).
   Sandbox → state 9; orbit units split (0x2ad100: defender list, attacker list); a side's list is emptied
   when its player is AI (defender = the P5 player, so an unknown P5 crashes here [M]); 0x2bce30(defender
   list, attacker list, forced type P6, flagship P14).
6. **Land**: `Land_Invade`(planet id, **unit list = the global int vector 0x140a172b0**, attacker, defender,
   P4, at_front 1), then that vector is cleared (0x32ba40(0, 0)). The vector holds the unit ids of the last
   invade command (0x48ff50, every id of the event) or single-unit landing (FleetManagement 0x5af260 case 5)
   and is cleared by nothing else, so: a LINK_TACTICAL fired by STORY_LAND_ON / STORY_DEPLOY gets the units
   the player just landed; one fired by STORY_TRIGGER / ENTER / MOVE gets whatever ids are left from an
   earlier landing, usually none [M]. Not sandbox → clear the force maps (as space). Sandbox → state 9 and
   0x2bc6a0(GOM, humanOnly 1, forced type P6).
7. 0x140b30544 := 0 (the new tactical mode's +0x2cf story flag stays 0 and 0x329530 is not called, §2.8);
   then **0x28fda0(GMM, kind, planet, defender, P4, 1)** (or the alternate callback GMM+0xa8 with {kind,
   planet, defender, map}). The battle load starts inside this call, so with P13 = 0 the battle begins in
   the same tick as the story event; with P13 = 1 after the banner and zoom.

**Who starts it when.** Reward fired in galactic mode → at once (or after the banner). Reward fired while a
tactical mode is current (none in shipped data) → the record waits and the battle-end tail's 0x28f590
starts it right after that battle. After loading a save with a record present, 0x28bc80 clears +0x178 and
calls 0x28f590 (the banner is shown again if P13). [H]

**2.9.4 Validation and decide** [H]. Neither runs for a linked battle in practice: 0x28f590 starts the
battle itself, the galactic GOM does not tick during the battle, and the battle end (0x28bf10 → 0x2a7580)
sets the state to −1 and removes the head. In the state machines the order is: (land only) **return at once
when the head's unit list is empty** (0x2b76a0 first test, record +0x20), then validation (0x2becb0 /
0x2befe0, on every pass, any state), then states 0 / 3 / −1(decide); **state 9 has no branch** (idle). So
validation is not skipped by state 9, but it is only reached if 0x28fda0 failed (e.g. a planet with an
unknown `Terrain`) and the record stayed queued: a land record with no units then sits at the head forever
and blocks the land queue; one with units is validated and then idles (sandbox) or is decided normally
(not sandbox, state −1) [M]. No overrun test, auto-resolve, MP vote or normal pending dialog is used; the
only choice is the optional one-button banner (no auto-resolve).

**2.9.5 Forces** [H]
- **Not sandbox (P8 = 0, e.g. Underworld M01 Kessel, M11 Coruscant):** both force maps are cleared, so no
  galactic unit of either side enters the battle; P6 and P14 are unused. Everything comes from the map
  (objects placed in the .ted, e.g. `UMP_TYBER_ZANN` found by `Find_First_Object` in
  STORY_UNDERWORLD_ACTI_M01_LAND.LUA) and the P7 plot's Lua spawns.
- **Sandbox land (0x2bc6a0, humanOnly = 1):** clears both maps; defender: every surface object (0x397120)
  → start, **only when the defender is human**; attacker (only when human): lead from the record units with
  P6 as forced type (TACTICAL_GAPS §4.2; the lead is landed, raid rules apply); every attacker transport in
  attacker fleets in orbit → reserve. **With an empty unit list there is no lead: the attacker's start list
  is created empty and only its orbiting transports go to the reserve.** Planet snapshot +0x518 (base
  fields only when the defender is human). If the land queue is empty (Land_Invade refused) it returns
  before clearing anything.
- **Sandbox space:** human sides' orbit units through 0x2bce30 with P6 as `forced_type` (that type starts,
  all others reserve) else the P14 flagship rule, else the population split (§2.3). AI sides get nothing
  from the galaxy.

**2.9.6 Battle end** [H unless tagged]. The normal path (§3.1–3.3), not the corruption skip: tail of
0x3639d0 reads the parent's queue head (the linked record, at the front), 0x2bff60 planet write-back,
0x28bf10(kind, winner, R, P, flag 0x140b30545). Differences:
- 0x28bf10 skips the retreat of R (land; space: only the defender side's fleet retreat) when the local player
  won and GOM+0x61c ≠ 0.
- 0x2a7580 (end of 0x28bf10) sets state −1, lands the record's units if the attacker won (none when the
  list is empty), sets GMM+0x1e3 := 1 if the state was 9 [L: read by the tactical tick 0x364b20], and, **if
  the winner is human and GOM+0x61c ≠ 0, applies the resolution** (land 0x2aa510, space 0x2aa8e0), then
  GOM+0x61c := 0 and the head is removed:
  - RETREAT_PLAYER: retreat (0x2ba850 / 0x2ba6c0) the ground units (fleets) of attacker and defender that
    are human; if the ground base (starbase) owner is human, the base is cut to level 0 (0x4bb1a0 removes
    structures above level 0) [M for the base step];
  - RETREAT_AI: the same for AI players; if the base owner is AI, its structures are processed (0x27f370
    per type) and the base cut to level 0 [M];
  - DESTROY_AI: every AI-owned surface object (space: AI fleets and members in orbit) is destroyed
    (containers released first), then as RETREAT_AI for the base [M].
  Default P12 is 3, so every link without P12 destroys the AI's units at the planet on a human win.
- The battle-end dialog callback 0x87770 sees 0x140b305ed: clears it, releases every galactic pause
  level, runs 0x32e910 on the galactic story (0x5303b0 per active plot), sets galactic mode +0x188 = 1 and
  **returns without the battle-summary dialog** [M for the dialog meaning].
- 0x140b30545 is cleared when the galactic pause counter reaches 0 (0x28c720), so with a banner (P13 = 1)
  it is usually 0 again at the battle end; without one it is still 1 and 0x2ba850 uses its alternative
  destination order (§3.3) [M].
- The tail then calls 0x28f590 (no record left → nothing).

**2.9.7 Refusals** [H]. `Land_Invade` queues only when attacker ≠ defender (defender derived only when −1,
§1.3); `Initiate_Space_Conflict` with an explicit defender always queues, with −1 only when an enemy
fleet/starbase defender exists. Both are void: 0x28f590 does not check, logs nothing and does not restore
the record (already cleared). It still sets state 9 / clears the force maps and **starts the battle**; at
the end the tail dereferences the parent's (empty or unrelated) queue head [M: crash or wrong record]. Not
reached by shipped data (P3 ≠ P5 everywhere). Port: refuse the start and log.

**2.9.8 Shipped FoC LINK_TACTICAL rewards** (foc_data_text.zip, Story_*.xml; Underworld rows in full, P4 /
P7 elsewhere follow the mission name). "–" = absent (default).

| file / event | trigger | P1 | P2 | P3 → P5 | P6 | P8 | P9 | P10 | P11 | P12 | P13 | P14 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| UNDERWORLD `Underworld_ActI_M01_Load` (UM01_A_Crimelord_Unleashed.TED, Story_Plots_Underworld_M01_LAND) | TRIGGER | Kessel | LAND | Underworld → Empire | "" | 0 | 0 | 1 | 1 | RETREAT_PLAYER | 0 | – |
| UNDERWORLD `..._M02_Start` (UM02) | ENTER | Yavin | SPACE | Underworld → Rebel | "" | 1 | 0 | 1 | 0 | RETREAT_PLAYER | 0 | 0 |
| UNDERWORLD `..._M03_Start` (UM03) | DEPLOY | Saleucami | LAND | Underworld → Hutts | "" | 1 | 0 | 1 | 0 | DESTROY_AI | 0 | – |
| UNDERWORLD `..._M04_Start` (UM04) | DEPLOY | Hypori | LAND | Underworld → Pirates | "" | 1 | 0 | 1 | 0 | – | 0 | – |
| UNDERWORLD `Underworld_ActII_M06_Start` (UM06) | ENTER | Dathomir | LAND | Underworld → Empire | "" | 1 | 0 | 1 | 0 | RETREAT_PLAYER | 0 | – |
| UNDERWORLD `..._M07_Start` (UM07) | DEPLOY | Bespin_Story | LAND | Underworld → Empire | Tyber_Zann_Passenger | 1 | 2 | 1 | 1 | RETREAT_PLAYER | 0 | – |
| UNDERWORLD `..._M09_Start` (UM09) | ENTER | Carida | SPACE | Underworld → Empire | "" | 1 | 0 | 1 | 0 | RETREAT_PLAYER | 0 | 0 |
| UNDERWORLD `..._M11_Start` (UM11) | ENTER | Coruscant | LAND | Underworld → Empire | "" | 0 | 0 | 1 | 0 | RETREAT_PLAYER | 0 | – |
| UNDERWORLD `..._M12_Start` (UM12) | ENTER | Kuat | SPACE | Underworld → Empire | "" | 1 | 0 | 1 | 0 | – | 0 | – |
| EMPIRE_ACT_I M02 / M03 / M04 | TRIGGER / MOVE / MOVE | Thyferra / Ilum / Geonosis | GROUND / SPACE / SPACE | Empire → Rebel / Pirates / Rebel | Darth_Vader / Boba_Fett_Team / – | 0 / 1 / 1 | 0 / 0 / 2 | 1 | 1 / 0 / 0 | – | 0 / – / – | – |
| EMPIRE_ACT_II M04 / M05 / M06 (+ restart) | ENTER / LAND_ON / MOVE (TRIGGER) | MonCalimari / Kashyyyk / Carida | SPACE / LAND / LAND | Empire → Rebel; M06: Rebel → Empire | – | 1 | – / 0 / 0 | – / 1 / 1 | – / 0 / 1 | – | – | – |
| EMPIRE_ACT_III M09 (+ restart) | MOVE (TRIGGER) | Bothawui | GROUND | Empire → Rebel | Emperor_Palpatine_Team (restart: –) | 1 | 0 | 1 | 0 | – | – | – |
| EMPIRE_ACT_IV M10 / M11 | ENTER | Tatooine / Alderaan | SPACE | Empire → Rebel | – / Death_Star | 1 | 2 / – | 1 / – | 0 / – | – / RETREAT_PLAYER | – | – |
| REBEL_ACT_I M01 / M02 / M03 | TRIGGER / DEPLOY / LAND_ON | Kuat / Wayland / Fresia | SPACE / GROUND / GROUND | Rebel → Empire | "" / Droids_Team / "" | 1 | 0 | 1 | 0 | – / RETREAT_PLAYER ×2 | 0 / – / – | – |
| REBEL_ACT_II M04 / M06 | ENTER / LAND_ON | Kessel / Kashyyyk | SPACE / LAND | Rebel → Empire | – | 1 | 0 | 1 | 0 | RETREAT_PLAYER / – | – | – |
| REBEL_ACT_III M07 / M07A / M08 / M09 / M10 | MOVE / ENTER ×3 / TRIGGER | Atzerri / Corellia / VergessoAsteroids / Corulag / Carida | SPACE ×4, LAND | Rebel → Empire | M08: HAN_SOLO_TEAM | 1 | 2 / 2 / – / 2 / 0 | 1 (M08 –) | 0 (M08 –) | – | – | – |
| TUTORIAL I / III / V / VI / VII | TRIGGER / ENTER / LAND_ON / TRIGGER / TRIGGER | Kashyyyk / Kashyyyk / Ryloth / Endor / Tatooine | LAND / SPACE / LAND / LAND / SPACE | I, VI, VII: Rebel → Empire; III, V: Empire → Rebel | – | 1 | I: 0 | I: 1 | I: 1 | – | I, VI, VII: 0 | – |
| demos / tests (UNDERWORLD_DEMO, _EXEC_DEMO, _FOCUS_DEMO ×2, _TUTORIAL, _TEST_CAMPAIGN(_MISSION_12), REBEL_STEVE_TEST, SANDBOX_FOCUSDEMO_EMPIRE ×2) | various | Felucia, endor, Bespin, Kuat, Coruscant, Ilum, Bothawui | LAND / SPACE / GROUND | Underworld → Empire / Rebel; copies of campaign links | Tyber_Zann(_Prologue), Vengeance_Frigate, … | 1 (exec demo 0) | 0 | see XML | see XML | RETREAT_PLAYER / RETREAT_AI / "" / – | 0 (steve test, sandbox –) | – |

Observations: 19 of the 45 links use the non-keyword `LAND` (works as land, §2.9.1; 6 use GROUND, 20 SPACE). Every Underworld campaign link
sets P13 = 0 (starts in the same tick, no banner); most EaW-campaign links leave P13 at the default and get
the banner. Land links fired by DEPLOY / LAND_ON (UM03, UM04, UM07, Empire M05, Rebel M02/M03/M06, Tutorial
V) carry the just-landed units into a sandbox battle; the TRIGGER / ENTER / MOVE land links (UM01 Kessel,
UM06, UM11, Empire M02, M06, M09, Rebel M10, Tutorial I / VI) carry no planned units (empty or stale
vector), and three of them (UM01, UM11, Empire M02) are not sandbox at all.

**2.9.9 Open items**
1. Exact meaning of P10 and P11 (overlay 0x140a28c10 +0xe / +0xf, 0x140b27ee0/ee4, 0x140a28c20) and of
   0x140b27d5e; reader of GMM+0x30 / +0x34 besides the save-load restore.
2. 0x4bb1a0 / 0x27f370 in the resolutions (base cut and the per-type player call) — [M] only.
3. GMM+0x1e3 reader 0x364b20 (one-shot check 10 s into the next tactical battle) and 0x140b30391 (no reader
   found).
4. Linked record arriving while a normal conflict's banner is up and the player picks auto-resolve
   (0x28c7e0 with param 1 → 0x29c020): when the record is then started was not traced.
5. A link fired by STORY_ENTER / MOVE can coexist with a normal space conflict queued by the same fleet
   arrival (no suppression like the invade's, 0x326a30); the normal conflict is left behind the linked one.
   Probe: Empire Act I M03 (Ilum) — does a second battle or auto-resolve follow the mission?
6. Probe LT1: Underworld M01 Kessel — no galactic units in the battle, no battle-summary dialog, AI units
   at Kessel after the win (RETREAT_PLAYER keeps them; Empire M02 Thyferra with default DESTROY_AI removes
   them). Probe LT2: P9 = 2 mission (Bespin UM07 or Rebel M07 Atzerri) — reinforcements hold in hyperspace
   until the script calls `Resume_Hyperspace_In`.

---

## 3. Battle end and write-back

### 3.1 Detecting the end (tactical common tick 0x3639d0 tail) [H]

When the victory monitor (mode+0x28) reports a result (0x342dc0, read with 0x342d80: victory type,
winner id) and the mode is tactical with a parent:

1. Space/land head of the **parent** GOM gives planet P, attacker A, defender D. Loser L = A if the
   winner is D, else D.
2. **Non-playable loser** (faction `Is_Playable` +0x20a8 false, e.g. pirates/neutral defenders): all
   of L's objects in the galaxy are killed (0x2a6710: collect from the player's object list, 0x391310
   each) and L's persistent units are flagged for removal (0x2a6840, bit 2 of +0x3a0). (MAIN_LOOP §2.4
   step 4 called these "survivor transfer"; that is incorrect.)
3. **Retreat-player R** (what the galaxy must move away):
   - victory type 0xe (land base destroyed) → R = D;
   - type 0x10 (story victory) → R = L;
   - type 0xf (starbase destroyed) → if D is not playable and not neutral, D is wiped (as step 2);
     otherwise R = D;
   - otherwise R = the player the retreat coordinator reports as retreated (mode+0x30, 0x33fe20), or
     −1.
4. Hero/story checks: if the record's lead object (+0x28) exists and is not the winner's, and the
   winner has a unit flagged `Is_Super_Weapon_Killer`-style type flag +0x1f … the lead object is
   destroyed and victory events are queued (type 3/6 records, 0x343b90). [L]
5. The parent galactic mode stores the result: +0x2bc winner, +0x2c0 loser, +0x2c4 planet id, +0x2c8
   battle kind.
6. 0x28f520 → parent GOM 0x2bff60(winner) (§3.2), **pop the tactical mode** (0x28f320), then
   0x28bf10(kind, winner, R, P, flag 0x140b30545) (§3.3), 0x28c4c0.
7. Story events on the galactic story manager: land 0x32f4d0(P, winner) + 0x32a340(P, loser player);
   space 0x32f570 + 0x32a3b0; if R ≠ −1, 0x32b330(R) (retreat event). Visibility refresh for the local
   player (0x2b59b0). Then 0x28f590 starts the next pending battle, if any.

### 3.1a Victory monitor: what counts and when (2026-10-31)

Answers why a story battle with no player units at frame 0 (Underworld M01 Kessel) does not end at once.
Sources: 0x344710 (evaluate), 0x346460 / 0x28f570 (test wrappers), 0x343b90 (declare), 0x342d80 / 0x342dc0
(result ready), 0x344670 (per-frame countdown), 0x341fe0 / 0x3440a0 / 0x343f80 (hold), 0x2c0340 (object-loss
trigger), 0x2a64f0, 0x4ca2f0, 0x5b07c0, 0x63fae0, 0x340750, 0x28fda0 registration, reward switch 0x456970,
0x462b70, 0x372230, 0x39a450, 0x2a9ff0; shipped FoC XML / Lua; maps.meg UM01 TED.

**Monitor layout** (`VictoryMonitorClass`, mode +0x28) [H fields, M names]: +0x00 mode; +0x08..+0x18 vector
of condition records (0x30 bytes: +0 type, +4 player id, +8 countdown frames, +0xc int, +0x10 float, +0x14
"satisfied" byte, +0x15 "silent" byte = no win/lose text, +0x18 vector of faction-name strings); +0x20
declared; +0x24 countdown; +0x28 declared record (copy); +0x58 last frame seen; +0x5c **hold counter**;
+0x60 `DynamicVectorClass<AwaitingVictoryTestType>` (data +0x68, count +0x70: deferred {type, player} pairs);
+0x79 / +0x7a end-wait flags. Records added at setup (§2.5) carry countdown 0xd2 = 210 frames (7 s).

**1. Counting rule for 0xb `SUB_TACTICAL_ALL_ENEMY_UNITS_DESTROYED` (test for player E)** [H]
- Only tested when E is the battle's attacker or defender (0x28a780 / 0x28a910); otherwise no victory.
- Candidates: every object in the tactical GOM's full object list (gom +0x38) whose owner (+0x58) ≠ E
  (0x2a9ff0 with no behaviour / list / radius filter). An object **blocks** E's victory when all hold:
  - its own type (+0x298, not a container's) is victory-relevant in this mode (0x372230): `Victory_Relevant`
    (type +0x5e) → always; else land → `Land_Victory_Relevant` (+0x60), space → `Space_Victory_Relevant`
    (+0x5f). All three default false (OBJECT_TYPES §2);
  - not dying / removed (obj +0x3a0 bits 0x40 and 0x02 clear);
  - its effective owner is an **enemy** of E (0x39a450 → `Is_Enemy` 0x2824f0); the effective owner is +0x58,
    or, for a galactic planet object (+0x335 ≠ −1), the planet's owner (not relevant in tactical).
- No blocker → E wins type 0xb. Nothing else is looked at: **no** behaviour test (SELECTABLE, LOCOMOTOR,
  UNIT_AI), no prop / structure / unit category, no Hide, invulnerability, Prevent_All_Fire, cinematic or
  letterbox state, no Is_Neutral test of the object's owner (a Neutral player is simply not an enemy). Garrisoned
  or transported objects are GOM objects with their own type and count like any other [M]. Units still in the
  reinforcement pool / reserve are not GOM objects and never count [M]. `Variant_Of_Existing_Type` children
  copy the parent's flags (OBJECT_TYPES §3.2), so a story variant of a hero counts if the hero does.
- 0xc `..._ALL_CAPABLE_ENEMY_UNITS_RETREATED` (player E): needs the retreat coordinator (mode +0x30) with
  0x33fe20 true and a retreating player R (+0x1c ≠ −1), R ≠ E and R an enemy of E → E wins. It does not count
  objects at all [H].

**2. When it is evaluated** [H]
- **0xb is not polled.** Its only trigger is 0x2c0340(gom, object, old owner P), called when a victory-relevant
  object (0x372230 on its type) **leaves** player P in a tactical mode: removal from the GOM (0x2a64f0, every
  death / despawn), owner change (0x4ca2f0 capture-style change incl. contained objects, 0x5b07c0, vehicle
  thief 0x63fae0). For every player E ≠ P that is an enemy of P: if the mode has a parent (campaign battle,
  0x28aa00) and E is attacker or defender → test 0xb, then (land base bytes +0x394 / +0x38e) 0xd / 0xe, or space
  0xf; without a parent (skirmish) the 0x12 / 0x13 / 0x14 / 0x16 types instead. So a side that **never had or
  lost** a victory-relevant object cannot lose by 0xb, and nothing is evaluated at frame 0 / 1 [H].
- 0xc only from 0x340750 (retreat coordinator reaches state 3, "retreat_complete"), for every enemy of R that is
  attacker or defender. 0x340750 first releases **all** hold levels without replay (0x343f80(…, 0)).
- **Gates inside 0x344710** (every type): hold +0x5c > 0 → append {type, player} to the deferred queue
  (0x341f40, no duplicates) and answer "no"; already declared (+0x20) → "yes" only for the declared {type,
  player}; no records → "no"; global byte 0x140b303c1 set → "no" (no direct writer found, [L]); a further
  galactic-only gate (player +0x62, mode kind 0). A record whose +0x14 is already set is declared directly.
- **Declare** (0x343b90): only if {type, player} is a registered record and nothing is declared yet. Sets
  +0x20, copies the record, countdown +0x24 := record +8, logs `VICTORY - Victory type %d, player ID …`, for the
  tactical types (0xb..0x11, 0x12..0x16) notifies the mode's story (0x32f3e0) and shows TEXT_WIN_TACTICAL /
  TEXT_LOSE_TACTICAL (corruption-mission variants) unless +0x15 is set.
- **Per frame** only the countdown runs (0x344670 from the tactical tick, skipped while 0x140b15490 is set):
  +0x24 −= frames elapsed; at ≤ 0 with +0x79 set it waits on callbacks 0x140b30720 / 0x140b30718 (countdown
  held at 1) [L purpose]. The battle ends (§3.1) when 0x342d80 / 0x342dc0 report: hold ≤ 0 **and** declared
  **and** countdown ≤ 0. A hold raised after a declaration therefore also delays the end.
- **No start delay and no "has had units" latch** exist; the event-driven trigger plays that role.

**3. Hold counter +0x5c and story rules** [H]
- Raise (0x341fe0, +1): story reward **ENABLE_VICTORY** (enum 0x1d in 0x456970) with `atoi(P1) == 0` (P1 must be
  non-empty; note `Rebel` also gives 0, REBEL_ACTIII_M09); TSW Fire 0x42a200 (TSW_PLANET_DESTROY §2); full-screen
  movie start 0x60330.
- Release one level (0x3440a0, replay 1 → when it reaches 0 the deferred tests are re-run in order, first
  declaration stops): ENABLE_VICTORY with `atoi(P1) ≠ 0` **only if** +0x5c > 0 (extra `1`s are no-ops); TSW
  state 13; movie end 0x61fe0.
- Release all levels **without** replay (0x343f80(…, 0); the deferred queue is kept but not re-run): the
  **VICTORY** reward (0x462b70) and retreat completion 0x340750. (TSW_PLANET_DESTROY §2 calls 0x343f80 a variant
  "with the same replay"; every caller passes 0.)
- **VICTORY reward** (0x462b70): P1 faction → its player W (else log `Unable to find faction %s to force
  victory`); release all holds; add record {0x10 `SUB_TACTICAL_STORY_MODE_TRIGGERED_VICTORY`, W, countdown 7 s ×
  FPS} (P2 ≠ 0: 1 s and +0x15 silent) and test it at once (0x28f570). Type 0x10 always succeeds when tested.
- **Not** victory-related: Lua `Suspend_AI`, `Lock_Controls`, `Fade_Screen_Out`, letterbox / cinematic mode,
  Story_Event; none of them calls the monitor (only the callers listed above exist). The linked-battle flag
  0x140b30545, the LINK_TACTICAL P10 / P11 flags and the story-mode byte +0x2cf are not read by any victory
  function. 0x364b20 is the 10-s battle-start advisor line (faction SFX by situation), not a victory check.
- **Registration** (0x28fda0, §2.5) is unconditional for every non-Is_Neutral player: story / linked battles get
  0xb and 0xc like any battle; no reward or Lua removes records. Story missions instead **hold** the monitor and
  end through the VICTORY reward. ENABLE_VICTORY 0 is used by 60+ shipped FoC story XMLs (every Underworld
  campaign tactical mission UM01–UM11 (not UM12), most EaW campaign tactical missions, the tutorials and the corruption missions).

**4. Kessel UM01 at frame 0 / 1** [H unless tagged]
- Map: one `UMP_TYBER_ZANN` placement (TED object id 0x13234, mini 02 owner = 6 = Underworld in campaign,
  REMAINING_GAPS §3.3). `UMP_TYBER_ZANN` is `Variant_Of_Existing_Type TYBER_ZANN`, and `Tyber_Zann`
  (UNITS_HERO_UNDERWORLD_TYBER_ZANN.XML) has `Victory_Relevant yes`, so the jailed prop **is a victory-relevant
  Underworld object** from frame 0, even under a polled check.
- Plot STORY_UNDERWORLD_M01_LAND.XML: `Underworld_A01M01_Begin` (STORY_ELAPSED 0) → TRIGGER children:
  `Underworld_A01M01_Prevent_Victory` = **ENABLE_VICTORY 0** (hold 1, never released in this plot), also
  DISABLE_RETREAT for EMPIRE and UNDERWORLD, HIDE_AUTORESOLVE, USE_RETRY_DIALOG. The mission ends only by
  VICTORY rewards: `Underworld_A01M01_Victory` (Underworld, after the Lua's `Underworld_A01M01_Win_from_LUA`)
  or `Some_Hero_Died_You_Lose` (Empire, after the Tyber / Urai lose messages from Lua) → type 0x10.
- Lua (STORY_UNDERWORLD_ACTI_M01_LAND.LUA) `State_Underworld_A01M01_Begin` OnEnter: finds players, hints, spawns
  an Empire `IMPERIAL_MEDIUM_STORMTROOPER_SQUAD` (invulnerable, no fire), registers prox / death events, starts
  the intro cinematic thread. Tyber is the map object (`Find_First_Object` / `Find_Nearest(pad2,
  "UMP_TYBER_ZANN")`, unhidden and freed later); Urai is spawned later (`Create_Generic_Object("URAI_FEN", …)`).
- So three independent reasons keep the Underworld alive at frame 1: (a) 0xb is only tested when a
  victory-relevant object is lost, and nothing is lost at frame 0 / 1; (b) the Underworld owns a
  victory-relevant object (UMP_TYBER_ZANN); (c) from the first story service the monitor is held, and every
  later 0xb / 0xc test is only queued. [M] for the frame on which the first tactical story service runs.
- **Port:** (1) evaluate 0xb on loss of a victory-relevant object (removal / owner change), not every frame;
  (2) count by the three `*_Victory_Relevant` flags only, with variant inheritance and map owners; (3) implement
  the hold (ENABLE_VICTORY, movie, TSW) with the deferred queue, the VICTORY reward (type 0x10, 7 s / 1 s) and
  the 210-frame end countdown.

**Open items:** writer of 0x140b303c1; the +0x79 end-wait callbacks 0x140b30720 / 0x140b30718; the galactic-only
gate (0x35f6b0 argument); whether a hero death (Tyber) is caught by the plot before any 0xb test of the
Empire — with the hold up it is only queued, so the plot's VICTORY decides [M].

### 3.2 Planet write-back — 0x2bff60(winner) [H]

On the parent GOM, for the head record:

- **Land**, if the winner is the attacker: 0x4b7350(planet data, planet) [L: transfer surface
  ownership]; if a starbase exists (+0x148 > 0) owned by someone else, it is removed (0x4b7450) and the
  starbase level set to 0 (0x3fe750); for planets with a production behaviour, both build queues are
  re-checked (0x403490 ×2). Always 0x4b7d90(planet data, winner).
- **Space**: the surviving station is found among the GOM's objects (behaviour slot +0x38e); its
  level (0x397540) is capped by the planet's maximum starbase level (0x3f8b30), then the type chain is
  walked down (`+0x8b0` next lower type) while level < `Base_Level` (+0x8a0), and the starbase is set
  to that level/type (0x3fe750). A damaged station therefore persists at a lower level; none left →
  level 0. The planet's +0x330 byte takes the attacker id.
- If the winner is the attacker, the land queue (land battle, `Clearing all land based production
  objects from build queue...`) or the orbital queue (space battle) is cleared.

### 3.3 Fleet/ground write-back — 0x28bf10(kind, winner, R, P) [H]

- If GMM+0x1f4 ≠ −1 (a super-weapon result pending [L]): planet-destroyed / weapon-fired story events
  (0x326f20 / 0x326f50) and the special planet update 0x3f6810, then skip the rest.
- **Land** (kind 1): 0x2b5360, 0x2b3e90 (release the conflict) [M]; if R ≠ −1 (and not "local player
  with GOM+0x61c set"), **0x2ba850 retreats R's ground units**: every surface unit of R (planet data
  +0xe0 list) is loaded into a new fleet (0x29e740), logged `%s: Retreating %s from surface.`, the fleet
  enters orbit (0x4338d0 with slot −2) and is ordered to move (MoveObjectToObjectEvent) to the first
  of 0x2a9dd0, 0x2aa400, 0x2a9ee0, 0x2a8dd0 that returns a planet (order 0x2aa400 first when the flag
  is set), else stays. Each member gets signal 0x1b.
- **Space** (kind 2): fresh lists of both sides in orbit (dead removed). For each side's first fleet:
  0x2b4660 releases the units; then if (R = −1 and the side's owner ≠ winner) or (R ≠ −1 and owner =
  R): **0x2ba6c0 retreats that owner's fleets** to the same destination order as land, and if that
  owner also owns the starbase it is destroyed (0x4bda40(…,0) + 0x4b7450).
- Production re-evaluation of P (0x402490 / 0x402660 with the planet's current limits) and
  `InvasionResultClass` (0x2a7580) records the outcome for the story/AI.

The four destination finders (0x2a9dd0, 0x2aa400, 0x2a9ee0, 0x2a8dd0) were not traced; their order
and the "came from" planet in `Lua_Retreat` suggest: previous planet on the fleet's path, nearest owned
planet, nearest allied, nearest any [L].

### 3.4 Retreat inside a tactical battle [H for timers, M for flow]

`RetreatCoordinatorClass` (mode+0x30; space 0x532b70, land 0x534bd0). Requests come from the HUD
`b_retreat`, the Retreat command (COMMANDS.md id 22), Lua `Player.Retreat` (0x603de0; in galactic mode
it sets the auto-resolve retreat instead) and AI task forces (0x5fdd50).

- **Retreat allowed after**: `Space/Land_Retreat_Allowed_Countdown_Seconds` (GameConstants +0x15b0 /
  +0x15b4, vanilla 30) × 30 frames from battle start (vf3). While it runs, requests get reason 2.
- **Request check** (vf9): reason 1 if the player is not attacker/defender of the current conflict or
  the game type forbids it; 2 while not yet allowed; 5 if another player is already retreating;
  otherwise for playable factions a story/ability check (0x282750: `Disallows_Hyperspace_Retreat`,
  `RetreatPreventionAbility`) gives the reason; 0 = allowed.
- **Countdown** (vf14): faction `Space_Retreat_Countdown_Seconds` (+0x20b0) / `Land_...` (+0x2190)
  × 30 frames; plays `Space_Retreat_Begin_SFXEvent` (own) or `..._Enemy_Begin_...`. The retreating
  units list is built by vf8 (`Adding %s to retreatable units list`). Cancel: vf15.
- **States** (+0x18): 1 countdown (units that died or lost their locomotor drop out of the list), 2
  jump-out flight (`Space_Retreat_Flight_Move_Increment` +0x20bc, one unit every
  `Space_Retreat_Unit_Increment_Wait_Frames` +0x20c0 toward `Space_Retreat_Off_Map_Dest_Pos`), 4
  waiting for the retreat cinematic, 3 done.
- **Post-retreat** (vf11): retreated objects and objects inside a retreated flagship are preserved;
  every other object of the retreating player is destroyed; land preserves transports. The victory
  monitor then sees type 0xc (all capable enemy units retreated).
- Damage taken while retreating is scaled by `Space/Land_Retreat_Units_Damaged_Mod_Factor`
  (TACTICAL_COMBAT.md); pursuers use `..._Pursue_Max_Speed_Mod_Factor` (MOVEMENT.md).
- `Space/Land_Retreat_Attrition_Factor` (vanilla 0.0, "per unit probability of destruction during a
  retreat") was not found in this path [L].

### 3.5 Credits, loot, heroes

- No credit reward is paid by the battle code itself. A planet captured through the ownership change
  pays the capture bonus of GALACTIC_ECONOMY §4.2 (step 6). [M]
- Heroes are ordinary units for survival; auto-resolve picks hero-carrying combatants first (§4.4) and
  the result text adds `TEXT_AR_HERO_LOSS` per dead named hero. Overrun is blocked by a hero (§1.6).
- Survivor health write-back from tactical to galactic objects was not located (open item 1).

---

## 4. Auto-resolve

### 4.1 Driver (space 0x2b92e0 states 6–7; land 0x2b76a0 the same) [H]

State 6 → 7: init (`vf1` space 0x60d040 / `vf2` land 0x60cf30), add the defender list then the
attacker list then the planet with `vf3` 0x608080 (Add_Object), raise the pause, then
`vf4` 0x60c9e0(attacker) (Begin). If Begin fails the conflict is dropped.

State 7, when the GOM timer expires: re-arm the timer to `Land_Auto_Resolve_Delay_Seconds` (÷ 3 when a
retreat is flagged; the space code also reads the *land* delay here [H]); if an overrun was found, set
the overrun loser retreating (`vf5`); then call `vf6` 0x60b930(Update, force = GOM+0x634) — in a loop
while force is set, so AI-vs-AI and overrun resolve within the tick. On a result 0x670001 (done) or
0x670002 (done, someone retreated):

1. release both sides' units (0x2b4660), visibility refresh, if 0x670002 retreat the retreating
   player's fleets (0x2ba6c0 with `vf21` = retreating player);
2. release the pause; story events 0x32f570(planet, winner) and 0x32a3b0(planet, loser);
3. every other queued space conflict whose defender was the loser now has the winner as defender;
4. result text (UI only) `TEXT_AR_SPACE_{WIN,LOSS}_{MAJOR,REGULAR,MINOR}` from the recorded
   losses of the local player (or, when not involved, the winner) n_own and of the other side n_other:
   loss MAJOR if n_own > 3·n_other, REGULAR if n_own > n_other, else MINOR; win MAJOR if
   n_other > 3·n_own, REGULAR if n_other > n_own, else MINOR; plus `TEXT_AR_HERO_LOSS` per dead named
   hero. Land uses `TEXT_AR_LAND_*`.

The loop result codes are `vf6` return values; vf7 0x60b570 resets the object afterwards (signals
0x14 land / 0x12 space to every remaining participant).

### 4.2 Combatants (vf3 0x608080) [H]

Every participating object becomes a combatant: `SquadronCombatantClass` (space with a squadron),
`CompanyCombatantClass` (land company), `BaseCombatantClass` (planet/base, built from the planet),
or the object's own `CombatantBehavior` (behaviour id 0x13). Two **sides** (0xb8 bytes each at +0x80
and +0x138; +0x00 player id, +0x04 combatant count, +0x40 strength vector, +0x58 combatant list,
+0x70 weakest combatant, +0x30 "has Super unit", +0x38 "has super-weapon killer"). A side takes the
first player id it sees; a third player id fails with 0x80670001. Combatants that cannot fight in
this kind of battle (combatant vf +0x138 land / +0x140 space) are skipped. Planets add their
garrison/defence objects (planet data +0xe0) and, for space, their starbase type; the hero holder
used for land is found through the planet's companies (+0x1f88 `Land_Bomber_Type` chain) [L].

### 4.3 Strength vectors (vf4 → 0x60b0d0 per side) [H]

A vector of `2 + K` entries {category mask (0 for totals), double value, bool is_land}:
entry 0 = land total, entry 1 = space total, entries 2.. = one per category of the AI contrast table
(K categories from the Lua script `PGAICommands`, `EnemyContrastTypes`/`FriendlyContrastTypes`, loaded
once by 0x5f2b80 into 0x140b396b8).

Unit value **P(t) = `AI_Combat_Power`** (type +0xdf0, float, 0x370e60; in a tactical mode a
`Create_Team` company is scaled by clamp(+0x1e44 / member count, 0, 1)). The representative type of a
combatant (0x60c710) is the object type, or its `Unique_Space_Unit`/`Unique_Ground_Unit`
(+0x1b50/+0x1b60, the higher P of the two).

For each combatant of the side, in list order:

- for not-contained objects, company/squadron members of the current tech level: add count × P(member)
  to the first category entry whose mask intersects the member's `CategoryMask` (+0x1648) and to the
  side's total (space total in space battles, land total otherwise);
- the combatant itself (unless it is a space battle and the type has property 5): add P(t) to its
  category entry and the total; track the weakest combatant (smallest P) in side+0x70;
- land transports (behaviour slot +0x335) add their cargo's P to category and land total.

Then 0x60c3b0 marks side+0x30 (a combatant whose first member type is in category **"Super"**,
0x375270) and side+0x38 (a combatant with a member type flagged `Is_Super_Weapon_Killer`, +0x1f).

Degenerate space case (vf4): no Super unit on either side and both space totals ≤ 0 → the side with
more combatants wins (tie: the side that is not the Begin player wins), the loser is set retreating,
done.

### 4.4 Resolution (vf6 0x60b930) [H]

Timing: let e = (FS frame − start frame) / 30 seconds. Nothing happens until e ≥ 0.6666 ×
`AutoResolveDisplayTime` (GameConstants +0x1594; vanilla 0.01 s → immediate) unless forced or a retreat
is flagged; the result is reported when e ≥ display time or forced. All values below are float32
unless marked double.

1. **Exchange of fire** (0x60d2a0, once per side): R_B = copy of side B's vector; for each combatant of
   side A (and first the side's special object at +0x48 when +0x40 matches A [L]):
   - p = P(t) (and once per company member type, count × P(member));
   - choose target entry j (0x60c200): over category entries with value > 0, with
     c_j = contrast(t vs category j) (0x5f1f90, from the contrast table; × a second factor 0x5f22c0
     when +0x50 ≠ −1 [L]) > 0, maximise (1 − ((R_j − p·c_j) / max(R_j, p·c_j))²) × c_j (double);
     the first maximum wins;
   - apply (0x60ad90): if j < 1 (none found), subtract p from the matching total entry; otherwise
     d = p × c_j × bonus, where bonus = (1 + b) and b = largest per-category bonus (−1 offset) that
     the attacker's planet-dependent modifiers give for the target's category bits (0x374010,
     cached in a 32-float table) [M]; a = min(d, R_j); R_j −= a; the matching total −= a + p_left with
     p_left = (1 − a/d) × p. Each unit fires once (the loop body runs once per unit).
2. **Winner** (0x60c010):
   - if side 0 has a Super unit and side 1 has no Super-weapon killer, side 1 is set retreating;
     symmetric for side 1 (the side facing an unanswerable Super retreats);
   - if a retreat is flagged: the non-retreating side wins;
   - else sum the positive entries (all entries, totals and categories) of each remaining vector,
     S_0 and S_1 (double). If both > 0 and both players are AI: a side whose faction is not
     `Is_Playable` loses outright. Otherwise side 1 wins iff S_0 ≤ S_1, except that an exact tie
     (or both 0) goes to the Begin player (+0xc, the attacker).
3. **Winner record**: +0x14 = winner player id.
4. **Transports in space** (0x60aac0, loser side only): k = number of loser combatants carrying ground
   units (+0x337); allowed survivors m = round((1 − `AutoResolveTransportLosses`) × k) (vanilla 0.333),
   m = 0 when k = 1 and m = 0 when the loser is not playable. Transports carrying a named hero always
   survive (removed from the list); then the first m others survive and the rest are destroyed. If any
   survive, the loser is set retreating.
5. **Attrition**: with (L, W) = (`AutoResolveLoserAttrition`, `AutoResolveWinnerAttrition`) =
   (0.80, 0.75), or (`RetreatAutoResolveLoserAttrition`, `RetreatAutoResolveWinnerAttrition`) =
   (0.65, 0.50) when a retreat is flagged (vanilla values):
   budget_loser = rem + (orig − rem) × (1 − L); budget_winner = rem + (orig − rem) × (1 − W)
   (double, on the battle's total entry: land or space). A non-playable loser's budget is 0.
6. **Survival roll** (0x60a140, per side, winner and loser): if the auto-resolve was started from a
   tactical battle, budget × `Auto_Resolve_Tactical_Multiplier` (vanilla 0.85) first. Then until the
   list is empty:
   - pick: the first combatant whose object contains a named hero or has behaviour 0x5f; if none,
     **index = sync RNG int(0, n − 1)** (0x1ffb40; one draw per pick);
   - Super-category units of the loser die only if the enemy has a Super-weapon killer; otherwise they
     survive without cost;
   - planets/starbases (space): the starbase is downgraded to the highest level whose P ×
     `AutoResolveAttritionAllowanceFactor` ≤ budget (0x3f88b0, walking type names down by one in the
     last character), destroyed if none; budget −= P(new level);
   - other units survive iff budget − P(t) × `AutoResolveAttritionAllowanceFactor` (0.333) > 0, and
     then budget −= P(t) (floored at 0); company members' P × count is charged first for
     non-contained companies;
   - loser units with behaviour 0x5f, and in space loser containers carrying ground units, always die;
   - destroyed units are killed through the combatant (vf +0x120 with damage-source CRC 0x5d5e0b6b)
     and recorded (0x60da50: type, owner) for the result screen;
   - the side's weakest combatant (side+0x70) is killed last: if nothing else of the winner survived,
     the winner keeps it.
   If any loser unit survives, the loser is set retreating.

   **Loss records for carried units** [H]. Both kill paths (0x60aac0 for step 4, 0x60a140 for step 6) call
   0x60da50 with the dying combatant's object *before* killing it. 0x60da50 appends (type, owner) pairs to
   the current battle record (ring of 8 records at +0x1f8 + 0x20·i, i = +0x1f0; list at +0x200 + 0x20·i)
   by the object's behaviours: TRANSPORT (5) → one entry **per contained unit** (contained list at
   object+0xc8, each with its own type and owner) and **no entry for the transport itself**; else
   FLEET (4) → one entry per fleet member (object+0xc0 list); else PLANET (3) → one entry for the planet's
   station type; else one entry for the object. So a transport destroyed with cargo counts its ground
   units, not its hull, and an empty object with TRANSPORT behaviour adds nothing. The result text
   (0x2b92e0, `TEXT_AR_*` ##2 = number of entries whose owner is the local/winning player, n_other the
   rest; `TEXT_AR_HERO_LOSS` per entry whose type has the hero flag type+0x13) reads this list [H]; the
   battle-end dialog's loss grids/values read the same record [M]. Port: when a combatant with cargo dies,
   add its cargo count (not 1) to `attacker_losses`/`defender_losses`.
7. The local AI learning system is updated for AI players (0x58a9a0).
8. vf6 returns 0x670001 (done) or 0x670002 (done with a retreating player, `vf21`).

### 4.5 When auto-resolve is used [H]

AI vs AI (always, immediately), overrun (when enabled), the player's choice in the pending dialog,
MP option 0, the tactical HUD button `IDC_AUTORESOLVE` (0xdc5d0: allowed only when no retreat is in
progress; runs vf1/vf2 → vf8 0x609270 (gathers combatants from the current tactical mode and its
persistent lists, sets +0x3e = "from tactical") → vf4 → vf6(force)), and Lua/story triggers. Story can
forbid it per planet (`DISABLE_AUTORESOLVE`, `RESTRICT_AUTORESOLVE`, `HIDE_AUTORESOLVE`,
`Prevents_Story_Campaign_Autoresolve` on types, campaign `Is_Autoresolve_Allowed` /
`Autoresolve_Exclusion_Locations`) [M for the field wiring].

### 4.6 Retreat from the pending dialog / Lua [H]

`vf5` 0x60cf00(player) = Set_Retreating: valid only after Begin and once; sets +0x0a and +0x10. It is
called by the dialog's retreat choice, Lua `Player.Retreat` in galactic mode, `WithdrawlEventClass`
(0x4adf40) and AI task forces. `vf9` 0x60cdb0 applies an early withdrawal: the retreating side's units
are removed (killed through the combatant and recorded) only when the retreating player is exactly one
of the two sides [M].

---

## 5. Mode stack, timing and determinism

1. **Choice phase**: the galactic mode keeps ticking (global frame +1 per tick, GOM service runs,
   state machine advances), but the pause counter freezes its mode frame and object behaviours
   (MAIN_LOOP §1.5). Income, production and movement do not advance. [H]
2. **Auto-resolve**: runs inside the galactic GOM service, single-threaded, in tick order. Its only
   randomness is the sync RNG `int(0, n−1)` per survival pick (§4.4 step 6). Strength math is float32
   with double accumulators — a port must keep the same precision per step and the same order (lists
   are in insertion order: defender units, attacker units, planet; within a side, list order). [H]
3. **Tactical battle**: Start_Game pushes the land/space mode; the galactic mode is suspended (not
   ticked, no galactic frame). The global frame and the sync RNG stream continue (no reseed). The
   battle's own mode frame starts at 0. Battle wall time is tracked separately (GMM+0x1e8). [H]
4. **Return**: the tactical common tick detects the end, pops the mode within the same tick, applies the
   results to the parent GOM (§3), then may immediately start the next queued battle. The galactic mode
   resumes on the next tick (`resume` vtable +0x58). [H]
5. **CRC**: every step above is sim state and is part of the frame CRC (MAIN_LOOP T13). MP choice is a
   vote resolved through scheduled events, so the switch frame is identical on all peers; the vote
   timeout uses wall time and is the one non-deterministic input (it only decides *which* command is
   sent). [M]
6. **Timer quirk**: the GOM+0x30/+0x28 delay timer counts the *current mode's* frame (0x4ad350),
   which is frozen by the pause counter that the same code raises. With the vanilla 0.01 s delays the
   duration is 0 frames and the timer expires at once; with a non-zero delay this looks like it would
   never expire. Unresolved (open item 4). [H] for the code, [L] for the consequence.

---

## 6. Function index

| RVA | role |
|---|---|
| 0x2af7c0 | `GameObjectManagerClass::Initiate_Space_Conflict` (callers 0x56a1e0 fleet arrival, 0x28f590, 0x4626a0 story, 0x576db0/0x6cd440 Lua `Force_Test_Space_Conflict`, 0x5af260 FleetManagementEvent, 0x6aebf0) |
| 0x2b0520 | `GameObjectManagerClass::Land_Invade` |
| 0x4338d0 | fleet enters orbit, returns "conflict" |
| 0x2ad100 / 0x2ad0e0 / 0x2ac5a0 | split head participants / space head / land head |
| 0x2be640 | galactic GOM service tail (conflict processing order) |
| 0x2b92e0 / 0x2b76a0 | space / land conflict state machines |
| 0x2befe0 / 0x2becb0 | space / land conflict validation |
| 0x2ab880, 0x395d10 | side strength for the overrun test |
| 0x2bce30, 0x2bd500 | force lists (start vs reinforcement), space population split |
| 0x28fda0 | start tactical battle (map choice, load, victory conditions) |
| 0x28f590 | start the linked-battle record (§2.9.3) |
| 0x45f9c0 / 0x28bd70 / 0x28c640 | LINK_TACTICAL handler / write the record / reset the record |
| 0x2a7580 | conflict completion at battle end (land units, resolution, remove head) |
| 0x2aa510 / 0x2aa8e0 | linked-battle resolution land / space (RETREAT_PLAYER, RETREAT_AI, DESTROY_AI) |
| 0x3639d0 (tail) | battle end detection |
| 0x28f520 → 0x2bff60 | planet write-back (starbase level, queues) |
| 0x28bf10 | apply battle result (retreats, production) |
| 0x2ba6c0 / 0x2ba850 | retreat fleets / retreat ground units |
| 0x2a6710 / 0x2a6840 | wipe a non-playable loser |
| 0x490de0 | AutoResolveClass ctor; vtable 0x8755b8 |
| 0x60d040 / 0x60cf30 / 0x608080 / 0x60c9e0 / 0x60cf00 / 0x60b930 / 0x60b570 / 0x609270 | AutoResolve vf1 init space, vf2 init land, vf3 add object, vf4 begin, vf5 set retreating, vf6 update/resolve, vf7 reset, vf8 gather from tactical |
| 0x60b0d0, 0x60d2a0, 0x60c200, 0x60ad90, 0x60c010, 0x60aac0, 0x60a140, 0x60c710, 0x60c3b0 | strength vector, exchange of fire, target choice, apply, winner, transports, survival roll, representative type, Super flags |
| 0x370e60 | unit value (`AI_Combat_Power`) |
| 0x5f1f90, 0x5f2b80 | AI contrast lookup, contrast table load (`PGAICommands`) |
| 0x532b70.. / 0x534bd0.. | Space/Land retreat coordinators (vf3 allowed timer, vf9 check, vf14 begin, vf5 update, vf11 post-retreat) |
| 0x341ff0 | VictoryType enum names |

---

## 7. Open items

1. **Health/shield write-back** of survivors from tactical objects to their galactic counterparts, and
   where tactical-only deaths remove galactic objects (only the non-playable wipe was found). Q2 probe:
   fight a space battle, damage a capital ship, log its galactic health before and after.
2. ~~Land force split 0x2bc6a0 and the land reinforcement-point flow~~ — resolved [H]: no cap at land start
   (defender surface all start; attacker = lead unit or raid force, other transports in reserve); land / space
   drop validity rules. TACTICAL_GAPS.md §4.2.
3. ~~Whether the battle load consumes the sync RNG~~ — yes [H draws, M path]: object construction (SAP init,
   hardpoints, behaviour Initialise) draws in creation order. TACTICAL_GAPS §4.3, §6.4.
4. The delay-timer/pause interaction of §5.6 (does any path release the pause before state 7, or does
   the galactic paused query ignore it in this state?). Runtime check: set
   `Land_Auto_Resolve_Delay_Seconds` to 2.0 and auto-resolve.
5. ~~Retreat destination finders~~ — resolved [H]: nearest planet by orbit control / base ownership / neutral /
   any, with the call orders. TACTICAL_GAPS.md §4.5.
6. ~~Behaviour 0x5f and bit 0x2c~~ — resolved [H]: 0x5f = DUMMY_GROUND_STRUCTURE; bit 0x2c in the space bitset =
   TACTICAL_SUPER_WEAPON (so §2.3 rule 1 = "a super-weapon always starts"). TACTICAL_GAPS §4.6.
7. ~~Second contrast factor / planet bonus~~ — resolved [H]: the factor is never applied (+0x50 always −1); 0x374010
   is a per-type bonus from `Abilities` (`Unit_Strength_Category/Factor`), empty in TR. TACTICAL_GAPS §4.7.
8. ~~Battle-end step 4 / GMM+0x1f4~~ — resolved [H]: +0x28 = spawned super-weapon (records 3 / 6);
   GMM+0x1f4 is the corruption mission, which skips the normal write-back. TACTICAL_GAPS §4.8.
9. ~~Retreat attrition consumers~~ — resolved [H]: RetreatCoordinator vf12 0x3404c0, one synced draw per
   retreating unit. TACTICAL_GAPS §4.9.
10. ~~Map-name sources~~ — resolved [H]: `Destroyed_Space_Tactical_Map` unread; random picker 0x47bf40 uses the
    **free** RNG (not deterministic). TACTICAL_GAPS §4.10.
11. ~~Pending-dialog command path~~ — resolved [H MP, M SP]: ResumeGame (51) after vote 2; SP button calls
    0x28c7e0 directly; Withdrawl (49) = retreat. TACTICAL_GAPS §4.11.
13. ~~Base layouts, §2.4 starting positions, map ownership, TEAM objects~~ — resolved [H]: REMAINING_GAPS §3. The §1.4
    step 2 story flag is the GMM+0xF0 linked-battle record (REMAINING_GAPS §6.4). Still a probe: map owner ids vs
    campaign player ids (REMAINING_GAPS §3.3).
14. Story-linked battles (LINK_TACTICAL): open items and probes LT1 / LT2 in §2.9.9.
12. Runtime confirmation of the auto-resolve formula: Q2 probe with two fixed fleets, a logged RNG
    seed (`RANDOMLOGFILE=`) and the per-unit outcome.

---

## 8. Key rules (short report)

- Space conflicts are queued on fleet arrival against enemy fleets/starbases; land conflicts only by
  invade orders. FIFO queues on the galactic GOM; one space head and one land head per tick, space
  first; heads re-validated each pass.
- While deciding, the galaxy is paused (mode frame frozen, global frame and RNG continue). AI vs AI
  and overruns auto-resolve immediately; humans get fight/auto-resolve/retreat; MP uses a vote.
- Map: planet `Space/Land_Tactical_Map`, else a random map by terrain/defender faction/base level that
  is then stored on the planet; story and record overrides win.
- Starting force: space population cap = faction `Space_Tactical_Unit_Cap`, proportional per type by
  `Population_Value`; the rest is the reinforcement pool (two per-player id maps on the galactic GOM).
- Victory: all enemy units destroyed or all capable enemies retreated, plus optional base/station
  destruction rules. Losers retreat to a neighbouring planet; non-playable losers are wiped; starbases
  are downgraded or destroyed; captures clear the planet's queues.
- Auto-resolve: AI_Combat_Power per category, contrast-weighted fire, winner by remaining strength,
  per-unit survival against an attrition budget (winner 0.75 / loser 0.80, 0.50/0.65 with retreat,
  allowance 0.333), unit order by sync RNG, heroes first.

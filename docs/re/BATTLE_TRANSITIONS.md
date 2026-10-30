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
| 9 | battle set up from a queued record (§2.7) |

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

0x28f590 starts a battle from a stored record (story `STORY_LOAD_TACTICAL_MAP`, save/load during a
pending battle, MP): kind (+0xf4: 1 land, 2 space), attacker (+0xf8), planet (+0xfc), map (+0x100),
faction name (+0x120), forced type (+0x140), strings (+0x148), "set up" flag (+0x168), game options
(+0x16c..+0x174, copied to GOM+0x61c). It re-queues the conflict at the front (`at_front` = 1), then
either jumps to state 9 and builds the force split immediately (0x2bce30 / 0x2bc6a0), or lets the state
machine run. It is also called after every battle end and from the galactic resume path (0x28bc80) to
start the next pending battle.

### 2.8 RNG and story hand-off

- The sync RNG is **not reseeded** when the tactical mode is pushed (MAIN_LOOP §4.1). [H]
- The battle start happens inside the galactic GOM service (state 3) or a callback from a GUI command;
  the map load then runs on the load thread while no tick runs (FS+0x34). Whether the loader draws from
  the sync RNG is unknown (open item 3). [L]
- If the story allows tactical story mode (0x140b30544 and the local player's flag +0x3c0 clear), the
  new mode's +0x2cf is set and the story manager is told the defender (0x329530); the tactical story
  scripts then run on the tactical mode's +0xa0 (T9). [M]

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
| 0x28f590 | start a queued/pending battle |
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

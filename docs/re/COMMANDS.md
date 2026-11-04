# Commands, scheduling, sync checks and sim RNG (StarWarsG.exe FoC x64 v1.121) — spec for G0 part 3

Written 2026-10-27 by an RE agent for Phase 4 step G0-3 ("command/event pipeline", `engine/commands`).
Clean-room: behaviour, constants and orders in prose and tables only. No code or pseudo-code from the
decompilation. Addresses are RVAs in StarWarsG.exe. Names come from RTTI and `docs/re/ghidra/`; where a
function has no useful name it is `FUN_<rva>`, and the address is authoritative.

Confidence: **[H]** read in the code, **[M]** read in the code but the role is inferred,
**[L]** inferred from surroundings, strings or naming.

Companion documents (not repeated here): `MAIN_LOOP.md` (loop order, pacing, pause, game speed,
RNG algorithm and seeding, §1.1 steps 8–10, §1.2 T2/T13, §4), `OBJECT_RUNTIME.md` (object ids,
0x3FFFFF = none, per-object CRC, store update order), `MOVEMENT.md`, `TACTICAL_COMBAT.md`,
`GALACTIC_ECONOMY.md`, `OBJECT_TYPES.md`, `LUA_API.md`, `SUBSYSTEMS.md` (networking module list).

Globals used below:

| global | what |
|---|---|
| 0x140b0a320 | FrameSynchronizer (FS): +0x00 global frame, +0x08 MaxAhead (frames), +0x0c command-turn length (frames), +0x10 latency unit, +0x18 pacing rate, +0x20 sim rate (30), +0x35 out-of-sync flag, +0x28 frame at which desync was detected, +0x54.. own 256-entry frame ring, +0xc68 per-player remote frame rings |
| 0x140b2ed18 | OutgoingEventQueueClass (locally issued commands not yet flushed) |
| 0x140b15690 | ScheduledEventQueueClass (derived from EventQueueClass): the one queue every command passes through before execution; also does replay recording/playback |
| 0x140a16fd0 | PlayerList; local player id via 0x294a70, local player via 0x294a40, player by index 0x294bc0 |
| 0x140b153e0 | GameModeManager (GMM); "is multiplayer" = game type (GMM +0x18) in {1, 2, 4} (0x28af60) [H] |
| 0x140b153f8 | network session type: 0 none, 1/2/4 networked, 3 local (treated like 0) [H for the branches, L for the names] |
| 0x140a13e24 / 0x140a13e20 | synchronized / free RNG state (MAIN_LOOP §4.1) |

---

## 0. Rules for the implementer (summary)

1. **One path for player orders.** Every order the local human gives (UI, hotkeys, mouse) is an *event
   object*: built, stamped with the local player id, added to the outgoing queue, flushed once per loop
   iteration into the scheduled queue, and executed by the scheduled queue. Single-player and multiplayer
   use the same objects and the same executor; only the stamping and the gate differ. [H]
2. **AI and Lua do not use the queue.** AI tasks and Lua commands construct the same event classes on the
   stack and call their Execute method directly, inside the tick that runs them. They are never sent
   over the network: AI and scripts run identically on every machine. [H] (§5)
3. **Single-player delay = "before the next tick".** In SP the flush stamps each command with the current
   frame and the executor runs it in the same loop iteration, before the next tick, even while paused.
   Equivalent to issue tick N → applied at the start of tick N+1. [H]
4. **Multiplayer delay is fixed per session type, turn-aligned, and adaptive only on internet games.**
   Execute frame = round **down** to a multiple of the turn length T of (current frame + 1 + MaxAhead +
   max(T−1, 1)). LAN-type sessions use T = 1, MaxAhead = 2 (commands land 4 frames later). Internet
   sessions use T = 5, MaxAhead = 10 (or T = 2, MaxAhead = 4), so commands land 11–15 frames later and
   only on frames that are multiples of T. The host may retune MaxAhead with a scheduled FrameSync event. [H]
5. **Execution point: start of the tick**, right after the frame counter increments and before
   GameModeManager::Update (MAIN_LOOP T2), so a command executed at frame F affects tick F. [H]
6. **Order within one frame: by player list index, then by arrival.** The executor walks the players in
   PlayerList order. For each player it walks the single queue in insertion order and runs that player's
   due events. There is no sequence number on the wire. Per-sender order is the order of the reliable
   connection, and completeness is guaranteed by the cumulative command counts in FrameInfo (§4.4). [H]
7. **Lockstep gate.** A peer does not advance past a frame until every human peer has reported frame info
   within MaxAhead and it has received as many scheduled commands from each peer as that peer declared.
   Commands therefore can never arrive late. [H]
8. **Selection is simulation state.** Select, SelectAll and ControlGroup are scheduled, synced events
   that set a per-player selection set inside the GameObjectManager. Many orders (stop, move to position,
   escort, cinematic, and move to object with an empty list) apply to "the issuing player's current
   selection", resolved at execution time. [H]
9. **Floats on the wire are quantized, and Execute re-quantizes locally.** Positions and angles are sent as
   fixed-range integers (§3.2). The local copy of an event is never serialized, so Execute itself rounds
   its floats through the same quantizer. All machines then use bit-identical values. [H]
10. **Validation is lazy and silent.** Execute resolves ids at execution time. Unknown, dead or wrong-mode
    targets make the event (or that element) a no-op, and nothing is reported back. A few events check
    ownership (§6.2). [H]
11. **Sync check:** after every tick a 32-bit frame CRC is computed. Its low **16 bits** are sent every T
    frames in a FrameInfo event and compared with the local ring entry for the same frame. On a mismatch
    the simulation stops (GMM::Update no longer runs), "TEXT_OUT_OF_SYNC" is shown, and sync logs are
    written. [H] (§7)
12. **RNG:** one synchronized LCG for all sim randomness, one free LCG for presentation (`MAIN_LOOP.md` §4.1,
    which is authoritative). Commands themselves consume no randomness when issued. Their Execute can
    (e.g. weighted picks), and it does so at T2 in the executor order above. [H]

---

## 1. The event object model

### 1.1 Classes [H]

- `EventClass` (vtable 0x87bd58, 9 slots) derives from a linked-list entry and a ref count. Fields:
  +0x08 ref count, +0x10/+0x18 list links, **+0x20 event type** (initial −1), **+0x24 issuing player id**
  (the constructor 0x5126c0 sets it to the *local* player id, and AI/Lua callers overwrite it).
- `ScheduledEventClass` (vtable 0x87c750, 10 slots; constructor 0x512c50) adds **+0x28 execute frame**
  (initial −1). It also holds an embedded int vector that the event subclasses do not use.
- 56 concrete event classes, 4 non-scheduled (derived from `EventClass` directly) and 52 scheduled.

> **Status 2026-10-30:** The port's catalogue has 54 classes (4 non-scheduled and 50 scheduled); ids 23, 24, 27, 41 and 44 are dead (names only), and the two extra classes have no wire type id (`engine/commands/include/alamo/commands/event_types.hpp:3-8`).

- Virtual interface (slot = byte offset / 8) [H for 0,1,2,4,5,6,7 from the call sites; M/L for 8,9]:

| slot | role | evidence |
|---|---|---|
| 0 / 1 | add-ref / release | queue insertion and removal |
| 2 | deleting destructor | outgoing-queue duplicate discard |
| 4 | **Execute** | executor 0x2c0c70, receive 0x2c2910, AI/Lua callers |
| 5 | **Pack** (write to bit stream) | sender 0x3b03e0, recorder 0x2c20d0 |
| 6 | **Unpack** (read from bit stream) | receiver 0x2c2910, replay loader 0x2c1270 |
| 7 | **Is_Scheduled** | false for the four `EventClass`-derived types, true otherwise |
| 8 | "flush the packet now" flag for non-scheduled events [L] | sender sets a send-immediately flag when true |
| 9 | duplicate-collapsing flag for scheduled events [L] | used only by the duplicate filter (§4.2) |

- Factories: `EventFactoryClass<T, id>` (54 instantiations, registered into one global vector at startup
  by 0x512930; lookup by id 0x512b50). The type name table at 0x87bdb0 (`EVENT_*` strings, 0x512c10) gives
  the enum. [H]

### 1.2 Wire header [H]

Each event is written as one bit-packed record, low bit first (§3.1):

| field | bits | notes |
|---|---|---|
| type | 6 | width computed at first use from the highest type id, 0x3A |
| player id | P | P = the 32-bit value at 0x140a16fb4. It is static data that no code writes, so the value cannot be read from the decompile [L]. Player ids index arrays sized "max players" (0x140a16fb0). |
| execute frame | 26 | scheduled events only |
| payload | per type | §2 |

---

## 2. The complete event list

`S` = scheduled (synced, executed at a frame), `N` = non-scheduled (executed on receipt, never part of the
sim). Types 23, 24, 27, 41 and 44 exist only as names (`EVENT_DEPLOYMENT`, `EVENT_SPECIAL_WEAPON_TARGET_MODE`,
`EVENT_KILL`, `EVENT_ACTIVATE_BEHAVIOR`, `EVENT_SPAWN_OBJECT`). No class or factory exists for them in FoC,
so they are dead ids. [H]

Payload notation: `id32` = 32-bit object id (0x3FFFFF = none, see OBJECT_RUNTIME), `P` = player id
(P bits), `qN` = float quantized with table entry N (§3.2), `list7`/`list8`/`list32` = element count in
7/8/32 bits followed by that many id32. "Sel" = the issuer's current selection. Pack address in
parentheses where it was read directly. Otherwise the payload comes from Execute's field reads [M].

### 2.1 Unit orders (tactical, and galactic where noted)

| id | class | S/N | payload (wire order) | effect | conf |
|---|---|---|---|---|---|
| 1 | MoveToPosition | S | q1 x, q2 y, q3 z, 8-bit flags, 1-bit (0x3ad050) | moves Sel to the point (group move). Flag bit 3 selects an alternative group-move routine. The 1-bit field swaps selected `Uses_Multiple_Locomotors` units to their secondary locomotor first (0x3a9910; no FoC sender sets it, MOVEMENT §3.1.1). Also notifies the tutorial/story "unit moved" hook for local-player units | H payload, L flag meanings |
| 2 | MoveToObject | S | id32 target, list32 movers (0x3aefc0) | movers move to/onto the target. An empty list means Sel | H |
| 3 | MoveObjectToObject | S | id32 object, id32 target | single explicit object to target: galactic fleet hop (free-store path step 0x71f710), AI/Lua moves | M |
| 10 | MoveThroughObjects | S | list8 waypoint ids, id32 (0x3c4390) | galactic multi-hop fleet route [M]. Execute checks a per-player galactic flag first | H payload |
| 14 | MoveToRay | S | q1 q2 q3 point A, q1 q2 q3 point B, 8-bit flags (0x3ae5f0) | drag-line formation move [L] | H payload |
| 19 | MoveToRayFacing | S | point A, point B, q6 angle, 8-bit flags (0x3aeac0) | same with final facing | H payload |
| 47 | MoveToPositionFacing | S | q1–q3 point, q6 angle, 8-bit flags, list32 (0x3adf70) | move with final facing | H payload |
| 55 | MoveToGarrison | S | q1–q3 point, 8-bit flags, three id32 (0x3ad550) | move into a garrison. Also built by Lua `Garrison` and by GarrisonUnitBehavior | H payload |
| 6 | Attack | S | id32 target, 32-bit second ref, three 1-bit flags, list32 attackers (0x3af670) | attack order. Also built and executed directly by the ConcentrateFire ability (0x6f5550) and AI (0x5920b0) | H payload, L flags |
| 20 | Facing | S | q6 angle, list32 (0x409c60) | face direction | H |
| 21 | Escort | S | id32 target, 32-bit option | Sel escorts/guards the target. This is the "guard" order [M] | M |
| 29 | StopMovement | S | none | stop Sel | H |
| 52 | Garrison | S | id32 unit, id32 garrison (0x3FFFFF = ungarrison) | enter/leave a garrison | M |
| 26 | CinematicAnimation | S | none | plays a fixed animation on every live Sel object | H |
| 31 | SetupPhaseMove | S | id32, q1–q3, q6 (0x5aebb0) | skirmish deployment-phase placement. Not packed when the id is −1 | H |
| 32 | SetupPhaseTriggerEnd | S | none | marks the player ready (player +0x184). The setup phase ends when all active players are ready | H |

### 2.2 Abilities, weapons, build/produce

| id | class | S/N | payload | effect | conf |
|---|---|---|---|---|---|
| 40 | TacticalSpecialAbility | S | list7 users, id32 target, id32, 7-bit ability type, id32 (0x429700) | targeted ability. Type must be 1..76, the target must exist and not be dying (+0x3a0 bit 2), and per user the ability must be available and the target valid | H |
| 57 | TacticalSpecialAbilityWithDummyTarget | S | as 40, then q1–q3 point, P (0x429800) | ability on a ground point (a dummy target object is used) | H |
| 43 | SetUnitAbilityMode | S | 7-bit count, 7-bit ability type, 1-bit on/off, 1-bit play-response, 1-bit, id32, q1–q3, then count × id32 (0x4300f0) | toggles an ability mode on the listed units. Also the instant area abilities from the command bar (heal, stun, whirlwind, convert), and Lua `Activate_Ability`/`Cancel_Ability` (executed directly) | H |
| 58 | SetAbilityAutofire | S | list7, 7-bit ability type, 1-bit (0x4b3790) | autofire toggle per unit | H |
| 25 | SpecialWeaponFire | S | id32 weapon, P, 1-bit "at position", then q1–q3 or id32 target (0x40a2d0) | fire a special weapon | H |
| 38 | TacticalSuperWeapon | S | id32 weapon, 2-bit op (0x5d6c60) | op 1 / 2 on the superweapon behaviour. Only if the object's owner is the issuer | H |
| 28 | BombingRun | S | q1–q3 point (pack 0x524650) | bombing run on a point (BombingRunManager, mode +0x38) | H |
| 54 | PlanetaryBombard | S | q1–q3 point | orbital bombard (PlanetaryBombardManager, mode +0x40) | M |
| 12 | Reinforce | S | id32 unit (or type ref, see note), q1–q3 drop point (0x403c10) | reinforcement drop in land (mode type 1) or space (2). The validity check runs inside Execute | H |
| 33 | TacticalBuild | S | id32 build pad, 16-bit type index, P (0x5d7820) | build on a pad | H |
| 34 | TacticalSell | S | id32 (0x6898c0) | sell a structure | H |
| 45 | RepairHardpoint | S | id32 object, 6-bit hardpoint index, P (0x430830) | repair one hardpoint | H |
| 48 | PlaceBeacon | S | 16-bit beacon type, q1–q3, q9 q10 q11 angles, P (0x436450) | map beacon/ping | H |
| 7 | Production | S | 16-bit type index, id32 location, 8-bit, 1-bit cancel, 4-bit queue, 4-bit index (0x524070) | queue a build. With cancel = 1 it removes entry *index* of queue *queue*. Fails with a log line when the player has no build queue. AI `ProduceObject` and the AI production task build and execute it directly | H |
| 13 | SpecialAbility | S | 3-bit kind 1..7, then a per-kind layout of id32s, 16-bit type indices, q5/q8 values, flags (0x4b4990) | galactic hero/unit abilities (kind-specific) | H layout, M meanings |

Note on Reinforce: Execute resolves +0x48 either as a reinforcement-pool object (0x28aa50) or as a
type (0x407db0), depending on a GMM flag (0x28afb0) [M].

### 2.3 Galactic

| id | class | S/N | payload | effect | conf |
|---|---|---|---|---|---|
| 8 | FleetManagement | S | id32 planet/fleet, id32 fleet (0x3FFFFF = new), id32 unit, 4-bit op, 6-bit slot, id32, id32 (0x5af190) | fleet create/merge/split/transfer/land/launch (ops 0–8). Op 8 is an object-only special case. Used heavily by AI (AIBuildTask, GalacticTaskForce) and Lua (`Reinforce`, `Lua_Move_Object`, `Land_Units`, `Launch_Units`, `AssembleFleet`), always executed directly. Ignored in galactic mode while a battle is pending (counter 0x140b1540c > 0). A second SP-only check skips it while another pending-conflict condition holds | H payload, M ops |
| 9 | Invade | S | id32 planet, list7 units, 1-bit (0x48fe70) | invade/land on a planet. Also AI, FleetLocomotor and Lua `Invade` (direct) | H |
| 22 | Retreat | S | 1 bit | 1 = request retreat for the issuer via the retreat coordinator, 0 = cancel when allowed | M |
| 46 | GalacticSell | S | id32 (0x532740) | sell a galactic structure. Not packed when the id is 0x3FFFFF | H |
| 53 | SetMarkerID | S | id32, 32-bit marker (unpack 0x5b05d0) | sets object +0x3b0, galactic only | H |
| 56 | SetGUIIndex | S | id32, 12-bit index (0x5b1230) | galactic per-object GUI slot. Written into sim state, so synced | H |
| 42 | DistributeMoney | S | none | splits min(issuer credits, cap 0x140b26fec) evenly over the issuer's allies (MP) | H |

### 2.4 Selection

| id | class | S/N | payload | effect | conf |
|---|---|---|---|---|---|
| 5 | Select | S | list7 ids, max 127 (0x3acb30) | clear the issuer's selection, then add each id that is allowed. Ignored in galactic mode with a battle pending | H |
| 37 | SelectAll | S | none | replace the issuer's selection with every owned SELECTABLE object that is not a base structure, star base, orbital structure or mine, has no parent and is not retreating (§2.4.1) | H |
| 30 | ControlGroup | S | 4-bit group 0..9, 1-bit assign, [1-bit add, only when assign = 1] (0x436160) | recall: select the group (a camera jump on a double recall within 1 s is local-player only). Assign: store Sel in the group | H |

#### 2.4.1 SelectAll in detail (q6 RE, 2026-11-04) [H unless tagged]

**Execute (SelectAllEventClass vf4 0x437FA0; vtable 0x86B020, factory id 37, no payload).**
1. Clear the issuer's selection (0x2A6370: the per-player selection list GOM +0x1C0 + player × 0x48; each entry is
   told it was deselected and released). SelectAll always **replaces**; there is no add form.
2. Query 0x2A9FF0(GOM, behaviour 2, owner = issuer, no excluded owner, no radius, no cap). The "2" is the behaviour id
   **SELECTABLE**, not a category: because an owner is given, the walk is the **issuer's per-owner object list**
   (GOM +0xC8 + owner × 0x48; the SELECTABLE list GOM +0x2A0 is used only when no owner is given), keeping objects whose
   SELECTABLE slot (obj +0x334) is present. No CategoryMask is read.
3. Per object, in that list's order, skip it when any holds:
   - a retreat is running and the object is in the retreat coordinator's list (mode +0x30, 0x3400C0; tested twice);
   - it has DUMMY_GROUND_STRUCTURE (0x5F, slot +0x391), DUMMY_STAR_BASE (0x5C, +0x38E) or DUMMY_ORBITAL_STRUCTURE (0x60,
     +0x392);
   - it has a parent object (obj +0x2B0 ≠ 0): a TEAM member, a garrisoned unit (garrison entry sets +0x2B0 := host,
     L1_S4_REMAINING §3.5), a transported unit [M for transports];
   - it has PROXIMITY_MINE (0x55, slot +0x387).
4. Each survivor → 0x2BD2F0(GOM, id, issuer) → insert 0x2BD360, the same insert as `Select`:
   - already selected → nothing;
   - a TEAM member is replaced by its container (obj +0x2B0 with TEAM), an attached object by its parent record
     (obj +0x150: +0x10, else +8) [M: name];
   - the object enters the issuer's selection only if its SelectionDataPack flag is set (obj +0xB0 → +0x38, 0x39B070):
     default 1 (pack ctor 0x55BE30), cleared by `Set_Selectable(false)` (0x580530, which also deselects the object and
     its TEAM members) and while a landing transport carries it (0x644330 / 0x6438A0);
   - a TEAM container also inserts every member that passes the same flag test;
   - then a per-mode selection notification 0x32F1B0 (on a sub-object of the current mode) [M: purpose].
5. If the issuer is the local player: the HUD refresh (mode vf +0x238 → 0x5392D0) and the selection-changed
   singleton 0x599070 (presentation).

**Not tested by the execute:** `Not_Really_Selectable` (type +0xE7; read only by the UI pick helper 0x29C040 and the
cursor resolver 0x2AD280; FoC sets it only on `Proximity_Mine`, excluded anyway, and on `Eclipse_Super_Star_Destroyer`,
which Select All therefore **does** select when the player owns it), hidden (`Hide`, obj +0x3A2 bit 0, model hidden), dying (+0x3A0), under construction,
locomotor, hero / structure / CategoryMask, land vs space mode, fog, stealth, cap. The list has no 127 cap (that is the
`Select` wire format); insertion order = the issuer's per-owner list order (object add order [M]).

**What that means for FoC data** (checked against `Data\XML`, mode-merged `Behavior` + `Land/SpaceBehavior`):
- **Excluded:** every base structure with DUMMY_GROUND_STRUCTURE (barracks, factories, command centres, power
  generators, base shields, turbolaser towers, mining facilities, the Hypervelocity Gun / Ion Cannon …), every star
  base and asteroid base (DUMMY_STAR_BASE), defense satellites and orbital structures (DUMMY_ORBITAL_STRUCTURE),
  proximity mines and remote bombs. Same rule in land and space.
- **Included:** every owned object with SELECTABLE and no parent, **heroes included**; TEAM containers (then expanded to
  their members); space fighter squadron containers; and **build-pad structures without a dummy behaviour**: the
  buildable turrets (`*_ANTI_INFANTRY_TURRET`, `*_ANTI_VEHICLE_TURRET`, `*_ANTI_AIRCRAFT_TURRET`, mass driver /
  torpedo / rapid-fire turrets, grenade mortars, EM field generators), bacta tanks, repair facilities, sensor nodes,
  the mineral processors and the selectable `UC_*` under-construction objects (`UC_*_Mineral_Processor`, space
  `UC_*_Defense_Satellite_*` [they also carry no orbital dummy]) [H for the code path; M for how it looks in game:
  not probed].
- **Never:** hardpoints (not GOM objects), units still in the reinforcement pool, fighters still in a hangar (not
  spawned yet [M]).

**UI path (Ctrl+A, key slot 10, UI.md §key bindings; handler 0x3AAD0).**
- The handler builds a local list from the same query and filters **plus** the selection flag (0x39B070) **and a
  locomotor** (slot +0x333 = behaviour id 1); objects with a parent are skipped as in the execute (its container
  redirect is dead code there); retreating objects are skipped again. No screen / visibility / fog filter, no modifier key test.
- If that list is empty nothing is sent. Otherwise it queues one SelectAll event (no payload) and plays the selection
  acknowledgement of a unit from its local list (0x44280 → 0x38290 pick, 0x3739A0 type SFX, 0x2D5320). So 0x44280
  is only the sound; the selection itself is the execute's broader set (so turrets are selected together with the
  mobile units, but Ctrl+A with only turrets owned does nothing).

**`Select` (id 5, vf4 0x3ACBF0) for comparison:** galactic mode with a battle pending and a human issuer → ignored;
clear the issuer's selection; insert each payload id (0x2BD2F0) unless it is retreating. No owner test in the execute:
ownership and `Not_Really_Selectable` are checked by the UI before it sends (0x29C040: selection flag, not
`Not_Really_Selectable`, owner = player or skirmish `Is_Community_Property` allied).

**Port notes** (`sim_host.cpp` SelectAll):
1. Query = the issuer's own objects with the SELECTABLE behaviour (not a category mask).
2. Exclude DUMMY_GROUND_STRUCTURE / DUMMY_STAR_BASE / DUMMY_ORBITAL_STRUCTURE / PROXIMITY_MINE holders, objects with a
   parent (TEAM members, garrisoned, transported), and objects in the running retreat. Do not exclude by
   `Not_Really_Selectable`, hidden, dying, hero, or the structure category: build-pad turrets, bacta tanks, repair facilities,
   sensor nodes and mineral processors stay in.
3. Insert like Select: skip already selected, TEAM member → container + its members, only objects whose selectable
   flag (`Set_Selectable`) is set.
4. Replace the selection, no cap, issuer's object-list order.
5. The UI sends the event only if at least one owned object passes the same filters and has a locomotor; Shift does
   not matter.

### 2.5 Session, chat, control

| id | class | S/N | payload | effect | conf |
|---|---|---|---|---|---|
| 0 | FrameInfo | **N** | 26-bit frame, 22-bit cumulative command count, 16-bit CRC (0x4c1c00) | lockstep bookkeeping (§4.4, §7) | H |
| 17 | FrameSync | S | 8-bit new MaxAhead, 8-bit multiplier (0x4c1de0) | adaptive latency retune (§4.3), applied through a callback | H payload, M fields |
| 18 | PerformanceMetrics | **N** | 12-bit max peer latency, 7-bit measured ticks/s (0x4c1fa0) | diagnostics | H |
| 39 | GameOptions | S | 32-bit tactical speed index, 32-bit galactic speed index (0x44d600) | game speed (MAIN_LOOP §1.5), index < 5 | H |
| 51 | ResumeGame | S | 1 bit | GMM pause/resume (MP galactic battle vote) | M |
| 36 | SaveGame | S | 7-bit slot, string (0x48fb90) | the issuer's machine saves into the slot, the others call the save routine with −1 | M |
| 15 | QuitGame | S | none | flags the player as quit (player +0x63), reports to scoring, queues the player for removal | H |
| 49 | Withdrawl | S | none | player withdraws, through a manager at 0x140b31400 | L |
| 35 | Ally | S | P target (unpack 0x689b00) | toggles alliance issuer↔target, shows the friendly/hostile allegiance text | H |
| 16 | Chat | **N** | wide string, 1-bit allies-only (0x44d2d0) | chat line plus receive sound | H |
| 50 | Taunt | **N** | 32-bit taunt id (0x4d6af0) | plays a taunt sound | H |
| 11 | Debug | S | string | debug/cheat console callback (0x140b31698) | M |
| 4 | Look | S | q4, q4 (0x689740) | two large-range coordinates. Execute not located | L |

---

## 3. Serialization

### 3.1 Bit stream and packets [H]

- Writer 0x2046f0 and reader 0x2043b0 pack N-bit fields low bit first into consecutive bytes (bit cursor at
  +0x10 for writing and +0x14 for reading). A field read past the end returns 0.
- Strings: dedicated wide/narrow string writers (0x204fb0 / 0x204e00).
- Sender 0x3b03e0 (MP): scheduled and non-scheduled events go into **separate packets**. Each packet starts
  with a **5-bit event count** (≤ 31), patched when the packet is closed. When the next event does not fit,
  the packet is sent and a new one is started. A non-scheduled event whose slot-8 flag is set makes its
  packet go out immediately. The local copy of each event is added to the scheduled queue **after** it has
  been packed, so the sender executes the identical object. Events whose player id is not the local
  player are only queued locally and never sent [H].
- Receiver 0x2c2910 (MP; per remote player, in player-list order): for each event, read the 6-bit type,
  create it through the factory, Unpack. Then:
  - **non-scheduled** → Execute now (outside the tick) and release;
  - **scheduled** → append to the scheduled queue and increment that sender's received-count.

### 3.2 Float quantizers (table at 0x140b09c40, configured in 0x28ab50) [H]

Each entry holds {min, max, step, bits}. Bits = the smallest b with 2^b − 1 ≥ ceil((max − min)/step) + 1.
The effective step is then (max − min)/(2^b − 2). Encoding clamps to [min, max] and stores
trunc((v − min)/step_eff + 0.5). Decoding gives q·step_eff + min, snapped to exactly 0 when its magnitude
is below step_eff.

| idx | range | nominal step | bits | used for |
|---|---|---|---|---|
| 0 | raw 32-bit | — | 32 | unused default |
| 1, 2, 3 | −10000 … 10000 | 0.1 | 18 | x, y, z of every position |
| 4 | −1e6 … 1e6 | 0.1 | 25 | Look |
| 5 | 0 … 86400 | 0.01 | 24 | SpecialAbility durations (value × 1000) |
| 6 | −360 … 360 | 0.1 | 13 | facings (degrees) |
| 7 | 0 … 2 | 0.1 | 5 | — |
| 8 | 0 … 1e6 | 0.1 | 24 | SpecialAbility amounts |
| 9, 10, 11 | −360 … 360 | 0.1 | 13 | PlaceBeacon angles |

**Local round-trip** (0x23bc10 = quantize then dequantize): Execute of MoveToPosition, BombingRun,
PlanetaryBombard, Reinforce and SetUnitAbilityMode re-quantizes its own position fields first, and sets
z to 0 when |z| < 0.0001. The issuing machine therefore sees the same numbers as the receivers, and SP
behaves the same as MP. [H]

---

## 4. Scheduling

### 4.1 Single-player (session type 0 or 3) [H]

- UI code builds the event and calls OutgoingEventQueue::Add (0x3b01f0).
- In the same loop iteration, flush 0x3b08c0 (MAIN_LOOP step 8) moves each event to the scheduled queue
  and sets its execute frame to the **current** global frame.
- Executor 0x2c0c70 (step 10, SP only) then runs **every** queued event immediately, whatever its frame:
  outside the tick, also while paused.
- Result: an order issued between tick N and tick N+1 changes state before tick N+1. Orders given while
  paused take effect while paused (selection, order targets), and units act on them when ticking resumes.
- The executor is called again at T2 of the tick and has nothing left to do.

### 4.2 Outgoing duplicate filter (MP only) [H structure, L flag meanings]

> **Status 2026-10-30:** Reference only: describes the original; multiplayer is out of scope (CLAUDE.md, Gabriel 2026-10-29).

When the game is multiplayer and the flag 0x140b15490 is clear, Add searches the outgoing queue from newest
to oldest:
- entries with neither slot 7 nor slot 8 are skipped;
- a scheduled entry without the slot-9 flag ends the search;
- otherwise an entry of the same type makes Add discard the **new** event, with the log line
  "Discarding duplicate outgoing event %s on frame %d".

A scheduled event that is added increments the local player's sent-command counter (cumulative, per
player id, 0x4b3ad0).

### 4.3 Multiplayer stamping, turn length and MaxAhead [H]

> **Status 2026-10-30:** Reference only: describes the original; multiplayer is out of scope (CLAUDE.md, Gabriel 2026-10-29).

- At flush (0x3b03e0), each scheduled event gets execute frame = 0x27cf30(FS):
  - with T = FS +0x0c (turn length) and A = FS +0x08 (MaxAhead), take current frame + 1 + A + max(T − 1, 1);
  - round it **down** to a multiple of T;
  - if A = 0 and the result equals the current frame, use current frame + 1 instead.
- Session reset (0x27d020, at Start_Game):

| session type | T | MaxAhead | latency unit (+0x10) | resulting command delay |
|---|---|---|---|---|
| 1 and 4 | 1 | 2 | 2 | exactly +4 frames |
| 2, default | 5 | 10 | 2 | +11…+15 frames, on multiples of 5 |
| 2, with FS +0xb0b4 set | 2 | 4 | 2 | +5…+6 frames, on even frames |

  At 30 ticks/s, +4 frames ≈ 133 ms and +15 frames = 0.5 s.
- **Adaptive retune** (0x27bff0, session types 2 and 4 only, run by the machine whose local player has
  +0x61 set, i.e. the host [M]):
  - runs every 64·T frames;
  - takes the worst measured peer latency and the current rate;
  - derives a new MaxAhead = latency unit × k, with k ≥ 2 or 3 (≥ 4 or 6 frames) and k capped so that
    MaxAhead ≤ 64;
  - when MaxAhead changes, the host issues a **scheduled** FrameSync event (payload: new MaxAhead,
    multiplier), so every peer switches at the same frame.
  - The turn length itself is not changed by this path [M].
- **Re-alignment at execution** (0x27cf90, MP only): a due event's frame is rounded **up** to the next
  multiple of T. If it falls inside a deferral window [FS +0x2c, FS +0x30), it is moved to FS +0x30.
  The window's purpose is not identified [L].

### 4.4 The lockstep gate and FrameInfo (0x27c5f0, 0x27cb70, 0x27d400) [H]

> **Status 2026-10-30:** Reference only: describes the original; multiplayer is out of scope (CLAUDE.md, Gabriel 2026-10-29).

- **End of every tick (T13, MP or recording):**
  - compute the frame CRC (§7);
  - store {frame, CRC, cumulative sent-command count} in the own 256-entry ring at index frame & 0xFF.
- Every T frames (frame % T = 0), and while not stopped, send a **FrameInfo** event {frame, count, low 16
  bits of the CRC}. The count is the sent counter, not a per-frame number.
- Every 16·T frames, send PerformanceMetrics.
- While the loop waits more than 500 ms, FrameInfo is re-sent (0x27d6a0).
- **On receipt** (FrameInfo Execute 0x4c1c80 → Register_Frame_Information 0x27d400, other players only):
  - record the peer's latest frame;
  - write {frame, CRC, count} into that peer's ring;
  - back-fill up to 128 skipped older slots with the same count and CRC 0;
  - run the desync compare (§7.2).
- **Gate before each tick** (MP, at least 2 players):
  1. Every other human peer with a live connection must have reported at least one frame.
  2. With R = the slowest peer's reported frame, no tick while (my frame − R) ≥ MaxAhead
     ("FrameSynchronizer waiting for player %ls. My frame = %d, their frame = %d, MaxAhead = %d").
  3. Pacing: rate + 5, or rate + 10 when this machine is behind (MAIN_LOOP §1.2).
  4. For ring slot (my frame − MaxAhead + 1) & 0xFF, per peer:
     - the peer's recorded frame must equal mine;
     - the scheduled commands received from that peer must be ≥ the peer's declared cumulative count
       ("…received_commands = %d, their_commands = %d").

  Passing the gate proves that every command any peer could have stamped for the next frame is already
  in the queue.

### 4.5 Ordering inside a frame (executor 0x2c0c70) [H]

1. **Replay playback** (queue +0xbe): recorded events run first, in recorded order, while their frame ≤
   current frame ("Executing event %s on frame %d").
2. **Normal path:** for each player in PlayerList index order, walk the queue from head to tail (insertion =
   arrival order). An event of that player:
   - if non-scheduled, executes now;
   - if scheduled, executes when its re-aligned frame ≤ current frame, or always in SP.
   Each executed event is removed, recorded when recording is on, Executed (slot 4) and released.

Consequences:
- Two players' commands for the same frame run player 0 first, then player 1, and so on, not
  interleaved by arrival time.
- One player's commands run in the order that player issued them (reliable ordered connection plus
  in-order append).

---

## 5. Who issues commands [H unless noted]

| source | path | notes |
|---|---|---|
| Human input, HUD, command bar, galactic UI, setup phase, menus | build event → OutgoingEventQueue::Add → flush → scheduled queue | about 119 Add call sites, all in UI/input/GUI code (0x33xxx–0x3exxx, 0x2f9xxx–0x30exxx, CommandBarClass) |
| AI (AIBuildTask, GalacticTaskForce, ProduceObject, production behaviour, FleetLocomotor, ConcentrateFire, target finder) | build the event on the stack, set fields and the player, **call Execute directly**, release | no queue, no network, runs in the AI's own tick slot (PlayerList update T7) |
| Lua (story, AI plans, object scripts): `Reinforce`, `Spawn_Unit`-family, `Move_Object`, `Land_Units`/`Launch_Units`, `Invade`, `Garrison`, `Activate_Ability`/`Cancel_Ability` | same direct construct-and-Execute pattern | scripts run identically on every machine. Lua randomness uses the synced RNG (MAIN_LOOP §4.1). The event keeps the issuer its constructor stamps (the local player) unless the caller sets one: `GameObjectWrapper.Invade` does not, and Invade's Execute keeps only units of the issuer (A1_READINGS §31) |
| Story events (StoryEvent*, triggered by game state) | call game systems directly | not commands. Events notify the story system the other way (§6.3) |
| Network/session code | FrameInfo, PerformanceMetrics, FrameSync (FrameSynchronizer), ResumeGame (GMM vote), QuitGame/Withdrawl/Chat/Taunt/SaveGame (menus) | through the same outgoing queue |

In single-player there is no delay beyond "before the next tick" (§4.1). AI and Lua apply instantly
inside the tick in all modes.

---

## 6. Application

### 6.1 Where [H]

- MP: only at T2, after "frame += 1" and before GameModeManager::Update.
- SP: at step 10 between ticks (and T2, which is empty).

A non-scheduled event received from the network executes at receive time (step 9). None of them touch
sim state.

### 6.2 Validation done by Execute [H for the listed checks]

- **Object ids** are resolved through the GameObjectManager id lookup (0x2aca60) at execution time. A
  missing id makes that element a no-op. Several events also skip objects marked dying/removed
  (+0x3a0 bit 2) or attached to a parent (+0x2b0).
- **Ownership:**
  - TacticalSuperWeapon requires owner = issuer;
  - Select adds ids through a per-player add (0x2bd2f0) that is also filtered by a mode-level exclusion
    test (0x3400c0, [M] retreating/locked units);
  - the abilities check per-unit availability (0x39b480) and target validity (0x37c380).
  - Movement does not re-check ownership, because it acts on the issuer's selection.
- **Mode/state:**
  - SetMarkerID and SetGUIIndex apply only in galactic mode (mode type 0);
  - Reinforce branches on land (1) or space (2);
  - Select, FleetManagement and MoveThroughObjects are ignored in galactic mode while a battle is pending
    (counter 0x140b1540c > 0; FleetManagement and MoveThroughObjects also have an SP-only
    pending-conflict check);
  - ability type ids must be 1..76;
  - FrameInfo from the local player is ignored.
- There is **no rejection message to the issuer**: an invalid command is dropped silently. Only local
  feedback exists, such as the "can't do that" speech for the local player in Reinforce and the
  SetUnitAbilityMode response sounds.

### 6.3 What comes back out [H for the mechanisms listed]

There is no general event bus. Execute calls the target subsystem, and feedback leaves through:
- **direct audio calls** (SFX/speech manager 0x2d5290 / 0x2d5010): chat/taunt sounds, ability responses,
  "insufficient" speech. These are mostly limited to the local player by comparing the event's player id
  with the local player id;
- **registered function pointers** (GUI hooks): chat display (0x140b30668), game-options display
  (0x140b30678), save (0x140b313d8), debug console (0x140b31698), FrameSync apply (0x140b318c8, set by the
  FrameSynchronizer constructor), galactic fleet-changed notifier (0x140b39580);
- **story/tutorial notifications**: MoveToPosition posts a "unit moved" story event for local-player
  units (0x32ea20; it is queued as a "delayed event" when sent to an inactive mode), Reinforce notifies
  the story layer (0x32a910);
- **object signals** raised by the systems the event drives (OBJECT_RUNTIME signal list).

For the port: anything that only the local player sees may use the local player id. Anything that changes
sim state must depend only on the event fields and the sim state.

---

## 7. Sync checking

### 7.1 Frame CRC (Calculate_Frame_CRC 0x27c3d0) [H unless noted]

1. **GOM term:** the sum (32-bit wrapping) over the current mode's primary-store objects, skipping those
   with +0x3a0 bit 2, of the per-object CRC (0x4ee890; details in OBJECT_RUNTIME §8/§5). Because it is a
   sum, it does not depend on the order of the objects.
2. **RND term:** CRC-32 (reflected table at 0x140a14d20, initial value 0, no final inversion) of the
   4 bytes of the synchronized RNG state, little-endian.
3. **PLR term:** PlayerList 0x294840. The decompile shows only a loop over the players with no visible
   accumulation, so its value is not recoverable [L].
4. The **no-scoring CRC** is GOM + RND + PLR (wrapping add). It is logged and kept.
5. When (frame & 0x31) = 0, or the sync-debug option (0x140b0a310) is on, the scoring manager's CRC
   (TheGameScoringManager::Get_CRC, 0x485700) replaces the value, seeded with it [M]. With sync-debug
   on, a sampled per-object debug CRC (0x243420) is chained first.
6. The value is inverted (XOR 0xFFFFFFFF) unless the cinematic lock (FS +0x37) is on, or a land-mode
   condition holds (0x13ecd0 with 0x140a284c4 ≠ 0). Then 1 is added when FS +0x36 is set [L: test toggle].
7. When a mode exists, four more CRC-32 byte steps fold in a byte from the mode (vtable +0x240), the low
   byte of 0x140a284c4, and two zero bytes.

Computed once per tick at T13 when synced (MP, or the mode's record flag).

### 7.2 Desync detection [H]

When a peer's FrameInfo for frame f arrives with f ≤ my current frame, the code compares the **low 16
bits** of the peer CRC with the CRC in my own ring slot for f & 0xFF. On a mismatch:

1. FS +0x35 is set ("out of sync") and the detection frame is stored. Only the first mismatch counts.
2. GMM::Update tests the flag at entry and stops running, so the simulation freezes. Rendering and the
   network keep running.
3. The main loop shows "TEXT_OUT_OF_SYNC":
   - only "Stop" in MP;
   - "Continue after sync" in a sync-test playback mode (0x140b1574e) [M].
4. With per-frame sync dumps enabled (0x140b0a310), the main loop writes `log\Sync_p<player>_frame<f>.txt`
   for up to 128 recent frames (FrameHistory dumps of objects: ids, types, raw position/facing/velocity
   bits, behaviours; the 0x87a1d0.. format strings).
5. It dumps the event log to `oosevents.log` (the recorder in 0x2c20d0).

The CRC is only compared, never resent or repaired. Recovery is impossible by design.

### 7.3 Recording/replay (ScheduledEventQueue +0xbd record, +0xbe playback) [M]

When recording, each executed scheduled event is appended to a list with two dwords
(0x140b153c8/0x140b153cc). The file (default `temp.rec`, chunked) holds:
- the map name, start frame, a flag, the **session seed**, a 36-byte settings block (0x140b1549c) and one
  0x1d8-byte record per player;
- chunk 0x33 with every event bit-packed by its own Pack (≤ 1024 bytes each);
- chunks 0x34/0x35 with the two dword arrays.

A replay is therefore seed + settings + players + the scheduled command stream, which confirms that
those inputs fully determine a game. Loading (0x2c1270/0x2c11a0) returns the stored seed for Start_Game
(MAIN_LOOP §4.1).

---

## 8. Sim RNG — additions to MAIN_LOOP §4.1

`MAIN_LOOP.md` §4.1 already documents the algorithm (32-bit LCG, multiplier 0x41C64E6D, increment 0xBDF,
15-bit output (s >> 10) & 0x7FFF), the int/float/percent helpers, seeding and the two instances, and is
authoritative. Extra findings:

- **Inline copies of the synced generator** do the same step without the helper:
  - 0x532480: float in [lo, hi] with lo/hi swapped if needed;
  - 0x5324d0: float in [0, 1];
  - 0x623fe0: draws one step and uses bit 0 of (s >> 10) as a coin flip.

  They consume the synced stream exactly like the helpers. The port must count them as draws. [H]
- **Synced stream users** (callers passing 0x140a13e24):
  - 75 int-draw sites and 37 float-draw sites, plus 23 + 21 users of the inline float forms;
  - code ranges: combat/projectiles/behaviours (0x37xxxx–0x3axxxx), AI and galactic (0x5xxxxx–0x7xxxxx),
    Lua `GameRandom` (0x5423a0: no argument → int 0..65535, one argument n → 0..n, two → [a, b]) and
    `GameRandom.Get_Float` (0x5461b0). [H]
- **Free stream users** (0x140a13e20): audio (0x2d7a40, 0x2d5440), GUI/HUD, particles and render effects
  (0x4bec90 and neighbours, 0x164690), and the ranged forms of Lua `Free_Random` in SP. [H]
- **Other private generators**: functions in 0x49exxx–0x4a6xxx and 0x2cdxxx call the int helper on their
  own state (not 0x140a13e24/…e20). They are presentation-side [M] and must not feed the sim.
- **Save/restore around previews:** 0x40aa0 saves the synced state, runs a tactical preview/placement
  computation, and restores it, so the preview consumes nothing. [H] A port that adds sim-side "what-if"
  queries must do the same.
- **The RNG state is part of the frame CRC** (§7.1), so one extra or missing draw desyncs on the next CRC
  compare.
- **Debug:** `_RandomHistory.txt` logging of every synced draw (0x1ffdf0) prints the low/high, pre-seed,
  post-seed and result.

---

## 9. Mapping to the port (`engine/commands`, Q1)

These points reconcile the provisional choices recorded in PHASE4.md G0-3:

- **Drain order (tick, player slot, per-player sequence) matches the original** (§4.5). The original has no
  sequence number. Arrival order per sender plays that role, so a per-player sequence assigned at issue
  time is equivalent and is safer. [H]
- **Command point "start of tick, before systems" matches** (T2). [H]
- **Delay default 1 matches single-player** (§4.1). For MP parity, use delay = the §4.3 formula with T and
  MaxAhead per session type, and execute only on turn multiples. [H]
- **AI and Lua should bypass the scheduler** and call the same command handlers synchronously inside their
  tick slot. Routing them through the queue would add a tick of latency the original does not have and
  would reorder their effects relative to the rest of the tick. [H]
- **Positions in commands:** quantize with §3.2 (18-bit per axis over ±10000) at command creation or at
  execution, the same way on every machine. Do not carry raw floats into the sim. [H]
- **Selection as sim state:** either model Select/SelectAll/ControlGroup as commands (original) or put
  explicit unit lists into every order. If the original's semantics are kept, "acts on selection at
  execution time" must be preserved. [H]
- **Q1 checksum:** the original's sum-of-per-object-CRC + RNG CRC is order-independent and cheap, but
  the 16-bit compare and the narrow object coverage (position, facing, health) are weak. A whole-state
  hash is a superset. Keep the RNG state and the command stream in it. [M recommendation]

---

## 10. Open items

- The player-id bit width P (static at 0x140a16fb4) needs the binary's .data section or an in-game probe [L].
- The roles of slots 8/9 and which classes override them (the duplicate filter) are unknown, because the
  per-class vtables cannot be read from the decompile export alone [L].
- Execute for Attack, Escort, Look, Invade and PlanetaryBombard, and Pack for MoveObjectToObject, Escort,
  Garrison, Retreat and PlanetaryBombard, are folded into shared functions that the export does not
  attribute. Their payloads above come from field use [M].
- The FS +0x2c/+0x30 deferral window (0x27cf90) and the land-mode/FS +0x36 CRC modifiers [L].
- The PlayerList CRC term (0x294840) [L].

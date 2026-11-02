# Station upgrade in battle, hangar spawning, engine loop sound (StarWarsG.exe FoC x64 v1.121)

Recovered 2026-10-28 from the FoC x64 binary (RVAs relative to 0x140000000) and the shipped FoC XML. Behaviour
only; no decompiled code. Confidence: [H] read directly, [M] inferred from reads, [L] guess.

Relation to existing docs (not repeated here):
- HARDPOINT_DISABLED.md §3 (hardpoint-state copy of the upgrade) and §9.4 (bay pruning) are correct and are
  the reference for the hardpoint part of §1. This doc adds everything around them.
- ABILITIES_HARDPOINTS.md §2.4/§2.5 (DEPLOY_SQUAD gate and handler), §3.1 (SAP framework).
- TACTICAL_GAPS.md §1.4 lists `Starbase_Upgrade` under "galactic only". **Correction:** its interval (2) is
  real but the class runs in **skirmish tactical space** (Activation_Style 12 `Skirmish_Automatic`). [H]
- AUDIO.md §1.2 (start gates), §1.5 (stop/fade), §1.6 (volume, visibility mute), §1.7 (3D distance). §3 here
  only adds who starts and stops the engine loops.

Engine module per item: §1 → `objects` (object swap, contents, control groups) driven from `tactical`
(ability service); §2 → `tactical` (SpawnSquadron behaviour) using `objects` spawn helpers; §3 → `audio`
(SFX instances) fed by `tactical` (locomotor state) and `objects` (flags).

---

## 1. Space-station upgrade in battle (`Starbase_Upgrade_Ability`)

### 1.1 Trigger [H]
- Vanilla data: `RS_/ES_/US_Level_{Two..Five}_Starbase_Upgrade` (UPGRADEOBJECTS.XML) are `UpgradeObject`s with
  `Behavior DUMMY_UPGRADE`, `Tactical_Production_Queue Tactical_Units`, `Tactical_Build_Time_Seconds` 80/100/…,
  `Tactical_Build_Cost_Multiplayer`, `Build_Limit_Current_For_All_Allies 1`, prerequisite = the current
  skirmish station level, and one `Starbase_Upgrade_Ability` with `Activation_Style Skirmish_Automatic`. Each
  `Skirmish_*_Star_Base_N` lists the next-level upgrade in `Tactical_Buildable_Objects_Multiplayer`.
- So the player (or AI, AI_TACTICAL "Produce_Force of `*_Starbase_Upgrade`") **buys the upgrade object** at
  the station. The build time is the only delay; it is the normal tactical production time, not part of
  the ability.
- When the build completes, the upgrade object is created as a **DUMMY_UPGRADE content of the station**
  (station contents list obj +0xF8; its parent container obj +0x2B0 = the station). [M: the delivery path
  was not traced; the ability resolves the station through +0x2B0, and DUMMY_UPGRADE objects are excluded
  from garrison space in L1_S4_REMAINING, so they live in that list.]
- At the object's creation the SAP init (0x37c050) sees an ability with service interval 2 that is allowed
  in the current mode (style 12 = skirmish only, ABILITIES_HARDPOINTS §3.1) and ready (`Initially_Enabled`
  default), and schedules its first service at **synced `Get_Int(now, now + 2)`** (0x1ffb40 on 0x140a13e24;
  one synced draw, inclusive range) [H]. Outside skirmish the ability is never scheduled, so the upgrade
  object does nothing [H].
- The SAP update (inside the upgrade object's `GameObject::Update`, ABILITIES_HARDPOINTS §6.2) calls the
  class update 0x70ec20 on the first frame ≥ the due frame:
  - if the SAP's "upgrade done" byte (SAP +0x230) is not 1: run `Do_Starbase_Upgrade` (0x70f070), store its
    result (0/1) in that byte, then **kill the upgrade object** (Kill 0x391310, flag 1). The kill happens
    whether or not the upgrade succeeded, so the ability is one-shot. [H]
- Lua `Set_Garrison_Spawn`, AI and story do not call this path. 0x70f070 has exactly one caller. [H]

### 1.2 `Do_Starbase_Upgrade(ability, upgradeObj)` (0x70f070), in order [H unless tagged]
1. **Resolve the station S.** If the upgrade object has the station behaviour (byte obj +0x38E ≠ −1) S is
   the object itself; otherwise S = its parent container (+0x2B0), which must also have it. Else return 0.
2. **Next type N** = S's type `Next_Level_Base` (resolved pointer type +0x8C0; name at +0x8B8). None → return
   0 (top level, `Skirmish_*_Star_Base_5` has none).
3. **Spawn S'** of type N via the normal tactical create (0x29f810) for **S's owner** (S +0x58), at S's
   position (+0x78) and S's facing (+0x84), no extra flags. Failure → return 0 (S survives).
   Creation runs the usual construction: fresh behaviours, full hull and shields, fresh hardpoints (each at
   full health, enabled), fresh SAP (its Skirmish_Automatic abilities are scheduled with their own synced
   draws, e.g. `Income_Stream_Ability`), fresh SpawnSquadron data, fresh ability cooldowns; byte +0x3CB = 1
   because N has SPAWN_SQUADRON (§2.1).
4. **Copy hardpoint state S → S'** (0x70ec70): exactly HARDPOINT_DISABLED §3 step 4 (by index; repairing,
   destroyed or disabled hardpoints come back disabled, health 0.1 or the old health; allied
   `SpecialWeaponRequiredHardpointEnabled` byte).
5. **Move contents S → S'** (0x70eec0): if S has a contents list (+0xF8): snapshot it, then for each entry in
   list order remove it from S (0x37a8d0, no eject effects) and add it to S' (0x379d30). This moves
   **garrisoned units and DUMMY_UPGRADE objects, including the upgrade object that is running this ability**
   and any earlier upgrades (`Enhanced_Shielding`, `Improved_Weapons`), so their effects stay registered on
   the live station. Then copy the two bytes +0x30 / +0x31 of the contents pack [L meaning]. S' creates its
   own contents pack if it has none (0x390740).
6. **Per player i = 0 … count−1 (player-list order)**, when player i exists:
   - **selection**: if S is in player i's selection (per-player selection list, mode +0x1C0 + i·0x48), add S'
     to it (0x2bd360). S is dropped from the selection later by Kill.
   - **control groups**: if S is in one of player i's 10 hot-key groups (0x2811d0), add S' to the same group
     (0x27f560). For the local player this also sets S' +0x331 := group number (HUD group badge).
7. **Owner O** (the upgrade object's owner, +0x58):
   - **tech level +1** (0x282190: only when below the player's max tech; runs the normal tech sweep).
   - **production retarget**: in O's tactical build manager (player +0x3A0; two queues at +0 and +0x18),
     every entry whose builder id (+0x2C) is S's id gets S'’s id (0x497460). In-progress and queued builds
     at the station continue on S' with their progress.
8. **Allies of O** (0x294750: every other player whose relation to O's id is allied, player-list order),
   for each: **tech +1** (0x282190) and, if it has a tactical build manager, the same retarget. So **every
   allied player gains a tech level** when one ally upgrades a station. [H read, M gameplay reading]
9. S' byte +0x3CB := S +0x3CB (garrison/hangar spawn enabled, §2.1; Lua `Set_Garrison_Spawn`).
10. **Remove S**: Kill 0x391310(S, flag 1). With S's +0x3A2 bit 0x20 clear (normal), the flag has no effect,
    and this path runs **no respawn scheduling, no `Destruction_Survivors` draw, no death clone and no death
    explosion** (those come from `Die`, which is not called). It does: deactivate active unit abilities,
    eject/transfer remaining contents (none left), the skirmish `Tactical_Destruction_Decrements_Tech_Level`
    test (vanilla stations do not set it), population capacity refresh for the owner's allies (0x289010,
    `Additional_Population_Capacity` 10/13/15/16/17), and `Destroy_Object` (detach, signal 0x10, removal
    from the selection manager, pending delete). [H for the calls, M that nothing else is visible]
    If battle statistics tracking is on, the kill records S as a lost unit (0x27fad0) [L].
11. **Announcement** (2D, 0x2d5290), chosen by the **local** player L relative to the **upgrade object's
    owner** O: L = O → faction `SFXEvent_Starbase_Upgraded` (+0x2060); L allied to O →
    `SFXEvent_Starbase_Ally_Upgraded` (+0x2080); L enemy of O → `SFXEvent_Starbase_Enemy_Upgraded` (+0x2070).
    Return 1. Not sim state.

### 1.3 What carries over (summary)
| State | Carried | Notes |
|---|---|---|
| Owner | yes | S' is created for S's owner [H] |
| Position, facing | yes | copied at create [H] |
| Hull health, shields | **no** | S' starts full (new object) [H: nothing copies them] |
| Hardpoint health / destroyed / disabled / repairers | yes, by index | HARDPOINT_DISABLED §3 [H] |
| Selection, control groups | yes | per player [H] |
| Ability cooldowns, active abilities, SAP state | **no** | fresh SAP; S's active unit abilities are switched off by Kill [H] |
| Garrisoned / contained objects, upgrade dummies | yes | contents list moved in order [H] |
| Production queue at the station | yes | builder id retargeted for owner and allies [H] |
| Hangar state (spawned counts, timers) | **no** | S' builds a new spawn list (§2.3); S's launched squadrons keep flying but are no longer counted by anyone, so S' launches a full new complement [M] |
| `Set_Garrison_Spawn` flag (+0x3CB) | yes | [H] |
| Object id | **no** | S' has a new id; script/AI references to S die with S [H] |

### 1.4 Timing [H]
- Build: normal tactical production time of the upgrade object (80 s for level two, etc.).
- Ability: first service 0–2 frames after the upgrade object's creation (one synced draw).
- Swap: **instantaneous within that one service**. No build-up animation, no construction state, no
  invulnerability window. S and S' coexist only inside the call.

### 1.5 What the AI and HUD see [M unless tagged]
- AI: S' is a new object registered through the normal GOM add hook (AI perception, threat, fleet
  bookkeeping); S disappears through the normal removal path. Nothing moves AI goals from S to S' except the
  build-queue retarget (§1.2 step 7–8). AI plans that referenced S re-acquire the station by query.
- HUD: selection and groups survive; the station's build queue keeps running (retarget); the special-weapon
  readiness reads the disabled flag of S'’s special-weapon hardpoint (HARDPOINT_DISABLED §4). Announcement
  per §1.2 step 11. The tech-level rise unlocks new buildables on the command bar at once.

### 1.6 Per-tick object order and determinism [H unless tagged]
- The whole swap runs inside the **upgrade object's** service (store serviced-list walk, newest-registered
  first, OBJECT_RUNTIME §5.3), in its behaviour/SAP step.
- S' is created during the walk and registered at the newest end, which the walk has already passed:
  **S' is not serviced until the next frame** [M, as OBJECT_RUNTIME §5.3]. Its first SpawnSquadron service
  and SAP abilities are scheduled by attach/init draws (OBJECT_RUNTIME, TACTICAL_GAPS §6.4).
- S stays in the lists with zero serviced behaviours until the deferred-delete flush of this frame; if S was
  newer than the upgrade object it was already serviced this frame, otherwise it is skipped.
- Synced RNG draws, in order: S' creation (behaviour first-due draws for every behaviour with interval ≥ 2,
  in attach order; SAP first-due draws for its interval > 0 abilities), then none in steps 4–11 (Kill takes
  the no-survivor branch). The upgrade object's own SAP draw happened at its creation.
- The new object id comes from the normal id allocator, so ids of every later object shift by one relative
  to a run without the upgrade.
- Port order to reproduce: create S' → hardpoint copy (index order) → contents move (list order) →
  per-player selection/groups (player index order) → owner tech/retarget → allies (player index order) →
  +0x3CB copy → Kill S → (cosmetic) announcement → kill the upgrade object (in 0x70ec20).

---

## 2. Hangar fighter spawning (`SPAWN_SQUADRON`, behaviour 0x1A)

`SpawnSquadronBehaviorClass` (vtable 0x8a33f8), data pack `SpawnSquadronDataPackClass` at obj +0x118
(0x68 bytes, created by the behaviour's data-pack hook 0x390e80). Used by stations (all `*_Star_Base_*`),
carriers (Star Destroyer, Victory, Executor, Calamari…) and land spawn structures. [H]

### 2.1 XML fields [H offsets, M meanings]
| Field | Type +off | Default | Meaning |
|---|---|---|---|
| `Starting_Spawned_Units_Tech_0..5` | +0xAD0 + 0x18·N | empty | (type, count) list: **count = how many of that type may be alive at once** |
| `Reserve_Spawned_Units_Tech_0..5` | +0xB60 + 0x18·N | empty | (type, count): extra spawns beyond the starting count; **−1 = unlimited** |
| `Spawned_Squadron_Delay_Seconds` | +0xA98 float | 5.0 | seconds between launches |
| `Spawned_Squadron_Location_Bones` | +0xAA0 (0x20-byte strings) | empty | **land only**: bone names to spawn at |
| `Spawned_Squadron_Location_Flyout_Distances` | +0xAB8 (floats) | empty | land only: per-bone fly-out distance |
| `Spawn_Garrison_On_Load` | +0xA0 bool | false | spawn the whole starting set at once (land structures) |
| `Spawned_Squadron_Types` | +0xA78 | — | parsed but **not read** by the behaviour (or any gameplay code found) |
| HardPoint `Fighter_Bay_Flyout_Distance` | hp type +0x448 float | 0 [L default] | space launch: distance along the bay bone's X axis |
| `Squadron_Units` (spawned type) | +0x970 list (data +0x978, count +0x980) | | member types of a squadron |
| `Company_Units` (spawned type) | +0x950 (data +0x958, count +0x960) | | land company members |
| `Max_Squad_Size` (spawned type) | +0x1E44 | 10 | land chunk size |
| `Is_Community_Property` (spawner) | +0x27 | false | skirmish: rotate the owner of spawned units among allies (§2.5) |
| obj +0x3CB | | 1 for SPAWN_SQUADRON objects (set at create 0x29f810) | hangar/garrison spawn enabled; Lua `Set_Garrison_Spawn(bool)` (0x580250) |

**Tech slot**: the lists used are those of the **highest N ≤ owner's tech level** (player +0x84) that has
starting entries (counts and reserves are looked up in the slot found the same way, separately). Tech > 5
gives no entries. Vanilla only uses Tech_0. [H]

Vanilla values: stations `X-Wing/Y-Wing/TIE … , 2–10` with reserve −1, delay 10 s; Star Destroyer
`TIE_Interceptor_Squadron 2, TIE_Bomber_Squadron 1`, reserve interceptor 8, delay 5 s; Executor 4/6/4/1 with
reserves 16/24/16/5, delay 8 s. There is **no** `Max_Squad_Size` on space spawners; that field is used only on
land companies. [H]

### 2.2 Data pack [H]
| Off | Meaning |
|---|---|
| +0x08 vector of 0x18-byte entries | { +0 type, +8 **live**, +0xC **max alive** (starting count), +0x10 **remaining** (start + reserve, or −1) } |
| +0x20 vector | cached FIGHTER_BAY (type 3) hardpoint pointers |
| +0x38 vector | land spawn bone indices |
| +0x50 int | state: 0 idle, 1 opening (land), 2 launched-waiting (land), 3 closing (land), 4 manual (DEPLOY_SQUAD) |
| +0x58 | land: the unit currently leaving (listener for signal 10) |
| +0x60 int | player index that owns the next spawn |
| +0x64 int | **next spawn frame** |
Saved: +0x64 (chunk 1), +0x50 (8), +0x58 (9), +0x60 (10), the bay list (0xB), bone list (0xC), and per entry
type CRC (4), live (5), max (6), remaining (7).

### 2.3 Building the list and the initial launch [H]
- Behaviour service interval **30 frames**; first due frame = attach frame + synced `Get_Int(0, 29)`
  (TACTICAL_GAPS §6.4). `Initialise` clears the entry list.
- On a service with an empty entry list: build it (0x594b30): +0x60 := owner's index; one entry per starting
  type in list order with live 0, max = starting count, remaining = reserve ≥ 0 ? start + reserve : −1;
  **next spawn frame := now**; land bone list; bay list = the object's hardpoints of type FIGHTER_BAY in
  hardpoint-list order. If `Spawn_Garrison_On_Load`, also run the bulk spawn (0x593960, land structures only:
  not in skirmish, needs +0x3CB, spawns every entry × count at once at the object/bones, then a move order).
- Same service, state 0: if can-spawn (§2.4) → launch one unit (§2.5) → next spawn frame := int(now +
  FPS × delay) (float sum, then truncate). So **space carriers and stations launch one squadron per launch,
  not all at once**: the first on their first service (0–29 frames after creation), then one every `delay`
  seconds, the actual frame being the first 30-frame service at or after the due frame.
- The list is rebuilt only if it becomes empty again (never in practice): tech-level rises after creation
  do **not** change a spawner's list. [H read, M consequence]

### 2.4 Can-spawn (0x593350), all must hold [H unless tagged]
1. Not suppressed by the in-game cinematic camera (0x140b27a10 active and global 0x140b27d90 = 0) unless the
   type has `Spawn_Garrison_On_Load`. [M names]
2. Campaign battle on a planet with corruption type 5 (`Corruption_Bribery`): only owners whose faction has
   `Benefits_From_Corruption` (+0x10B) spawn.
3. The spawner's locomotor does not report the hyperspace byte (locomotor pack +0x260; hyperspace arrival /
   departure) [M meaning].
4. Unit ability **DEFEND (1) is not active** (power to shields stops launches).
5. State = 0 and next spawn frame ≤ now.
6. **Space only:** erase every bay hardpoint that is destroyed (destroyable and health ≤ 0) from the cached
   list; **an empty list → no spawn, permanently** (erased bays never return; a disabled bay still counts,
   HARDPOINT_DISABLED §9.4). A space spawner without any FIGHTER_BAY hardpoint never spawns.
7. If obj +0x3CB ≠ 0: one synced draw r = `Get_Int(0, entries − 1)` (no draw with one entry); scan entries
   circularly from r; true at the first entry with live < max, remaining ≠ 0, and the type not restricted by
   the battle planet (`Planet_Restricted_Unit_Categories` +0x808 / `_Types` +0x810 of the planet object,
   0x373490).
8. Else (no eligible entry, or +0x3CB = 0), skirmish space station only (+0x38E ≠ −1): first player index i
   allied with the owner whose pending space-delivery list (player +0x2D8, count +0x2E8) is non-empty →
   +0x60 := i, true. [H read; L: what fills that list — skirmish-bought space units waiting to launch]

### 2.5 Launch one (space: service state 0) [H unless tagged]
- If the spawner has the DEPLOY_SQUAD (75) unit ability and its owner is **not** an AI player (0x2814d0
  false, or player +0x62 ≠ 0): state := 4 and nothing spawns. Launches then happen only through the
  DEPLOY_SQUAD activation (ABILITIES_HARDPOINTS §2.5: reset, next-ready frame, recharge). Vanilla: Executor.
- Else pick (0x594600): if +0x3CB ≠ 0, a second synced draw r = `Get_Int(0, entries − 1)`, circular scan
  from r, first entry with live < max, remaining ≠ 0, not planet-restricted, **and whose spawn succeeds**:
  remaining −= 1 when > 0; live += 1. If the spawner is `Is_Community_Property` in skirmish, +0x60 advances
  round-robin (player-list order, wrapping) to the next player allied with the spawner's owner, so
  community stations hand out fighters to allies in turn. Return.
- Else (skirmish space): spawn the first type of player +0x60's pending list (removing it from that list).
- Then next spawn frame := int(now + FPS × delay) whether or not something spawned.

**Spawn position (space)** (0x594dc0):
- bay = synced `Get_Int(0, bays − 1)` (no draw with one bay); P0 = bay bone world position (hardpoint bone
  +0x2C, else +0x38; zero if the bone is missing); D = bay bone X axis × `Fighter_Bay_Flyout_Distance`;
  P1 = P0 + D.
- **Squadron type** (has `Squadron_Units`): each member created at **P0**, owned by player +0x60, facing
  along P0 → P1, with initial velocity = normalize(P1 − P0) × member max speed (locomotor velocity);
  then the squadron container is built (team helper 0x2a5a60, REMAINING_GAPS); flag +0x3A2 |= 2 (spawned);
  listener for signal 0x28 (removal) registered to the spawner. **Initial order:** if the carrier has the
  behaviour at +0x333 (slot 0x01 = locomotor present, i.e. a moving carrier) → an order of kind 3 with the
  carrier as target (guard/escort the carrier); otherwise (stations) a formation move to P1.
  Resolved (S7 t24 RE, 2026-11-02) [H]: the moving-carrier order is a new coordinator's `Move_To` (0x4f8200) with a
  block {kind 3, object = the carrier, flags 0x02 (guard), mask 0x3f, no threat (1e18), word 0x1000001}; the station
  case is a group move (0x2a7b80) of {kind 1, position P1, flags 0}. When the carrier dies, the coordinator's target
  listener (0x4f77e0) turns the block into kind 1 at the last waypoint of the formation's path, flags still 0x02, so
  the squadron guards that point (`Guard_Chase_Range`). Details: FIGHTER_FLYING §4.10 E.
- **Single ship** (no `Squadron_Units`, e.g. station corvettes/frigates): try positions around P1 on rings
  r = 0, 200, 400, 600, 800 at angle steps of 45° (r = 0 once), the first one the space placement test
  accepts; create there facing along P0 → P1; spawned flag; removal listener; if the new ship has a
  locomotor, locomotor vf+0x290(1) [L]. If it came from the pending list, register it with the player's
  reinforcement list (0x2b5f70). If player +0x60 is the local player, play faction
  `Reinforcements_Enroute_SFXEvent` (+0x2270, 2D). No ring free → no spawn.
- Land (mode 1): spawner position and facing, or a synced random bone and its fly-out distance; anim
  `Structure Open` (68) → state 1 → spawn + `Structure Hold` (69) → state 2; the unit's signal 10 (left the
  structure [M]) or removal → `Structure Close` (70) → state 3 → state 0 and next := now + delay. Companies
  spawn min(`Company_Units` count, `Max_Squad_Size`) members, then a move along the facing by the fly-out
  distance (model radius by default).

### 2.6 Population cap [H for the reads]
- Can-spawn and launch **never test the population cap**. Spawned units are created even when the owner is
  at or above `Space_Tactical_Unit_Cap`.
- Whether they consume population depends on the per-object population share (combat pack +0x198) that the
  used-population sum reads (AI_GALACTIC_3 §3.2); its writer is still open there. [L]

### 2.7 Losses, respawn, reserves [H]
- Removal of a spawned unit (signal 0x28 on the squadron container or single ship) reaches the spawner
  (0x593760):
  - land: if it was the unit still leaving (+0x58): stop listening, play `Structure Close` → state 3, or
    state 0 with next := now + delay when the clip is missing.
  - find the first entry whose type equals the removed object's type; live −= 1.
  - **respawn timer restart**: if every entry up to and including the matched one was full (live ≥ max,
    before the decrement; all entries if none matched) and state is 0, next spawn frame :=
    int(FPS × delay) + now (truncate first, then add). Otherwise the running timer is kept.
- A squadron counts as lost only when its container is removed (all fighters dead). Individual fighter
  losses do not trigger respawn (REPLENISH_WINGMEN is a separate ability).
- Reserves: remaining starts at start + reserve and drops by 1 per launch; at 0 that type never launches
  again. −1 = endless. Star Destroyer: 2 interceptors at a time, 10 interceptor launches in total, 1 bomber
  launch in total.

### 2.8 Hangar hardpoint destroyed or disabled [H]
- **Destroyed** bay: erased from the bay list on the next can-spawn; launches use the remaining bays; none
  left → the spawner is dead for good (even if hardpoints are later "repaired": destroyed ones cannot be).
- **Disabled** bay (station upgrade copy): still in the list, still launches.
- The bay also gates DEPLOY_SQUAD via the live count, which excludes disabled bays.

### 2.9 Carrier dies or retreats [M unless tagged]
- Spawned units are independent objects (no parent link). The carrier's death does not kill them; Kill
  ejects or kills only the **contents** list, which spawned units are not in [H for Kill's lists]. Their
  removal listeners pointed at the dead behaviour and are dropped with it.
- Station upgrade: the old station's squadrons survive the swap uncounted (§1.3).
- Retreat: while the carrier is in hyperspace (+0x260) it launches nothing (§2.4 item 3). Spawned squadrons
  are judged by the space retreat coordinator like any unit (team type `Hyperspace` +0x24, 0x532c80) [H];
  units not on the retreat list are destroyed after the retreat (BATTLE_TRANSITIONS §3.4) [H]. Whether a
  jumped-out spawned squadron gets a galactic object is open [L: no persistent id, so no].
- The +0x3A2 bit 2 "spawned" flag is read by: the kill path (excluded from battle statistics), the AI free-
  unit score (+2, AI_GALACTIC_2), and HUD presentation. Nothing else. [H]

### 2.10 Order of multiple spawners and hangars [H]
- Each spawner is an independent behaviour serviced every 30 frames from its own staggered first-due frame,
  inside the object's service, in the store order of OBJECT_RUNTIME §5.3 (newest object first).
- Inside one spawner: one launch per service at most; entry pick and bay pick are synced draws in this order
  per launch: (can-spawn entry draw, if +0x3CB and ≥ 2 entries) → (launch entry draw, same condition) →
  (bay draw, if ≥ 2 bays). Bays are not round-robin.
- Port checklist: keep the two separate entry draws (the can-spawn draw is not reused), the 30-frame
  service, the float-then-truncate next-frame formula on launch vs truncate-then-add on loss.

---

## 3. Engine loop sound (`SFXEvent_Engine_Idle_Loop` / `SFXEvent_Engine_Moving_Loop`)

### 3.1 Data [H]
| Field | Type offset | Used by |
|---|---|---|
| `SFXEvent_Engine_Idle_Loop` | +0x1400 | idle loop; playing flag obj **+0x3B6** |
| `SFXEvent_Engine_Moving_Loop` | +0x1410 | moving loop; playing flag obj **+0x3B5** |
| `SFXEvent_Engine_Cinematic_Focus_Loop` | +0x1420 | only ever **stopped** (§3.4); no start call exists [M] |
| SFXEvent `Loop_Fade_In_Seconds` (+0xBC) / `Loop_Fade_Out_Seconds` (+0xC0) | event | fade passed by the start/stop calls |

Vanilla presets (SFXEVENTSENGINES.XML): `Preset_EGLC_Idle` and `Preset_EGLC` are 3D, Priority 1,
**Max_Instances 2**, Play_Count −1, **fade in 0.5 s, fade out 1.0 s** (comment: "crossfading from idle to
moving and vice-versa"), volume 70 (idle) / 80 (moving), saturation distance 800. Capital ships often use
the **same sample** for both events (Star Destroyer: `EGL_Star_Destroyer_1.wav`), so the switch is a volume
crossfade. Users: corvettes, frigates, capitals, space heroes, transports, land vehicles. **Fighters,
bombers and squadrons have none.** Stations use `SFXEvent_Ambient_Loop` (ambient behaviour), not these.

### 3.2 The two primitives [H]
`Set_Engine_Idle_Loop(obj, on)` (0x3a7cf0) and `Set_Engine_Moving_Loop(obj, on)` (0x3a7e30), identical apart
from the event and flag:
- Do nothing when the object is dead (+0x3A0 bit 0x02) or engine audio is suppressed (obj **+0x3B9** = 1).
- For a team container (TEAM slot +0x348 ≠ −1) the event is taken from the type of the team's member
  (0x4ec820) when there is one [M: first member].
- on = 1 and flag = 0: start the event attached to the object (0x2d5320) with fade-in = the event's
  `Loop_Fade_In_Seconds`; flag := 1. on = 0 and flag = 1: stop it on that object (0x2d5c00) with fade-out =
  `Loop_Fade_Out_Seconds`; flag := 0. Otherwise nothing.
- **The flag changes even when the type has no event** and even when the SFX start is refused by a gate
  (pool, probability…). The flag is the requested state, not "audible". A refused start is not retried
  until the flag goes 0 → 1 again.
- Log lines "Starting/Stopping engine audio idle/moving loop w/ fade".

### 3.3 Who calls them (complete caller list) [H unless tagged]
| Caller | When | Effect |
|---|---|---|
| object enters the active tactical mode (0x3952e0; mode vf+0xF0 true and the object belongs to that mode) | creation in battle, reinforcement spawn, hyperspace-in start, map load | **idle on, moving off** |
| **SimpleSpace locomotor service** (0x626420; corvettes, frigates, capitals, transports) | every service in path-following (0x13), stopped (0x16) or idle (0) state, after the motion step | moving := speed ≥ 1.1 × `SpaceIdleMovementSpeed` (GameConstants, 0x140b16d54); idle := speed < that. **Moving call first, then idle.** Speed = locomotor pack +0xEC |
| SimpleSpace in hyperspace states (vf+0x210 true: states 4, 5, 0x28, 0x2C) | arrival / departure / scripted hyperspace | returns early: **loops untouched** |
| engines offline (0x3924f0: last engine hardpoint destroyed, station-upgrade disable) | | moving off, idle off |
| engines online (0x393980, repair to full) | | idle on (HARDPOINT_DISABLED §9.1) |
| timed engine disable in `Take_Damage` (0x3a9e30, energy-pool projectile) | | moving off, idle off; the timer expiry does not call audio |
| land transform (0x3a9910, type +0x74 locomotor swap) | | moving off, idle off |
| Walk (0x61e930), Bike (0x639e90), LandTeamInfantry (0x62ca10) locomotors, JetPack start/move/end (0x642530 / 0x6420e0 / 0x641df0), tactical super weapon (0x42a7b0) | land; same idle/moving switch on their own "moving" test [M: tests not traced] | |

Not callers: FighterLocomotor (fighters), StarshipLocomotor (unused legacy), Fleet/Team locomotors, death,
retreat, fog, camera, pause.

### 3.4 Suppression flag +0x3B9 [H calls, L context]
- 0x392410(obj, fade): stops moving, idle and cinematic-focus loops with `fade` (or each event's own
  fade-out when fade < 0), clears +0x3B5/+0x3B6, sets **+0x3B9 := 1**. While set, §3.2 does nothing.
- Cleared by 0x393970 and 0x3989a0; copied by 0x472560 (object state copy).
- Callers are in the 0x635xxx land behaviour block next to the garrison/vehicle host code (0x635180,
  0x635310, 0x635450, 0x6355c0, 0x635750 set; 0x635950 clears) [L: crew leaving / holstering / garrison
  vehicle]. Not used in space. After the clear, nothing restarts the loops until the next locomotor service
  or mode enter turns a flag 0 → 1.

### 3.5 The resulting rule for a space ship [H for the code, M for the composition]
1. **Appears in battle** (any creation path, including reinforcements that hyperspace in): idle loop starts
   at mode enter with 0.5 s fade-in. During the hyperspace-in sequence the model is hidden, so the instance
   runs **muted** (AUDIO §1.6 visibility factor) and becomes audible when the ship is shown (state-4 timer 25
   for reinforcements); the locomotor does not touch the loops until hyperspace ends.
2. **Starts moving** (speed reaches 1.1 × `SpaceIdleMovementSpeed`): moving loop starts (0.5 s fade-in) and
   idle stops (1.0 s fade-out) **on the same service**.
3. **Slows below the threshold**: moving stops (1.0 s fade-out), idle starts (0.5 s fade-in). The threshold
   is the same in both directions (no hysteresis).
4. **Crossfade mechanics**: a stopped instance with fade > 0 stays on the active list, its level dropping by
   (1/30)/fade per SFX update until 0, then it is removed. Fading instances are **ignored** by the
   "already attached" loop gate and by the Max_Instances count (0x2d4470 counts only non-fading ones), so a
   restart during a fade creates a **new** instance that fades in while the old one fades out (two
   instances of one event briefly coexist on one ship).
5. **Engines destroyed/disabled**: both loops fade out. But the locomotor keeps servicing: on the next
   service the stopped ship (speed below threshold) gets **idle back on**. So disabled engines are heard as
   "idle", not silence. Repair to full also starts idle.
6. **Death**: no engine call. The death behaviour (0x58cf90) and object removal (0x2a6a00) kill every SFX
   instance attached to the object **immediately, no fade** (AUDIO §1.5). The dead bit then blocks restarts.
7. **Hyperspace out / retreat**: loops keep their last state through the jump (locomotor returns early in
   hyperspace states); the object is removed at the end of the jump, which kills them without fade. [M]
8. **Fog of war, hidden, stealth**: instances keep running at gain 0 (AUDIO §1.6); no stop/start happens.
   Coming out of fog makes the running loop audible at once, with no fade-in.
9. **Camera distance**: no start/stop. Attenuation is the 3D model of AUDIO §1.7 (min distance = 800 ×
   mode saturation mod, max 15000). With Max_Instances 2 per event, only the two nearest ships holding a
   voice for that event are heard; a loop that loses its voice ends its cycle (state 11) and, since it
   loops forever, re-enters state 1 and re-tries a voice on the following updates (new free-RNG volume
   draw each cycle). Nearest-first sorting decides who wins.
10. **Pause**: the sim does not tick, so no flag changes; audio is updated only while the application is
    active (AUDIO §6.1) and nothing in the engine-loop path stops loops on pause. [M: whether the pause menu
    mutes the SFX bus was not traced; L]
11. **Mode push/pop, load, quit**: stop-all kills everything (AUDIO §1.5); flags are not cleared by that
    path [M], so a mode return relies on mode enter (idle on / moving off) to restart.

### 3.6 Port notes
- Keep the per-object requested-state flags in sim-side object state (they gate calls, and the calls are
  made during the tick), but they must not feed back into sim logic. The SFX instances themselves are
  audio-side.
- Evaluate the switch every locomotor service, moving first, with the strict `<` / `≥` threshold above.
- Implement stop-with-fade as "mark fading, exclude from instance counts", not as "stop then start", or the
  restart during a fade will be refused (the symptom the port shows now).

> **Status 2026-10-30:** The "symptom the port shows now" no longer occurs. The tactical request flags and Start / Stop requests are in `engine/tactical/hangar/include/alamo/tactical/hangar/engine_loops.hpp`, and the audio side marks fading instances and excludes them from the instance and already-attached counts (`engine/audio/src/audio_system.cpp:439-453` for the check, `stop_sfx` at `audio_system.cpp:522-543` for the fade). Death and removal kill instances without fade (`object_removed`, `audio_system.cpp:556`).
- Death and removal: kill attached instances without fade; do not route through the fade path.

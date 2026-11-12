# Skirmish setup, production, victory and income (FoC x64)

Behaviour recovered from the retail binary (RVAs relative to 0x140000000) for the skirmish host.
Confidence: [H] read and cross-checked, [M] read once or partly inferred, [L] guess.
Related: `AI_TACTICAL.md` §7, `L1_S4_REMAINING.md` §3.6 / §4.2, `STATION_HANGARS_SOUND.md` §2.4,
`BATTLE_TRANSITIONS.md`, `TACTICAL_GAPS.md` (0x2a4fc0 facing records).

## 0. Globals and the skirmish predicate

- GMM (game mode manager) 0x140b153e0; current mode at 0x140b15418, its GOM at mode[3]; mode vf 0xe0
  returns 1 land, 2 space. GameConstants 0x140b15800, PlayerList 0x140a16fd0, FPS 0x140b0a340 (30). [H]
- "Is skirmish" 0x28afb0: current mode is tactical (vf 0xf0) AND (GMM+0x1e5 set, the SKIRMISH_RULES
  story override, OR the mode has no parent mode (0x28aa00 returns null) and GMM+0xb0 == 0). [H]
- Game options record 0x140b1549c (GMM+0xbc): +0 start credits, +4 start tech, +8 max tech,
  +0xc auto-resolve, +0x10 timer, +0x14 win condition (0x140b154b0), +0x18 int param, +0x1c float param,
  +0x20 flags (0x140b154bc): 0x1 heroes, 0x2 "free starting units" (IDC_ALLOW_STARTING_UNIT_PURCHASE /
  TEXT_FREE_STARTING_UNITS), 0x4 super weapons, 0x8 pre-built bases, 0x10 random events, 0x20 record valid. [H]
- Defaults from GameConstants MP_Default_* (GC+0x590..+0x5b4) in 0x4acc60 / 0x4acf00. The default win
  condition is chosen by the lobby / staging **game type** (the `IDC_GAME_TYPE_LIST` enum, UI.md §9.7; not the mode
  type) in 0x4acf00(options, type, keep): type 1 (campaign) leaves it, type 2 takes the land default (GC+0x5b8),
  type 4 forces 0x15, any other type the space default (GC+0x5bc). 0x4ad000(options, type) then steps an invalid
  value to the next valid one: type 2 accepts 0x12..0x14, type 3 0x12 / 0x16, type 4 only 0x15. [H] (ccpgt t01)
- Has-AI 0x2814d0 = player+0x360 non-null. 0x28b060 = GMM+0x18 == 3 and GMM+0x20 != 1. [H]
  0x140b15490 is a global that skips pre-built pads, the income tick and selects the MP buildable list;
  its meaning is not settled [L].

## 1. Player list and slot mapping (item 3)

- 0x2926c0 builds players from the lobby slots. Slots are bubble-sorted by slot+0x94 (faction index)
  when GMM+0x20 == 1, else by slot+0xa0 [M: meaning of +0xa0 not settled]. The first occupied non-AI
  slot (+0xc3 == 1, +0xc0 == 0) is created first, so it is player 0 and the local player; the rest follow
  in sorted order. [H]
- Per player from its slot: team +0x54 = slot+0x98, +0x394 = slot+0xa4, +0x484 = slot+0x138,
  +0x61 = slot+0xc4, is-AI slot +0x182 = slot+0xc0, AI difficulty +0x3bc = slot+0x158. [H]
- After the slots: one player per faction with Create_Player_In_Multiplayer_Games and not Is_Playable,
  in FACTIONS.XML order (Pirates, Neutral, Hostile, Sarlacc, Hutts in FoC). Two slots give 7 players with
  Neutral = id 3, which matches the Geonosis trace (human Empire 0, AI Rebel 1, Neutral 3). [H]

## 2. Start sequence (item 3)

Entry points: 0xeed80 single-player skirmish, 0xc9e10 MP lobby start, 0xbd380 debug/demo start. [H]
Order, all three alike [H]:
1. Map object owner remap: 0x294fa0(PlayerList, 0, 0), or 0x295cf0 for a team game (lobby record +0xb5).
2. 0x2a74f0(GOM, 1), mode vf 0x250(0), mode vf 0x248.
3. 0x291d40: EVERY player (Neutral and the other non-playable ones included) gets credits (+0x70, float32)
   set to the option credits.
4. For each player in list order: if its faction Is_Playable and option flag 0x2 is set, 0x288810 copies
   the faction's resolved Space_ (+0x2918) or Land_ (+0x2900) Skirmish_AI_Default_Forces list into the
   player's start/garrison force list (+0x2a8); if Is_Playable, 0x2a4fc0 places that force. Then, for
   every player, one victory-condition record {option win condition, player id, countdown 210 frames}
   is registered via 0x28a4a0.
5. Mode vf 0x20, 0x2b3e00, 0xcac70.

Correction (q6 RE, 2026-11-04): 0xeed80 does not touch the buy-units budget. GC Skirmish_Buy_Credits (GC+0x1184c =
0x140b2704c, 2000) is copied to 0x140a62214 / 0x140a62218 by the dialog's open function 0xeddd0, and only when no game
mode exists yet. The dialog, its list and where the bought list goes are in §2.7. [H] Faction
Land_ / Space_Skirmish_Unit_Buy_Credits (+0x28c4 / +0x28c8, 50000 / 100000) have no code reader [M].

### 2.1 Owner remap 0x294fa0 [H]
- Each map object's owner id is the map-time player, matched by faction to the first new player of that
  faction (log "remapped from default player ID %d to actual player ID %d").
- No player of that faction: a type with Is_Decoration (+0x3b) and not Is_Discardable (+0x3c) goes to the
  first Is_Neutral player; anything else is removed. An invalid owner id removes the object too.
- Then: player list swap; AI setup 0x2886d0 with the faction's Basic_AI (+0x378) for players with +0x182;
  a tactical build manager (0x4967a0, 0xb8 bytes) at player+0x3a0 for every player; 0x293d60 (space
  special weapons, §2.3); per player 0x288980 (tech) and 0x289010.

### 2.2 Starting forces 0x2a4fc0 [H]
- Collects GOM marker objects with a valid owner. Free-for-all: markers named `PLAYER_<k>_SPAWN_POINT_MARKER`
  whose owner's faction equals the player's, k = the player's index among players of that faction
  (0x294c60). Team game: the single `TEAM_<tt>_SPAWN_POINT_MARKER`, tt = team +0x54 (two digits), k from
  0x294cc0 (index within the team).
- The local player's camera focus (mode+0xc0) goes to the first marker; the player's facing record
  (mode+0x288, 12-byte records) takes the marker orientation.
- Each type in +0x2a8 is placed as a company by 0x29e960 at marker[i] with the marker orientation,
  registered for garrison tracking; i advances on success and wraps to 0. A failure prints an
  "insufficient credits" line, but no credit charge for starting forces was found [M].
- 0x29e960: land uses the type's Company_Units (+0x958/+0x960), space Squadron_Units (+0x978/+0x980);
  each member is created by 0x2b3be0, then grouped (0x2a5a60) and signal 0x1d is sent. With the
  registration flag the objects join the GOM per-player garrison list (GOM+0x550, stride 0x48). [H]

### 2.3 Space special weapons 0x293d60 (space mode only) [H]
For each player with a playable faction, Standalone_Space_Maps_Special_Weapon_A (+0x2b08) and _B
(+0x2b18), when the type index (+0x1de4) is valid, are spawned for that player (0x29f810) at the first
DUMMY_SPECIAL_WEAPON_SOURCE (behaviour 0x63) object's position and orientation, else at the map origin,
and registered with 0x35c520 (log "Setting space special weapon %d '%s' at object '%s' for player %S(%s)").
This is where Ground_Empire_Hypervelocity_Gun / Ground_Ion_Cannon in the space trace come from.

### 2.4 Garrison re-delivery [H]
- When an object leaves the GOM (0x2a6a00) and the player's garrison list becomes empty, 0x280c50 arms
  (skirmish only, pending list empty, timer not armed) a timer +0x2f0 = faction
  Garrison_Reinforcement_Delay_Seconds (+0x2ee4, 20 s), flag +0x2f8, plus a local UI message.
- Player tick 0x287e70: on expiry, with nothing pending (+0x2e8 == 0), the start force list +0x2a8 is
  copied to the pending list +0x2d8. In land each type is also appended to the reinforcement pool
  (+0x2c8), registered with the AI free store (0x4d31a0), message 0xe7 for the local player and the
  faction speech +0x22a0. In space the pending list is launched by the skirmish station hangar
  (STATION_HANGARS_SOUND §2.4 step 8). 0x287e70 also services the build manager every frame.

### 2.5 Map data that is NOT a start-state source [M]
- Map mini chunk 0x2B (placement parser 0x39ce40) sets object+0x300, the model ALT / damage-state index
  also written by the construction service 0x5d7350. Visual only, not a pre-build flag.
- MultiplayerStructureMarker types (Marker_For_Specific_Object_Type +0x20f8) are matched only in the
  campaign land placement 0x2a1eb0 (galactic structure's pad type +0x22f0 to marker). No skirmish reader.

### 2.6 Pre-built base pads [H]
Pad vf13 0x42df80 (skipped while 0x140b15490 is set; the campaign restore 0x42d8b0 runs only outside
skirmish): if the pad's data pack (obj+0x150) holds a pre-placed type (+0x18, map-saved) and the pad is
empty, and either not skirmish or (option flag 0x8 and the pad type's Respects_Skirmish_Pre_Build_Bases_Option
+0xe5), the type is created for the pad owner (+0x58) at the build bone (0x42e280) with the pad
orientation; an under-construction type is replaced by its Tactical_Buildable_Constructed (+0x9f0).
Then sell/garrison setup, +0x3cb copied, revealed to all players, +0x18 cleared. Otherwise pad anim 0x75.

### 2.7 The buy-units dialog IDD_SKIRMISH_BUY_UNITS_DIALOG (q6 RE, 2026-11-04) [H unless tagged]

Functions: open 0xeddd0(faction, is_space, game type), list build 0xf0600, list fill 0xef9f0, buy 0xf0920, sell one
0xf0c30 (also inline in the handler), clear 0xf0de0, message handler 0xee400, budget text 0xf03f0, close 0xee3c0.
State: list of candidate types 0x140a11d98 (count 0x140a11da0), bought list 0x140a11db0 (count 0x140a11db8), budget
0x140a62218, start budget 0x140a62214, faction 0x140a62220, is-space flag 0x140a62211, open flag 0x140a62210.

**Who opens it.**
- In this build the only openers are 0x36250 and 0x37760 (a command-bar callback, registered by 0x30ae50 from
  `CommandBarComponentFiles`; which button or key uses it is [L]). Both run only during a tactical battle that is a
  skirmish (0x28afb0), with the dialog closed and a local player present. Arguments: the local player's faction
  (player +0x68), is_space = (mode vf 0xe0 == 2), game type = GMM+0x18.
- No opener runs before a game mode exists. The "no mode" branches below (Skirmish_Buy_Credits budget, Sell / Sell All /
  Next) are therefore unreachable from the single-player skirmish setup in this build [M]. In the multiplayer lobby the
  bought list is copied into the local player's start-force list (§2 step 4, player +0x2a8) by 0xc9190 (sets player
  +0x183) and sent to the peers as type name CRCs (+0x11c) by 0xc8d80. Multiplayer is out of scope for the port.
- So the single-player skirmish has no pre-battle purchase in the original. The start force is the faction's
  Space_ / Land_Skirmish_AI_Default_Forces when option flag 0x2 is set (§2 step 4), and nothing otherwise.

**Two modes.**

| | no game mode (pre-game branch) | in a tactical skirmish (the reachable one) |
|---|---|---|
| budget | GC Skirmish_Buy_Credits (2000), also kept as the start budget | the local player's credits (player +0x70, float truncated to int), re-read on message 0xc |
| buttons | Buy, Sell (`IDC_SELL_BUTTON`), Sell All, Next, Back | Sell, Sell All and Next are hidden; Buy is relabelled `TEXT_BUTTON_REINFORCE` |
| Buy (0xf0920) | cost ≤ budget; budget −= cost; the type is appended to the bought list (duplicates allowed) | cost ≤ budget; then 0x33d50(input controller 0x140a60df0, type +0xec) starts a reinforcement placement for that type and the dialog closes. The dialog itself charges nothing [M: whether the placement charges] |
| Sell (0xf0c30) | the selected type must be in the bought list; refund its cost; remove one entry (0x474a0) | — |
| Sell All (0xf0de0) | clear the bought list; budget := start budget | — |
| Next | only when game type == 3: 0xef700 starts the map (0x8d930) and the dialog closes | — |
| Back / close | close; with no mode, back to the setup screen 0xf1230 | close |

A refused action plays the GUI error sound (0x2d5290). Every change refreshes the list (0xef9f0) and the budget text
(`TEXT_SKIRMISH_BUY_UNITS_CREDITS_BALANCE` + the formatted budget, in `IDC_SKIRMISH_BUY_UNITS_CREDITS_TEXT`).

**Candidate list 0xf0600** (rebuilt on every open). All object types in type-manager order (0x140b2f020, index
0..N−1) that pass all of:
1. `Build_Cost_Credits` (+0x86c) > 0 (0x3711c0 with no planet returns this field).
2. affiliated with the dialog faction (0x3751a0: the faction is in the type's `Affiliation` list +0xef0; an empty list
   never matches);
3. `Available_In_Skirmish` (+0x77) **or** the build tab of the map kind: `Build_Tab_Space_Units` (+0x4d) on a space
   map, `Build_Tab_Land_Units` (+0x4c) on a land map;
4. not `Cinematic_Object_Only` (+0x21).

Nothing else is tested: no tech level, `Build_Initially_Locked`, prerequisites, `Build_Limit_*`, unit cap or
population, `Is_Dummy`, behaviours (DUMMY_*), XML class, the faction's `Skirmish_*` lists or any pad
`Tactical_Buildable_Objects_*` list. Note the OR in step 3: a type with `Available_In_Skirmish` is listed on both map
kinds (FoC: `Darth_Team_Executor`, a named hero, is a candidate in the Empire land list too).

**Rows 0xef9f0.** For each candidate in list order, the same tests 1–3 again, and a type with `Is_Named_Hero` (+0x13)
is hidden unless the heroes option bit (0x140b154bc & 1) is set. The row key is the candidate's index, so the
selection still maps to the candidate list.
- Columns: the localized `Text_ID` (+0x120); when the type is in the bought list n > 0 times, a column "x n"; the
  cost (0x22f780, formatted integer). There is **no population or unit-cap column**.
- Colours (GUI palette 0x140a7bca0): bought rows palette 5; not bought and cost > budget palette 9; else palette 1 /
  4 [M: which is normal and which is highlight].
- **Order:** after the fill the listbox is sorted (0x1c4a10 → 0x1d1c90) with comparator 0xf0e80, which returns
  `b.data − a.data` (code bytes `8b 42 04 2b 41 04 c3`). The row data is the cost passed to the row insert (0x1c3dd0),
  so the list shows the **most expensive first** [M: that row field +4 is the cost]. The sort is an introsort, not
  stable: equal costs come in no defined order.
- No cap and no paging: every candidate gets a row; the listbox scrolls.

**Buy limits 0xf0920.** cost ≤ budget. `Can_Be_Only_One` (+0x78): in the pre-game branch a type already in the bought
list is refused; in a battle a type of which the local player already owns an object (0x2a87d0, owner +0x58) is
refused, where for a DUMMY_GROUND_COMPANY (0x5D) with `Company_Units` the type looked up is the company's second
member (+0x958 entry 1) [M].

**What the filter gives on shipped data** (evaluated with the XML, 2026-11-04; `Variant_Of_Existing_Type` resolved).
- FoC: Empire space 23 rows (20 without named heroes), land 16; Rebel 17 / 9; Underworld 20 / 23. They include story
  and campaign types that happen to carry a build tab and a cost: `Executor_Super_Star_Destroyer` and its
  `_No_Tractor_Beam` copy, `Eclipse_Super_Star_Destroyer` (all three factions), `UM12_SUPER_STAR_DESTROYER`,
  `Keldalbe_Prototype`, `UM06_Prisoner_*`, `Tartan_Patrol_Cruiser_Easy`, `Star_Destroyer_E3`. Only 9–14 of each list
  are named in some type's `Tactical_Buildable_Objects_Multiplayer`.
- Thrawn's Revenge (workshop 1125571106): Empire space 407 rows (138 of class DummyStructure), land 63; Rebel space 484
  (183 DummyStructure), land 45; Pentastar space 354, land 51. The original **would list** these marker types:
  - `Template_Option`, `Option_Cruel_On/Off`, `Option_Factional_Fighters`, `Option_Canon_Units`, `Option_FS_Units`,
    `Template_Cheat`, `Cheat_Disable_AI`, `Cheat_Give_Credits`, `Cheat_Give_Unit`, `Cheat_Give_Vision`, `Cheat_Government`,
    `Cheat_Influence`, `Cheat_Infra_And_Discount`, `Cheat_Kill_All`, `Cheat_Turn_Off_Crews`, `Cheat_Victory`
    (Options.xml / Options_TR.xml: cost 1, `Build_Tab_Space_Units` yes, DUMMY_STARSHIP, every playable TR faction):
    space lists of all those factions;
  - `ZLayer_Medium`, `ZLayer_High1..4`, `ZLayer_Low1..4` (LayerDummies.xml: cost 1250, Rebel only, space tab,
    DUMMY_STARSHIP + POWERED): the Rebel space list;
  - `Dummy_Treaty_Victory` (cost 1, Empire and Rebel, space tab): those two space lists;
  - `Cheat_Space_Unit` (cost 1, Rebel / Empire / Underworld / Hutt_Cartels): their space lists;
  - the galactic ground-war formations `GW_*` / `UNIT_GW_*` (cost 1, space tab): the space lists;
  - shipyard / Golan / mining-facility orbital structures with a cost and the space tab.
- TR marker types the original would **not** list: `Victory_Point` (no cost), `Cheat_Dummy_Vision`,
  `Options_Category_Dummy`, `Cheat_Dummy_Infra_And_Discount`, `Template_Cheat_Dummy_Infra_And_Discount` (cost 0),
  `Cheat_Team`, `Cheat_Ground_Unit`, `Dark_Empire_Cheat_Choice_*` (no `Affiliation`), `NR_Canon_Option` (no cost).

**Port notes**
- The port's pre-battle buy screen (`engine/apps/game/app/app_screens.cpp`, `open_buy_units`) is a [port] feature: the
  original single-player skirmish has none. Its budget, its use of the bought list as the start force and the screen
  itself stay port decisions (kept by Gabriel 2026-11-04; PHASE4 Decisions).
- To match the original's list, replace the port's filter (unit kind from `galactic_unit_kind`, first 24 by type index,
  ascending cost) with steps 1–4 above plus the named-hero rule; no cap; order by cost descending. The original's tie
  order is undefined; use type index ascending for equal costs [port].
- An empty `Affiliation` must not match (the port's "empty = every faction" is wrong for this list).
- Steps 1–4 alone fill a TR list with hundreds of option, cheat, ZLayer and ground-war marker rows, exactly as the
  original would. A [port] extra rule that removes all of them and keeps real units: list a type only if it is named in
  some type's `Tactical_Buildable_Objects_Multiplayer` (TR: Empire 106 of 407 space and 38 of 63 land rows remain, every
  Option_* / Cheat_* / ZLayer_* / Dummy_Treaty_Victory row is gone; FoC: 9–14 rows per list, the story props are gone).
  Say [port] where it is applied.
- Cost column = `Build_Cost_Credits` (not the tactical cost); no population column.

## 3. Tactical production (item 1)

Build manager at player+0x3a0 (ctor 0x4967a0): queues at +0 and +0x18 (queue index = type
Tactical_Production_Queue +0xa30 minus 2, so only 2 and 3 are valid), player id +0x30, build-time
modifier map +0x38, price modifier map +0x78. [H]

Enqueue 0x496a20(manager, type, builder, queue) [H]:
- Check 0x497140: queue 2 or 3; builder friendly; in land the builder is not ion-stunned
  (behaviour 0x45, 0x4c3700); the type is in the builder type's buildable list for the builder owner's
  faction (§7); Build_Limit fields +0x880..+0x88c counting queued entries; price and time finite and
  greater than zero; prerequisites (+0xa18) owned.
- A human is limited to fewer than GC Max_Build_Queue entries (0x140b26f78, default 5); AI is unlimited.
- Pay-as-you-go test 0x289050: an AI player always counts as PAYG; a human uses GC Pay_As_You_Go
  (+0x320, false in FoC). Not PAYG (FoC humans): fail if credits < cost, else deduct the full cost at
  enqueue. PAYG: no charge at enqueue. [H]
- Entry (ObjectUnderConstruction): type, total time, timer, remaining, player, queue, cost, builder id
  (0x3fffff if none). Local player: queue SFX (type +0x1490 / +0x1450) or speech (+0x1570).

Build time 0x496f00, in seconds [H]: Tactical_Build_Time_Seconds (+0xa00) times GC
Tactical_Build_Time_Multiplier (0x140b26f34, 1.0); times the AI difficulty multiplier for non-human
players with an AI (difficulty record via 0x4b0500: +0x2c space, +0x30 land); times (1 minus the sum of
build-time modifiers for the type); times the faction corruption factor 0x33e4a0 in campaign battles only.
The cheat flag player+0x3c0 forces 5.0. Builder object modifiers (+0xf0 then +0x138) scale it by
(1 + their sum) at enqueue.

Cost 0x496d80 [H]: 0x3743f0 picks Tactical_Build_Cost_Multiplayer (+0x9fc) in skirmish, else
_Campaign (+0x9f8) falling back to the MP value when below 1; times (1 minus the sum of price modifiers);
times the corruption factor 0x33e470 in campaign battles only.

Service 0x498170, every frame, both queues [H]:
- Entries are re-validated last to first (0x497140, counting themselves); a failure cancels (0x498820).
  The refund on that auto-cancel appears to apply only to non-human players [M].
- Head entry: for an AI player (has AI, not human) or a non-PAYG player the timer just advances
  (0x499070) with no payment. Only a PAYG human pays (time slice / total) x cost per frame (0x498ec0) and
  stalls when unaffordable (0x499090). Net effect: the AI is never charged credits for tactical builds,
  consistent with free AI pad builds in L1_S4 §3.6. [H]
- Completion (0x498f70): 0x2821b0(player, type, 1); signal 0x22; local SFX (+0x14a0 / +0x1460) or speech
  (+0x1598).
  - Not a DUMMY_UPGRADE (0x65): 0x27fad0 appends the type to the reinforcement pool +0x2c8, registers it
    with the AI free store (0x4d31a0), message 0xe7. Same for land and space.
  - DUMMY_UPGRADE: spawned at the builder (0x29f810) and attached to it (0x379d30; galactic persistence
    0x4b7090).
  - Type flag +0xc8 with list +0xa60: allied objects of the listed types are killed. Flag +0xc5: allied
    same-faction players' tech becomes this player's tech + 1 (0x288980).
  - The head pops; the next head's timer restarts at its full time.
- Cancel 0x498820 refund (0x499000): full cost when not PAYG, else the paid fraction. Teardown
  0x496830 / 0x4986a0 refunds everything.

Where land units appear [H]: nowhere directly. A bought land unit (human or AI) goes into the reinforcement
pool and needs a Reinforce drop (human ReinforceEvent; AI TaskForce Reinforce in skirmish 0x6ee010).

AI Produce_Force [H]: the AI plan path 0x648980, in skirmish with AllowFreeStoreUnits false, calls 0x496a20
for each builder in the free store (+0x120). 0x6c1e80 only lowers the plan's budget field (+0x1a4), not
credits. With the PAYG rules above the AI's tactical production costs it nothing; only queue, limits,
prerequisites and build time constrain it.

Not traced [L]: GC Skirmish_Reinforcement_Delay_Frames (GC+0x117e8, default 90) is read near
0x1211656 and 0x894995, in what looks like an ability / countdown context.

### 3.1 What a finished land company becomes: pool, landing, members, AI use (q6 t19 RE, 2026-11-04) [H unless tagged]

Question: how the original creates the members of a land skirmish company that has no team (one member without
`Create_Team`, e.g. `Boba_Fett_Team_Land_MP`: `Variant_Of_Existing_Type` `Boba_Fett_Team`, a HeroCompany with
`DUMMY_GROUND_COMPANY`, `Company_Units` = `Boba_Fett`, `Company_Transport_Unit` = `Slave_I`, whose
`Landing_Transport_Variant` is `Slave_I_Landing`). Sources: 0x27fad0, 0x4d31a0, 0x6ee010, 0x2b5fe0, 0x644330,
0x6438a0, 0x690a10, A1_READINGS §35, AI_GALACTIC_3 §1.3.

1. **Production never spawns a land unit.** Completion of a non-`DUMMY_UPGRADE` type calls 0x27fad0(player, type):
   the type is appended to the player's reinforcement table (player +0x2c8, count +0x2d0); when the player has an AI
   for the current mode, 0x4d31a0 appends one empty reservation slot to the AI free store's vector +0x88 (same index
   as the pool entry); the local player gets message 0xe7. There is no code path that places a built land company
   at or in front of its builder. Company or single unit, team or no team, makes no difference here.
2. **It reaches the map only by a reinforcement drop.** Human: `ReinforceEvent` at a valid drop point. AI: a plan's
   `TF.Reinforce` (skirmish 0x6ee010, AI_TACTICAL §7.4) for a pool entry that the Reinforcement marshaller assigned to
   the plan (AI_GALACTIC_3 §1.3: pool entries count as candidates of every AllowFreeStoreUnits plan; free store
   +0x88[i] non-null = already reserved). The build plan that bought it (`TacticalMultiplayerBuildLandUnitsGeneric`,
   `ReserveForce`, AllowFreeStoreUnits false) only calls `Produce_Force` and exits; it never drops the units.
3. **Landing object** (0x2b5fe0, skirmish branch, A1_READINGS §35.1): the pool type T must be in the pool; the landing
   type L = T's `Company_Transport_Unit[0]` (+0x938 → +8), replaced by its `Landing_Transport_Variant` (+0x1e68) when
   set (here `Slave_I` → `Slave_I_Landing`). **T without a `Company_Transport_Unit` spawns nothing** (0x2b5fe0
   returns null; the TF entry is not added). L gets TRANSPORT_LANDING and carries T (TransportDataPack +0x48).
4. **Members** (0x644330, one step every 2 s × (1 + deploy modifier)): for a member type **without `Create_Team`**
   (the "no team" case) the vehicle branch runs: **one object of the member type** (with `Unique_Ground_Unit`, the
   substitute first and the member riding it) is created at the landing position, owned by the landing's owner,
   registered with the player (pack +0xbc, 0x2b5f70). It gets no TEAM container; obj +0x2b0 stays null. If it has a
   locomotor for the mode (and type byte +0xd4 [M]), it is sent on a coordinator move to its unload offset with the
   forced-move flag and **movement-locked**; otherwise it is placed at the offset at once.
5. **Mobility.** The unload finish (0x6438a0, 2 s after the last step) unlocks every unloaded unit, clears the
   forced-move flag, sets its locomotor pack +0x38 := 1 and raises signal 0x19 (arrival). From then on the member is
   an ordinary mobile unit with its own locomotor; nothing in the company type keeps it in place. (Boba Fett has
   `WALK_LOCOMOTOR` plus a `JETPACK_LOCOMOTOR` secondary.)
6. **AI admission.** An AI drop's units join the requesting task force on signal 0x19 (AI_TACTICAL §7.4). When that
   plan ends (`QuickReinforce` → `WaitForAllReinforcements` → `ScriptExit`, or any later release), they go to the free
   store through the land admission 0x690a10 (A1_READINGS §6.2). A single no-team member passes it: not `Create_Team`,
   no TRANSPORT, it has the implicit `STARSHIP_COMBATANT` behaviour (OBJECT_TYPES §9) and a locomotor, it is not
   `Is_Dummy` (the company type is, the member is not), no container (+0x2b0 null). So it is a normal free-store unit
   [H]. Whether the GOM add hook offers it to the free store already while it is still locked inside the unload, before
   0x19 moves it to the task force, was not traced [M].

**Port notes**
- Do not create a finished land company at its builder. Append the type to the player's reinforcement pool (and an
  empty AI reservation slot) and let the human / AI `Reinforce` drop it through the landing object (A1_READINGS
  §35.3). The "Port status" row "a finished land company appears 120 units in front of its builder" is a [port]
  deviation; its stand-in fix that makes no-team members movable (tactical_battle_hangar.cpp, q6 land end chain) is
  only needed while that deviation exists.
- A company member without `Create_Team` must become a single movable unit without a TEAM container, locked during
  its exit move and unlocked at the unload finish; then it is admitted to the AI free store like any unit.
- A pool type without `Company_Transport_Unit` cannot be dropped in skirmish (no landing object, no members).
- The AI uses bought pool entries only through the Reinforcement marshaller of a later plan; the buying plan does not
  drop them.
- Port status (q6 t19, 2026-11-04): done for land. A finished land company becomes a pool entry
  (`TacticalBattle::add_built_reserve`, key 0x01000000 + participant index, no galactic counterpart); the producing
  TF's entry is cleared (`production_completed`) and a later plan drops it through `order_reinforce` and the landing
  object (locked until the 0x19 unload finish). A skirmish type without a landing object is refused and stays pooled.
  The hangar no-team stand-in stays only for space hangar launches; space production still spawns at the builder
  [port].

## 4. Land skirmish victory, types 0x12..0x16 (item 2)

Names (0x341ff0): 0x12 SKIRMISH_ALL_ENEMY_UNITS_DESTROYED, 0x13 ALL_ENEMY_BASE_COMPONENTS_DESTROYED,
0x14 ENEMY_COMMAND_HQ_DESTROYED, 0x15 CONTROL_POINT_DOMINATION, 0x16 SPACE_ENEMY_STARBASE_DESTROYED. [H]

Registration: one record per player at start (§2 step 4) with the option's win condition and a 210-frame
declaration countdown. [H]

When tested [H]: 0x2c0340(GOM, object, old owner) runs when a victory-relevant object leaves player P
(death or owner change). Without a parent mode, for each enemy E of P: test 0x12; then if the object
is a DUMMY_LAND_BASE_LEVEL_COMPONENT (behaviour 0x62, slot +0x394) test 0x13 and 0x14; else if it is
a DUMMY_STAR_BASE (0x5C, slot +0x38e) test 0x16. With a parent mode (campaign) only attacker/defender and
types 0xb, 0xd/0xe or 0xf are tested.

Evaluator 0x344710 (E wins when no blocker is left) [H unless marked]:
- 0x12: any object enemy to E (0x39a450), Victory_Relevant (0x372230: type +0x5e and the mode-specific
  +0x5f space / +0x60 land) and alive (+0x3a0 bits 0x42 clear). In skirmish or game type 3 only objects
  whose owner is human or has an AI count, so Neutral / Hostile objects never block.
- 0x13: behaviour 0x62 objects enemy to E, Victory_Relevant, alive.
- 0x14: behaviour 0x5F (DUMMY_GROUND_STRUCTURE) objects enemy to E with HQ_Win_Condition_Relevant (+0x61),
  alive. No Victory_Relevant test.
- 0x15 (ccpgt t01) [H]: no win unless E's faction is `Is_Playable` (+0x20a8). Candidates: every CAPTURE_POINT
  (0x3d) object of the GOM's full list (gom +0x38, any owner, E included) whose type has
  Control_Point_Domination_Condition_Relevant (+0x62). No Victory_Relevant, alive / dying (+0x3a0) or behaviour test.
  None found: no win. Else E wins when **every** candidate is allied with E (0x39a2c0: relation(E → object owner
  +0x58) == 0, so E itself or a team ally). A Neutral- or Hostile-owned point (relation 2) blocks like an enemy one.
  Symmetric for humans and AIs (no "if E is not human" clause as in 0x16). Trigger and timer: §4.2.
- 0x16 (and campaign 0xf), space only: DUMMY_STAR_BASE objects enemy to E, ignoring dead (0x40), flag
  0x80, non-Victory_Relevant and non-playable-owner ones. None left: E wins. If E is not human, E wins
  only when no remaining starbase is allied (0x39a2c0) with a human player.
- So "base components" = objects with behaviour DUMMY_LAND_BASE_LEVEL_COMPONENT that are Victory_Relevant.

### 4.1 What else counts, and what does not (q6 RE, 2026-11-04) [H unless tagged]

Re-read of 0x344710 cases 0x12–0x14 and of the readers in OBJECT_SCRIPTS §10. Additions to the list above:
- **Candidate list:** 0x12 walks the tactical GOM's full object list (gom +0x38) skipping objects owned by E
  (0x2A9FF0 with exclude-owner = E); 0x13 / 0x14 walk the same list filtered by behaviour 0x62 / 0x5F. Ownership
  is the object's own owner (+0x58); a TEAM container and its members are separate objects, each counted by its
  own type (containers' `Team` types are not Victory_Relevant in FoC data, so the members count).
- **The 0x12 owner filter:** in skirmish (0x28AFB0) or game type 3 (0x28B060), an enemy object counts only when its
  owner player is human (player +0x62 [M: name]) or has an AI (0x2814D0), or the owner id resolves to no player.
  Outside those two session types any enemy owner counts.
- **Not tested by any of 0x12 / 0x13 / 0x14:** INVULNERABLE (`Make_Invulnerable`), `Hide` (model / obj +0x3A2 bit 0),
  `Prevent_AI_Usage` (free-store membership only), `Set_Selectable`, garrisoned / transported state, stealth, fog,
  locomotor / UNIT_AI / SELECTABLE behaviours. Such objects count exactly like visible ones. An invulnerable
  Victory_Relevant object therefore keeps its side alive until it dies through damage kind 0 or 13, `Kill`, or a
  despawn (OBJECT_SCRIPTS §10.2).
- **Structures under construction:** the object on the pad is the separate UC type (`UC_*`, L1_S4_REMAINING §3.6),
  replaced by the final type at completion. It counts only by the UC type's own flags. Shipped FoC UC types set
  `Victory_Relevant No` or nothing (default false) and none has DUMMY_LAND_BASE_LEVEL_COMPONENT, so a structure under
  construction never blocks 0x12 or 0x13. The finished structure counts by its final type (e.g. the command centres:
  DUMMY_LAND_BASE_LEVEL_COMPONENT + DUMMY_GROUND_STRUCTURE, `HQ_Win_Condition_Relevant yes` for 0x14).
- **Dying objects** (+0x3A0 bit 0x40 or 0x02) never count, so the test run from the death of the last blocker
  already sees the side as empty.
- **When:** only from 0x2C0340, i.e. when a Victory_Relevant object leaves its owner (removal from the GOM or owner
  change). Gaining objects (a script `Spawn_Unit`, a reinforcement landing, a capture) never triggers a test, and a side
  that never had a Victory_Relevant object is never tested (BATTLE_TRANSITIONS §3.1a item 2). The declared result then
  waits the record's 210-frame countdown (and any hold) before the battle ends.
- Reinforcement pool / reserve units are not tactical objects and never count (A1_READINGS §34.1); a landing transport
  counts by its own type only [M].

**Port notes**
- The land-skirmish check must ignore every script flag (invulnerable, hidden, AI usage, selectable); count by type
  flags, ownership (with the skirmish human-or-AI owner filter for 0x12), and not dying.
- UC objects count by their UC type (all non-relevant in FoC data); do not map them to the final type.
- Run the check only when a Victory_Relevant object leaves a player (death / removal / owner change), not on spawns,
  and apply the 210-frame declaration countdown.
- The win condition that is registered is the lobby option's (§2 step 4): 0x12 alone, or 0x13 / 0x14 when the base /
  HQ option is chosen; 0x12 is still evaluated first on every trigger. (ccpgt t01: 0x344710 only looks at records
  whose {type, player} equals the request, and 0xeed80 registers exactly one record per player, of the option's type.
  So a test of a type that was not registered answers "no": in a 0x13 / 0x14 / 0x15 game the 0x12 test never wins.)

### 4.2 Control point domination (ccpgt t01, 2026-11-08) [H unless tagged]

Sources: 0x344710 case 0x15, 0x360d90 (arm), 0x3639d0 (tactical tick), 0x3611f0 (announcer), 0x500e30 / 0x501980
(capture service), 0x2c0340, 0xeed80, 0x3464b0, 0x39a2c0 / 0x2823e0; FoC MARKERS.XML, GAMECONSTANTS.XML, FACTIONS.XML.
- **Relevant objects (data):** only `Reinforcement_Point` (MARKERS.XML: CAPTURE_POINT, REINFORCEMENT_POINT, transition
  10 s, radius 100) sets `Control_Point_Domination_Condition_Relevant Yes`; its six `Reinforcement_Point_PlusN_Cap`
  variants inherit it. Default false: build pads (also CAPTURE_POINT) never count, and the faction-fixed
  `Reinforcement_Point_Rebel / _Empire / _Underworld` have neither CAPTURE_POINT nor the flag. Map owner of the
  shipped points not read [M: Neutral by the §2.1 remap].
- **Not tested from object loss.** 0x2c0340 never tests 0x15, and its 0x12 / 0x13 / 0x14 / 0x16 tests find no record in
  a 0x15 game (§4.1 last port note). **Elimination cannot win a 0x15 game**, and a map without a relevant point never
  ends by victory.
- **Arm / disarm** (0x360d90(mode, point, old owner, new owner)): its only caller is the capture service 0x501980 via
  0x500e30, when a capture transition **completes**, before the owner is written (0x38e5a0). Ignored unless the mode is
  tactical, the monitor exists, the point's type has +0x62 and some player has a {0x15, player} record (0x3464b0).
  Then: new owner's faction not `Is_Playable` (a point turning Neutral) → disarm. Else collect the relevant points as in
  0x344710 and skip the changing point itself; if every other one is allied with the new owner (0x39a2c0) → arm:
  mode +0x2b0 := 1, +0x2b4 := round-half-away(FPS × `Control_Point_Domination_Victory_Time_In_Secs`, GC+0x11b30, 30.0
  in FoC) = **900 frames**, +0x2b8 := new owner id, announce once; otherwise disarm (+0x2b0 := 0, +0x2b4 / +0x2b8 := −1).
  An enemy capture always passes through Neutral (L1_S4_REMAINING §4.2), so it disarms on its first transition.
  Owner changes outside the capture service (Lua `Change_Owner`, 0x501050 resets) do not re-check [M: from the single
  caller of 0x500e30].
- **Countdown** (tactical tick 0x3639d0, every tick, not gated by 0x140b15490): while +0x2b0 is set, +0x2b4 −= 1, then
  the announcer; when +0x2b4 ≤ 0, the monitor exists and no result is ready (0x342dc0), test 0x15 for player +0x2b8
  (0x346460, hold → queued as usual) and disarm. The test re-checks every relevant point (now with the new owner).
  A pass declares with the record's 210-frame countdown, so the battle ends ~900 + 210 frames after the last capture.
  [M: ±1 frame for the order of the capture service and the mode tick in one frame.]
- **Announcer** 0x3611f0 (presentation): when +0x2b4 equals 60 / 30 / 15 / 5 s × FPS, the local player's faction plays
  `SFXEvent_HUD_Reinforcement_Point_Owned_*` (local player dominates), `_Ally_Owned_*` (relation 0) or `_Enemy_Owned_*`
  (relation 1) for that mark; Neutral observers hear nothing. With 30 s the lines are 30 s (at arming), 15 s, 5 s.
- Shipped `_MP_LAND_*` mini 0A tags (effective file after the patches; UI.md §9.4): `control` in any case on 18 of 24
  (Bespin, Bothawui, Corellia, Dagobah, Dantooine, Endor, Felucia, Geonosis, Honoghr, Hoth, Hypori, Kamino, Kessel,
  Mandalore, Naboo, Saleucami, Tatooine, Yavin); empty or blank on Alderaan, Dathomir, Polus, Ryloth, Shola, Utapau.
  Which maps place `Reinforcement_Point` objects was not checked [M].
- Mode +0x2b0..+0x2b8 in saves not read [L].

**Port notes**
- 0x15: count CAPTURE_POINT objects whose type has the flag, any owner, no alive / Victory_Relevant / script-flag test;
  win for E only if E's faction is playable, at least one point exists and every point is relation 0 to E.
- Run the check only from the domination timer, never from object loss; register only {0x15, player} records, so no
  elimination win. Arm / disarm on each completed capture transition of a relevant point (the changing point is
  skipped, it is about to become the new owner's), timer round(FPS × GC time) = 900, minus 1 per tick, test at ≤ 0,
  then the usual 210-frame declaration countdown. Keep the timer state in the battle state (sim, CRC-relevant).
- The announcer is presentation only.
- Port status (ccpgt t02, 2026-11-08): `BattleConfig::control_point_domination` (alamo_game: the staging screen's Capture
  Control Points row on a land map; alamo_headless `--skirmish-ai foc-land|skirmish --control-points`) registers only
  {0x15, player} (skirmish slots; [port] attacker and defender in alamo_game's hop battle), arms / disarms on each completed
  capture event, counts down per battle tick in the victory check and tests 0x15 at <= 0 (`tactical_battle_victory.cpp`);
  the timer is in the battle hash (while armed) and the battle save (state version 9). `TacticalBattle::control_point_timer()` /
  `control_point_leader()` expose the timer for the host.
- Port status (ccpgu, 2026-11-12): alamo_game plays the announcer (`GameAudioHost::control_point_line`, called from
  `battle_tick`): the leader's relation to the local player picks `_Owned_` (the local player leads), `_Ally_Owned_` (relation
  0) or `_Enemy_Owned_` (relation 1); a line plays on the tick the timer reaches 1800 / 900 / 450 / 150 frames and on an arm
  or re-arm at that value (the shipped 900-frame timer gives the 30 s line at arming, then 15 s and 5 s); a Neutral observer
  hears nothing. Test `alamo_game_audio_host_tests`.
- AI in a type-4 game (ccpgu t03, 2026-11-12): `Game.IsLandControlGame` (AI_GALACTIC_TOKENS 0x6e2060: GameModeManager +0x20 ==
  4) was never set in the port, so the skirmish AI ignored the points. [port] it is true in a land skirmish with
  `control_point_domination` (`BattleAi::fill`). The shipped data has no other type-4 rule: `May_Lose_Land_Control_Game` (=
  owns no Reinforcement_Point and IsLandControlGame) doubles `Should_Secure_Victory_Control_Point` (goal
  Secure_Victory_Control_Point, plan SecureVictoryControlPoint.lua: Produce_Force infantry + vehicle / hero escort, Move_To,
  Guard_Target), forces `Should_Build_Infantry_Emergency` and blocks unit / structure builds, tech and early upgrades and
  bunker garrisons until a point is held. Port run `alamo_headless --skirmish-ai foc-land --map _MP_LAND_ENDOR
  --control-points --ticks 12000`: 0 SecureVictoryControlPoint plans before the fix; after it both AIs start them (first at
  frame 499; 29 ended, 5 in success) and each holds one or two of the five Reinforcement_Points through the run. Test
  `test_battle_ai_land_mode` (alamo_sim_battles_ai_tests).

## 5. Income streams (item 4)

- The tactical income manager (mode+0x298) exists only in skirmish: created in 0x35e560 / 0x35f970 when
  0x28afb0 holds. Command-centre streams therefore never pay in campaign. [H]
- Income_Stream_Ability vf30 0x703990 creates a stream (0x537840): Base_Income_Value at +0x10; interval
  0x6953c0(Base_Interval_In_Secs) into +0x58 and the first stats frame +0x80; split mode +0x84
  (1 Split_Favors_Owner with owner percent +0x88, 2 Full_Amount_To_Everyone, 0 equal shares). Recipients
  (0x7038b0): the owner, or with Split_Income_With_Allies every allied playable player. Default
  Activation_Style 2 (Combat_Automatic). [H]
- Tick: 0x537ab0 from the tactical tick 0x3639d0 (not while 0x140b15490), then 0x6951a0 per stream per
  frame (skipped when 0x35f790, meaning not settled [L]). Per frame each stream pays
  ((1 + sum of multiplier mods) x base + sum of additive mods) / max(1, (1 + sum of interval mods) x
  base interval) / FPS, distributed by 0x694ba0; each recipient gets it through 0x27f370 (credits),
  the +0x7c income stat and a per-player accumulator (+0xc0). Once per interval a stats snapshot goes to
  +0xa8 and +0x80 advances. Payment is per frame, not a lump per interval. [H]
- Trace check (_MP_SPACE_GEONOSIS, starbase level 1 30/10 s plus supply-dock add 20): (30 + 20) / 10 / 30 =
  0.1666667 per frame. Added to a float32 near 6000 (ulp 2^-11) each step rounds to 0.16650391;
  x 1800 frames = 299.707, giving 6299.7, 6599.4, 6899.1 exactly as traced. Above 8192 the step would round
  to 0.16699. [H]
- Income_Stream_Mod_Ability (0x703fe0 / 0x7046e0): vf3 0x7045e0 returns 5 s x FPS unless
  Reverse_Application_Logic. How the mod finds its target stream (Target_Stream_Source / hard point) was not
  traced [M].
- XML values (for reference): Imperial/Rebel_Command_Center 45 per 5 s, split, full amount to everyone;
  Empire/Rebel_Mineral_Processor 75 per 5 s and a x2 mod on the Ground_Mining_Facility; *_Ground_Mining_Facility
  125 per 5 s; power generators x0.8 on the mining facility; starbase levels 30/10, 35/12, 40/14, 45/16,
  55/18 with supply-dock adds 20/25/30/35/45.

## 6. Land unit cap (item 5) [H]

- 0x2ac5c0: in skirmish land the cap is min(player population capacity +0x404,
  integer part of the faction's Land_Skirmish_Unit_Cap_By_Player_Count interpolator (+0x2cd8) at N).
  Space uses faction Space_Tactical_Unit_Cap (+0x2cd0); campaign land uses Max_Ground_Forces_On_Planet etc.
- N (0x294af0) = number of players that are human or have an AI, and have +0x180 set [M: +0x180 read as
  "active"]. Neutral and other non-playable players do not count.
- Interpolator vf2 0x2c8660: points sorted by x on first use; piecewise linear between points; x below the
  first point or above the last gives 0; fewer than two points gives 0.
- FoC data: N = 2 gives 9.29, cap 9; N = 4 gives 7; N = 8 gives 5; N > 8 gives 0.

## 7. Lua build list and Find_Nearest (item 6) [H]

- Buildable list 0x372c10 returns Tactical_Buildable_Objects_Multiplayer (+0x9d0) when skirmish (0x28afb0)
  or 0x140b15490, else _Campaign (+0x9b8). The same list gates the enqueue check in §3.
- Find_Nearest considers only UNIT_AI objects from the perception grid lists (see LUA_API.md), so it never
  returns particles, markers or projectiles.

## 8. Capturing a neutral build pad (item 7) [H]

Already specified in `L1_S4_REMAINING.md` §4.2. Summary: no start delay in skirmish; a neutral pad goes to
the owner of the first in-radius candidate unless contested; progress 1 / (5 s x 30) per frame, owner
changes at completion (150 frames). A pad with contents keeps its owner. An empty owned pad with an enemy
present goes through neutral first (2 x 5 s). When the pad's child dies the pad resets to neutral at once (no
transition); a sold child leaves the pad with its owner. Nobody can build on a Neutral-owned pad (the builder must be
allied with the pad owner), and the structure belongs to its builder, not to the pad: AI_TACTICAL §7.5.1 (q6 t19).

## 9. Not settled

- Reader and use of Skirmish_Reinforcement_Delay_Frames (GC+0x117e8). [L]
- ~~The pre-game buy-units dialog: what it buys and how the 2000 budget is spent.~~ §2.7 (q6, 2026-11-04). Still open:
  which command-bar button / key opens it in battle (0x37760 / 0x36250) [L], and whether the reinforcement placement it
  starts charges credits [M].
- ~~Body of victory type 0x15 (control point domination).~~ §4 / §4.2 (ccpgt t01, 2026-11-08). Still open: which
  shipped maps place `Reinforcement_Point` objects and their start owner [M]; whether the domination timer is saved [L].
- Meaning of player+0x180, global 0x140b15490 and predicate 0x35f790. [L]
- Sort key slot+0xa0 for slots when GMM+0x20 != 1. [M]
- Whether retail space maps carry PLAYER_<k>_SPAWN_POINT_MARKER objects: the two fighter squadrons per side
  in the trace may be the start force or the station garrison. [M]
- How an Income_Stream_Mod_Ability binds to its target stream. [M]
- The "human gets no refund on auto-cancel" reading of 0x498820 via 0x498170. [M]

## 10. Trace evidence and port status (skai, 2026-11-02)

From the retail Geonosis trace (q2f, `tools/probe_mod/q2_traces/out/20261030-062751-skirmish-vanilla-lua`, seed 0x41ccdfe4,
human Empire idle vs AI Rebel Medium), read without the binary:

- Influences_Capture_Point unset = true [H, trace]: the Rebel AI owns a Mineral_Extractor_Pad by frame 600 with X-Wings as its
  only mobile units, and X-Wing sets no Influences_Capture_Point (the shipped data sets it False on 150 types, True on 38).
  The port read an unset tag as false, so no shipped infantry or fighter could take a pad.
- The AI's credits fall in tactical play (Rebel 6000 → 2500 / 900 / 226 at frames 600 / 1200 / 1800, while it starts
  extractors, then rises with their income to 2112 at 5400). §3 says AI tactical *production* is never charged, so these
  costs are pad builds (TacticalBuild, L1_S4 §3.6) and/or the station upgrade; which ones is open [M]. The port still
  charges the AI for production (Tactical_Build_Cost_Multiplayer) [port].
- Frame-0 objects: each slot has its skirmish station, its Standalone_Space_Maps_Special_Weapon (§2.3) and 2 squadrons
  of its Space_Skirmish_AI_Default_Forces (Empire 14 TIE Interceptors, Rebel 10 X-Wings); the idle human gets them too.
- No hangar launches in 5400 frames for the idle human (its station keeps 14 interceptors, no TIE Fighters). The port's
  skirmish station hangar launches its Starting_Spawned_Units at once [port, open].

Port status (headless `--skirmish-ai skirmish` / `foc-land`, `tactical_battle_skirmish.cpp`):

| Item | Port |
|---|---|
| §1 slot map | slot 0 / 1 = the two battle players by ascending id (Empire 0, Rebel 1); non-playable players follow in GalacticRules order, not the lobby order [M]. |
| §2 step 3 credits | MP_Default_Credits for the two slots when the battle starts. |
| §2.2 starting forces | the factions' Space_ / Land_Skirmish_AI_Default_Forces as the battle's start participants at the slot's start anchor (Player_k / Team_tt spawn markers, bt_skirmish_anchors); no garrison re-delivery (§2.4) [port]. |
| §2.3 space special weapons | not placed [port, open]. |
| §2.5 / §2.6 pre-built pads | t50 rule kept: the Team_NN Command Center and Barracks pads of both slots start built, from MultiplayerStructureMarker data. §2.5 says retail reads that data only in campaign and pre-builds from the map's pre-placed pad type (+0x18), which the port's map reader does not decode [M, open]. |
| §3 production | land and space skirmish; a finished land company joins the reinforcement pool and arrives by a later Reinforce landing (§3.1, q6 t19, 2026-11-04). Space production still appears in front of its builder [port]. |
| §4 victory | 0x13 (land) and 0x16 (space) polled each frame, declared at once, reported as 0x0e / 0x0f [port]. |
| §5 income | per-frame float add of (base × multiplier + additive) / interval / 30 for the object's own stream; a mod on the object's own stream (the supply dock) counts while the object lives; mods on other streams are not applied [M]. |
| §6 unit cap | ConflictFactionInfo::unit_cap_for_players: sorted keys, linear, 0 outside, integer part (2 players: 9); N = the two slots. |
| L1_S4 §4.2 capture start gate | 0 in skirmish map battles. |
| Time limit | none for the skirmish host (the port's 8-minute campaign battle limit is off; the run ends at `--ticks`). |
| L1_S4 §3.6 pad rule 0x42e400 | a Neutral-faction owner (map props) does not block a pad (BATTLE_TRANSITIONS: "a Neutral player is simply not an enemy"); with `Influences_Capture_Point` defaulting to true, every prop near a pad blocked it before [M]. The Lua `pad_rule` copy (`engine/lua`) still uses the battle's `is_enemy` [open]. Correction (q6 t19, AI_TACTICAL §7.5.1) [H]: 0x42e400 has no Neutral exemption; every alive `Influences_Capture_Point` object not allied (relation ≠ 0) with the pad owner blocks, Neutral-owned ones included, unless it has BASE_SHIELD or DUMMY_GROUND_STRUCTURE. Which shipped props near pads have `Influences_Capture_Point` (default true) decides the effect [open]. Port (q6 t19, 2026-11-04): the exemption is gone, and the candidates are the battle's live units only [port, open], because Neutral props (`Influences_Capture_Point` unset) next to three Utapau pads otherwise stopped every AI build there (AI_TACTICAL §7.5.1 Port notes). |
| AI_TACTICAL §7.5 Build / Build_All | a task force with no reservations takes its pad picks from its plan's PotentialPlan first (BuildEconStructure.lua calls Build_All without Produce_Force) [M: retail's TF holds them from plan start]. |
| AI_TACTICAL_PERCEPTION TacticalBuiltStructureCount | counts the player's finished structures on build pads (pre-built base included) [M]. Before, nothing was counted, so `Num_Refineries` stayed 0 and `Tactical_Multiplayer_Should_Build_Units_Generic_Land` (gated by `Is_Initial_Skirmish_State_Established`) never fired. |
| AI_GALACTIC_3 §1.11 produced unit | a task force with a unit still in production is not empty (No_Units_Remaining waits for it); before, the Produce_Force plan ended 3 frames after its order. Since S7 t20 (main) this is the `units_lost` check, raised from the removal signals; the skai poll was dropped on the merge. |

Land skirmish result (headless `foc-land`, _MP_LAND_UTAPAU, both slots AI, 2026-11-02): the Rebel takes the Outpost pad and
builds a Ground Mining Facility (frame 3575), then buys tech 2 and 3, upgrades, turrets, every base structure, three
Mineral Processors, vehicles and heroes; the Empire stays at its pre-built base with Boba Fett and infantry, builds one
Mineral Processor, and its Destroy_Structure plans fail. Geonosis (space, idle human Empire): the Rebel's credits at frame
600 are 2500.0, as in the retail trace; later the Rebel takes fewer extractor pads than retail (2 vs 5 by 5400): its
Secure_Build_Pad_Space plans (MOVETOLOCATIONRUSH) end in failure 15 frames after they start [open].

skai t02 (the AI takes the enemy base) [open, retail probe]: in the port the Rebel never starts a Destroy_Structure plan.
Its free store is empty at every solve: its whole army (5 infantry teams and the droids) is at the 2-player land cap of 9
and held by Land_Scout_Area / Secure_Contestable plans. Open questions: is 9 the real cap (or do heroes, reinforcement
drops or vehicles count apart), and does retail pre-empt those plans for an attack. Probe:
`tools/probe_mod/q2_traces/run_skirmish_land_attack.cmd` (Utapau, idle Empire vs Rebel Medium, 36000 ticks), read with
`analysis/land_attack.py`. Port run to compare: `alamo_headless --skirmish-ai skirmish --map _MP_LAND_UTAPAU --human EMPIRE
--ticks 36000`.

skai t07-t11 (2026-11-12) port status: §2.6 pre-built pads = every active pad whose type keeps the default
Respects_Skirmish_Pre_Build_Bases_Option (only the five generic `<FACTION>_BUILD_PAD`s set it false) starts built with the
owner's first UC type of its faction row ([port] proxy for the map-saved pre-placed type, vf13 not decoded); Utapau gets 10
structures per slot, as retail's frame 0. Land start companies stand on the last TEAM_tt spawn marker in map order. The
skirmish AI pays pad builds (Tactical_Build_Cost_Multiplayer) when it starts one; campaign land battles keep the free AI
pad builds of L1_S4 §3.6 [port]. Build_Limit_Current_Per_Player: 0 refuses, > 0 refuses at live + queued >= limit
(STRATEGIC_GAPS 23). A survivor company (Destruction_Survivors) spawns its Company_Units. Imperial Command Center self repair
and E_Magnetically_Sealed_Structure (Initially_Enabled No) were already as retail.

Answered by the retail runs of 2026-11-08 (skai t06): `SKAI_T06_LAND_RUNS.md` (retail does not take the base in 36000 frames:
the Command Center survives; pre-built base of 9 structures per slot; default forces on the last TEAM_tt spawn marker; the AI
pays production at enqueue and pad builds at UC creation; Neutral pads change owner 150 frames after the first unit arrives).

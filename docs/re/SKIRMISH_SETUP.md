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
  condition is chosen by a game-kind enum (not the mode type): kind 2 takes the land default (GC+0x5b8),
  other kinds the space default (GC+0x5bc), kind 4 forces 0x15. [M]
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

0xeed80 also copies GC Skirmish_Buy_Credits (GC+0x1184c, 2000) to 0x140a62214 / 0x140a62218: the budget
of the pre-game buy-units dialog (IDD_SKIRMISH_BUY_UNITS_DIALOG, list built by 0xf0600) [M]. Faction
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
- 0x15: CAPTURE_POINT (0x3d) objects whose type has Control_Point_Domination_Condition_Relevant (+0x62);
  the rest of the body was not read [L].
- 0x16 (and campaign 0xf), space only: DUMMY_STAR_BASE objects enemy to E, ignoring dead (0x40), flag
  0x80, non-Victory_Relevant and non-playable-owner ones. None left: E wins. If E is not human, E wins
  only when no remaining starbase is allied (0x39a2c0) with a human player.
- So "base components" = objects with behaviour DUMMY_LAND_BASE_LEVEL_COMPONENT that are Victory_Relevant.

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
present goes through neutral first (2 x 5 s). When the pad's child dies the pad resets to neutral.

## 9. Not settled

- Reader and use of Skirmish_Reinforcement_Delay_Frames (GC+0x117e8). [L]
- The pre-game buy-units dialog: what it buys and how the 2000 budget is spent. [M]
- Body of victory type 0x15 (control point domination). [L]
- Meaning of player+0x180, global 0x140b15490 and predicate 0x35f790. [L]
- Sort key slot+0xa0 for slots when GMM+0x20 != 1. [M]
- Whether retail space maps carry PLAYER_<k>_SPAWN_POINT_MARKER objects: the two fighter squadrons per side
  in the trace may be the start force or the station garrison. [M]
- How an Income_Stream_Mod_Ability binds to its target stream. [M]
- The "human gets no refund on auto-cancel" reading of 0x498820 via 0x498170. [M]

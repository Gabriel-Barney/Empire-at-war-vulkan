# bkg3: RE answers (2026-11-12)

Roadmap group `bkg3`. Three claims that engine code cited from an RE page that was never committed are re-derived
here from scratch. Source: the Ghidra decompilation of the FoC x64 `StarWarsG.exe` (addresses are image offsets,
relative to 0x140000000), a `dumpbin /disasm` listing of the retail exe where Ghidra dropped code (0x295cf0: the
marker swap body is "unreachable block removed" in the decompile; the object chunk reader / writer), the shipped XML,
and a byte scan of `maps.meg`. Behaviour is described in prose; no decompiled code is reproduced.
Tags: [H] read directly; [M] one inferred link; [L] hypothesis.

## Index

| Q | claim | verdict | key addresses |
|---|---|---|---|
| 1 | the hop factor ignores the campaign's route and takes the first global route (ascending unsigned CRC of the upper-cased name) whose endpoints match in either orientation | **verified [H]**, with one addition: the hop must first pass the campaign link test (0x3f7b30). Path cost and AI estimates do **not** use this lookup | 0x397470, 0x3f7b30, 0x471470, 0x3f95a0, 0x3f7490, 0x3f5e10 |
| 2a | 0x401ba0 = head item's live remaining time + full build times of the rest of that queue | **verified [H]** | 0x401ba0, 0x4ad310, 0x371230 |
| 2b | queue selector: sel = 1 when +0x86c < 1, else by +0xf0c == 0 and not kind 0x5F | **verified [H]**; +0x86c = `Build_Cost_Credits`, +0xf0c = `Required_Ground_Base_Level`; sel 0 = ground queue, sel 1 = space queue | 0x664210, 0x3fff70 |
| 3a | the skirmish team-marker swap clears +0x3CB on what it creates | **verified [H]** (disassembly; missing from the Ghidra output) | 0x295cf0 (store at 0x296314) |
| 3b | map load reads mini-chunk 0x29 into +0x3CB; a structure built or pre-built on a pad copies the pad's byte | **verified [H]** | 0x39ce40 (0x39d18c), writer 0x3a5edb, 0x42df80, 0x42e890 |
| 3c | which objects spawn hangar squadrons for an idle player in space skirmish | **none** [H code, M map data]: the station is created with +0x3CB = 0 and keeps 0 through upgrades; only the start-force re-delivery can come out of its hangar | 0x593350, 0x594600, 0x70f070, 0x280c50, 0x287e70 |

Side corrections to existing pages (not edited here): `SKIRMISH_SETUP.md` §2.5 says MultiplayerStructureMarker types
have "no skirmish reader"; 0x295cf0 is one (Q3.1). `STATION_HANGARS_SOUND.md` §2.1 says +0x3CB is "1 for
SPAWN_SQUADRON objects (set at create)": true at create, but the map byte, the marker swap and the pad copies overwrite
it afterwards (Q3.2).

---

## Q1. Hyperspace hop factor: which trade route

### Short answer

- **Verified [H].** The hop factor 0x397470(mover, faction, A, B) does two separate things:
  1. **Link test** 0x3f7b30(A, B): A's planet data pack (+0xb8) has a link array (data +0x58, count +0x60, 0x20-byte
     entries); true when some entry's neighbour **object id** (+0x10) equals B's id (B +0x50). These links come from
     the **campaign's** `Trade_Routes` list.
  2. Only when linked: **route lookup** 0x471470(TradeRouteManager 0x140a2a060, A, B). It walks the manager's route
     map from begin to end and returns the first route R with (R +0x30 == A +0x298 and R +0x40 == B +0x298) or the
     swapped pair. +0x298 is the planet's **GameObjectType pointer**; R +0x30 / +0x40 are the resolved `Point_A` /
     `Point_B` types. If a route is found and its `HS_Speed_Factor` (R +0x50) ≥ the current value, the factor
     becomes R +0x50.
- The map is an ordered tree keyed by a 32-bit unsigned value (node +0x20, compared unsigned in 0x471360's lower
  bound); the key is the CRC-32 of the **upper-cased route name** (loader 0x4715e0: `_strupr`, then 0x215a30; log
  "--- Created new TradeRouteClass for key '%s' - CRC %u"). So "first" = **lowest unsigned CRC-32 of the upper-cased
  name**, over **all** routes of TRADEROUTES.XML, whether or not the campaign lists them [H].
- The link entry itself stores the campaign route (entry +0x08 = TradeRouteClass*), but 0x397470 does not read it.
  That is the whole discrepancy.
- **Nal Hutta ↔ Kashyyyk:** `KASHYYYK_NALHUTTA` CRC 765897670 (HS 6.0) sorts before `NALHUTTA_KASHYYYK` CRC
  3159653329 (HS 5.0) (both recomputed here). The campaign lists only NalHutta_Kashyyyk, so the planets are linked,
  and the global scan returns Kashyyyk_NalHutta: factor **6.0**, ratio 6/5 over the listed route. Matches retail's
  3.15 per tick against the port's 2.62 [H].

### Evidence

- 0x397470: start 1.0; if the faction has `Benefits_From_Corruption` (+0x10b) and both planets' data +0x2f4 ≠ −1,
  `Corruption_Hyperspace_Bonus` (0x140b2793c); then the link test and the lookup above. Result = max(base, route HS)
  when linked and a type-matching route exists; otherwise base [H].
- Callers of 0x397470 (all movement): 0x431c80 (fleet departure; logs "Fleet HS trade route speed factor (%s to %s) =
  %.2f"), 0x569980 (two calls), 0x56a010 and 0x56a1e0 (next hop of a path). 0x471470 has no other caller [H].
- How links are made (0x3f7490, per planet P at galaxy build): walk the campaign's resolved route vector (campaign
  +0x178, count +0x180) **in the campaign's list order** (0x479400 resolves each listed name with 0x471360, by CRC,
  and appends); for each route with one endpoint type == P's type, find the first object of the other endpoint's
  type (0x2a87d0) and call Create_Trade_Route_Link 0x3f5e10(P, other, route, force = 0). 0x3f5e10 returns at once
  when P already has a link to that neighbour, so **a pair gets the first route in campaign list order that joins
  it**; later listed duplicates are ignored. The entry is appended to both planets: {route CRC (route +0x20),
  route*, neighbour id, neighbour*} [H].
- Endpoints: every comparison is by **type pointer** (route +0x30 / +0x40 against object +0x298), never by name
  string or planet object. Two planet objects of the same type would be indistinguishable to 0x471470 [H; M that
  +0x28 / +0x38 in GALACTIC.md §1 are the 16-byte type references whose pointer halves are +0x30 / +0x40].

### Same lookup elsewhere?

| use | function | which route | tag |
|---|---|---|---|
| hop speed factor | 0x397470 → 0x471470 | global map, lowest CRC, type match | [H] |
| (a) link / adjacency test | 0x3f7b30 (id match), 0x3f95a0 `Find_Trade_Route_To` (neighbour pointer match, returns the link's route) | **campaign link** | [H] |
| (b) player path finding: graph 0x3f1c10, edge cost 0x3ef4c0 | edge exists when 0x3f95a0 ≠ null or distance ≤ `MaximumFleetMovementDistance`; cost = distance / (**link's** route +0x50) when linked, else distance (+100 for a corrupted planet under one flag path). The A* heuristic divides by the largest link-route HS seen (finder +0x130) | **campaign link's route** | [H] |
| (c) AI path graph edge 0x693c30 (feeds the travel-time estimate 0x648e80 through 0x690f10 / 0x691370) | cost = distance / (**link's** route +0x50), stored as int(cost × 10) in 16 bits; unlinked and farther than finder +0xa8 → no edge | **campaign link's route** | [H] |
| (c) AI planet-evaluator distance 0x662b30 | linked (0x3f7b30) → distance / GameConstants `TradeRouteMovementFactor` (+0x10c, vanilla 0.5); else the plain distance when ≤ `MaximumFleetMovementDistance`; else −1. No route HS at all | constant | [H] |
| AI neighbour lists 0x661890 / 0x661da0 / 0x6ddca0 / 0x6ddfb0, `IsConnectedTo` 0x65d830 | 0x3f7b30 / 0x3f95a0 as booleans | campaign link | [H] |
| trade-route income 0x3f6210, political control | link's route (GALACTIC_ECONOMY §1) | campaign link's route | [H, earlier page] |

So retail is inconsistent with itself: **route choice and ETA use the listed route's HS (5.0), the actual flight uses
the global lowest-CRC route's HS (6.0)**. The AI estimate 0x648e80 divides the path length (already divided per edge
by the link route's HS) by speed × faction factor × mode multiplier; it never calls 0x397470.

### No route joining the planets

- Not linked in this campaign (0x3f7b30 false): the global map is **not** consulted; the factor is 1.0, or the
  corruption bonus (1.5) when it applies. A type-matching route that exists in TRADEROUTES.XML but is not in the
  campaign therefore gives nothing [H].
- Linked but no type-matching route in the global map: cannot happen with consistent data (the link's own route
  matches); the code would keep the base factor [H].
- Path finding still allows the hop when the distance ≤ `MaximumFleetMovementDistance` (100) [H].

### Affected vanilla pairs

Of 329 routes (305 distinct endpoint pairs), 18 pairs have two routes with different `HS_Speed_Factor`. The winner
(lowest CRC, listed first) is used for the flight whenever the pair is linked, whichever route the campaign lists:

Carida_Abregado_Rae 5 (other 6); Abregado_Rae_Coruscant 5 (6); Fondor_Abregado_Rae 5 (6); Dantooine_Alderaan 5 (6);
Endor_Bestine 5 (7); Bothawui_Kuat 5 (6); Tatooine_Bothawui 6 (5); Byss_Felucia 5 (6); Coruscant_Corellia 5 (6);
Coruscant_Corulag 5 (6); Eriadu_Dagobah 5 (6); Sullust_Eriadu 5 (6); Fondor_Thyferra 6 (5); Mustafar_Hoth 6 (5);
Kashyyyk_NalHutta 6 (5); Utapau_Mandalore 6 (5); Yavin_Saleucami 6 (5); The_Maw_Yavin 5 (TheMaw_Yavin 6).

[M]: the pairs were grouped by the Point_A / Point_B names in the XML; retail groups by resolved type pointer.

### Port consequence

- Hop factor: keep the link gate on the campaign's links, then pick the route by scanning **all** loaded routes in
  ascending unsigned CRC-32 of the upper-cased name, first whose two endpoint planet types match in either
  orientation; factor = max(base, that route's HS). Tier 1 (speed is visible: 3.15 vs 2.62 per tick).
- Path cost, A* heuristic, AI edge cost and income keep using the link's own route (first campaign-listed route for
  the pair). Do not "fix" them to the global route: retail does not.
- Link creation: first listed route per pair wins; a second listed route for the same pair adds nothing.

---

## Q2. Production queue time 0x401ba0 and the queue selector

### Short answer

- **0x401ba0(production behaviour, planet, sel) verified [H].** It reads the planet's production object (planet
  +0xd0), queue `sel` (item-pointer vector at +0x10 + sel × 0x18, count at +0x18 + sel × 0x18). Sum, float, in queue
  order:
  - item 0 (the head): 0x4ad310(item +0x14) = the item's countdown timer read **live**: (start + duration − NOW) /
    30 as float, 0 when already past;
  - every later item: 0x371230(item type (item +0x08), planet) = that type's **full build time at this planet**.
  - Empty queue → 0.
- **Selector verified [H]** (0x664210, both the flag 0 and flag 1 branches; the same predicate at 0x6639f0,
  0x525200, 0x400370 and others): sel = 1 when T +0x86c < 1, **or** (T +0xf0c == 0 **and** T lacks behaviour 0x5F);
  otherwise sel = 0.
  - T +0x86c = `Build_Cost_Credits` (int).
  - T +0xf0c = `Required_Ground_Base_Level` (int).
  - behaviour 0x5F = DUMMY_GROUND_STRUCTURE (test 0x370320(T, 0x5f)).
  - **sel 0 = the ground queue** (priced types that need a ground base level, and ground structures); **sel 1 = the
    space queue** (everything else: ships, squadrons, star bases, orbital structures, heroes without a ground-base
    requirement, and every free type).

### Evidence

- The enqueue 0x3fff70 (Add to production queue) uses the mirrored test to pick the vector: `Required_Ground_Base_Level`
  ≠ 0 or DUMMY_GROUND_STRUCTURE → the vector at production +0x08 (count +0x18) = index 0; otherwise the vector at
  +0x20 (count +0x30) = index 1 [H]. Index 1 additionally needs `Required_Star_Base_Level` (+0xf10) ≠ 0, or
  DUMMY_STAR_BASE (0x5C) with T +0x8a0 == 1; else the order is refused. A cost < 1 is never queued at all, so the
  "cost < 1 → 1" arm of the selector only decides which (unaffected) queue's wait a free type is charged in the
  estimate [H].
- 0x400370 (build time with modifiers) applies the star-base-level production speed-up only to types passing the
  sel = 1 predicate, which fits "1 = space" [H].
- The item (ObjectUnderConstructionClass, 0x38 bytes): +0x08 type, +0x10 countdown seconds, +0x14 timer, +0x20 owner
  id, +0x28 cost. The timer is set at enqueue with the modified build time (0x400370); the log says "countdown is
  %.2f seconds, once underway" [H]. [M]: whether a head item whose timer is held (blockade, pause) still counts
  down in 0x4ad310 was not read; 0x4ad310 itself is a pure (start + duration − NOW) read.
- Note the asymmetry: the head uses the timer (modified time, live), the rest use 0x371230 (the per-planet build
  time without the 0x400370 modifiers) [H].
- Callers of 0x401ba0: 0x664210 and 0x6639f0 (first solve only, byte 0x140b3a524 = 1), 0x53bb20 and 0x53cd60 (the
  AIPlanetBuildQueue adds its own slots on top), 0x65ba10, 0x667750, 0x669b00 (perception readers) [H].

### Port consequence

Both parts of the earlier claim stand; no correction. Port rule: queue_time(planet, sel) = remaining seconds of the
head (live, clamped at 0) + Σ btime(type, planet) over the other items of the same queue; sel as above, with a free
type (cost < 1) reading the space queue.

---

## Q3. Skirmish garrison / hangar spawn byte (object +0x3CB)

### Short answer

- **(a) verified [H, disassembly].** In the skirmish player setup 0x295cf0, the team-marker swap creates the
  faction's object with 0x29f810 and then stores **0 into +0x3CB** (0x296314), right after setting bit 0 of +0x3a3.
  Ghidra drops this whole block as unreachable, which is why the decompile shows no writer there.
- **(b) verified [H].** The object chunk reader 0x39ce40 reads mini-chunk **0x29** (1 byte) into +0x3CB (0x39d18c);
  the matching writer stores +0x3CB as mini 0x29 between 0x28 (+0x3C8) and 0x2B (0x3a5edb). Pad code copies the
  pad's byte to the structure in both paths: pre-built (pad vf13 0x42df80) and construction complete (0x42e890).
- **(c) Nothing spawns for an idle side in a space skirmish.** The star base comes from a `Team_NN_Space_Station`
  marker, so it is created with +0x3CB = 0; the upgrade 0x70f070 copies the byte to each next level; with +0x3CB = 0
  the hangar skips its own `Starting_Spawned_Units` entries entirely (0x593350 step 7, 0x594600). The only thing the
  station hangar can still launch is the player's **pending start-force re-delivery**, which is armed only after
  every unit of the start force is gone. An idle player that loses nothing gains nothing, as in the retail Geonosis
  trace (SKIRMISH_SETUP §10: "No hangar launches in 5400 frames for the idle human").

### Q3.1 The team-marker swap (0x295cf0) [H unless tagged]

0x295cf0 is the skirmish map-start player setup (the same function as SKIRMISH_SETUP §2.1: owner remap, AI setup
0x2886d0, build manager 0x4967a0, special weapons 0x293d60, 0x288980, 0x289010). Before the remap it does:

1. Clear bit 0 of +0x3a3 on every object.
2. For every GameObjectType of class "MultiplayerStructureMarker" (type list query 0x407f60(…, 0x11), class name
   compared with 0x3712e0), take a snapshot of all objects of that type (0x2a87d0) and for each marker object:
   - scan the **type name** with the format `TEAM_%02d_` (sscanf wrapper 0x1a8510). No match or team = −1 → the
     marker is left alone (the unprefixed markers such as "Space Station" are not swapped) [M: names are compared
     upper-cased as stored];
   - find the first player whose team (+0x54) equals the number. None, or no faction → the marker is deleted
     (0x2a6280) and nothing is created;
   - pick the first entry of the marker type's `Marker_For_Specific_Object_Type` list (count type +0x2108, getter
     0x3722f0) whose affiliation list (type +0xef0) contains the player's faction (0x3751a0);
   - delete the marker; create the picked type with 0x29f810 for that player (player +0x4c) at the marker's
     position (+0x78..+0x80) and facing (+0x84..+0x8c);
   - created object: +0x3a3 |= 1, **+0x3CB := 0**, damage-state refresh 0x3ac290;
   - if the created object is a build pad (behaviour slot +0x35b ≠ −1): pad data (+0x150) +0x18 := 0x374420(type,
     faction, 0) = the first entry of the pad type's per-faction buildable list (the multiplayer list, type +0x9d0,
     in skirmish), then pad vf13 (0x42df80, the pre-build of SKIRMISH_SETUP §2.6), and the occupant (0x42e3b0), if
     one was built, gets +0x3a3 |= 1.
3. This is the only 0x29f810 call in 0x295cf0.

Shipped data: `Team_00/01/02_Space_Station` list `Skirmish_Rebel_Star_Base_1, Skirmish_Empire_Star_Base_1,
Skirmish_Underworld_Star_Base_1`; the land `Team_NN_*` markers list the factions' build pads.
`_MP_SPACE_GEONOSIS.TED` contains exactly one `TEAM_00_SPACE_STATION` and one `TEAM_01_SPACE_STATION` placement and
no placed `SKIRMISH_*_STAR_BASE_1` (type-CRC byte scan of the map in maps.meg) [H for the scan, M that a CRC hit is
a placement]. So both skirmish stations at Geonosis are swap products with +0x3CB = 0, human or AI alike: 0x295cf0
makes no human / AI distinction here.

This also corrects SKIRMISH_SETUP §2.5 ("MultiplayerStructureMarker … No skirmish reader") and explains §2.6's
"pre-placed type (+0x18, map-saved)": in skirmish the +0x18 value is written by this swap, not read from the map.

### Q3.2 All writers and readers of +0x3CB [H]

From the full disassembly (every `[reg+3CBh]` operand in the image):

| where | what |
|---|---|
| create 0x29f810 (0x29fc95) | := 1 when the object has the behaviour slot +0x34c (SPAWN_SQUADRON, per STATION_HANGARS §2.1); other objects keep 0 |
| object chunk reader 0x39ce40 (0x39d18c) | := mini-chunk 0x29 (1 byte), for map placements and saved games alike |
| object chunk writer (0x3a5edb) | writes mini 0x29 |
| marker swap 0x295cf0 (0x296314) | := 0 |
| pad pre-build 0x42df80 (0x42e178) | structure := pad's byte |
| pad construction complete 0x42e890 (0x42eb4d) | structure := pad's byte (also copies it before the persist call 0x42f160) |
| station upgrade 0x70f070 (0x70f302 / 0x70f328) | new level := old station's byte |
| object state copy 0x472560 (0x47309a) | copy := source's byte (bulk field copy) |
| Lua `Set_Garrison_Spawn` 0x580250 | := argument. No shipped FoC script calls it (grep of Data/SCRIPTS) |
| menu demo battle 0x22f10 (0x2352f) | := 1 on the category-0x1a objects |
| readers: can-spawn 0x593350, launch pick 0x594600, bulk spawn 0x593960 | gate on ≠ 0 |

Map data [M, byte heuristic]: across every map in maps.meg, the mini-chunk sequence `28 01 xx 29 01 yy` has yy = 0 in
128939 cases and 1 in 15; in `_MP_SPACE_GEONOSIS.TED` all 39 are 0. So map-placed objects, pads included, carry 0
almost everywhere, and because the chunk is read after create, a map-placed SPAWN_SQUADRON object ends with the map's
byte, not with the create default [M for the create-then-read order].

### Q3.3 What can spawn in a space skirmish [H unless tagged]

- **The star base's own hangar entries** (`Starting_Spawned_Units_Tech_0`, reserve −1): **never**. +0x3CB = 0 from
  the swap; 0x593350 skips the entry scan (and its synced draw) when the byte is 0; 0x594600 skips the entry pick
  (and its draw). The byte survives every upgrade (0x70f070). Consequence for draws: a skirmish station makes **no**
  can-spawn / launch-entry draws.
- **Start-force re-delivery** (the only hangar output of a skirmish station): with +0x3CB = 0, 0x593350 falls to the
  skirmish-space-station branch (skirmish 0x28afb0, mode 2, station slot +0x38e ≠ −1): the first player allied
  with the station's owner whose pending list (player +0x2d8, count +0x2e8) is non-empty makes it true, and
  0x594600 launches the first pending type. The pending list has exactly two writers: the player tick 0x287e70
  (copies the start-force list +0x2a8 when the re-delivery timer expires and nothing is pending) and the save
  loader 0x2832f0. The timer is armed by 0x280c50 (skirmish only, not already armed, nothing pending; delay =
  faction `Garrison_Reinforcement_Delay_Seconds` +0x2ee4), whose only caller is the object-removal path 0x2a6a00
  when the player's start-force (garrison) list becomes empty (SKIRMISH_SETUP §2.4). So: **all start-force units
  dead → delay → the whole start force again, one unit per hangar launch**. An idle side that loses no unit never
  triggers it.
- **Structures built on pads** (extractors, satellites, …): they copy the pad's byte. Pads are map placements with
  mini 0x29 = 0 at Geonosis [M], and pads made by the swap have 0 [H]. A built structure with SPAWN_SQUADRON data
  would therefore not spawn either. An idle human builds nothing anyway: nothing in the build manager or the pad
  code starts a build for a human player without an order (0x3fff70 / the tactical build queue are command-driven)
  [M: by absence of an autonomous caller in what was read].
- **Bought capital ships** (Star Destroyer, …): created by 0x29f810 with +0x3CB = 1 and no later clear, so their
  fighter bays launch normally. Not relevant to an idle player, relevant to the AI side.
- **Frame-0 content every slot gets, idle or not** (SKIRMISH_SETUP §2.2 / §2.3 / §10): the station, the faction's
  `Space_Skirmish_AI_Default_Forces` start force (Empire: 2 TIE squadrons at Geonosis per the trace), the
  `Standalone_Space_Maps_Special_Weapon` objects. These are setup, not spawns over time.
- **Station upgrades**: only through the DUMMY_UPGRADE object's Skirmish_Automatic ability, which exists only after
  the upgrade is bought (STATION_HANGARS §1.1). None for an idle player.

### What stops an idle side from gaining units in retail

One store: +0x3CB := 0 at 0x296314. Everything else (no own-entry launches, no draws, byte kept across upgrades,
pad structures inheriting 0) follows from it. The port symptom (idle Empire gains about 19 units and loses 8) is the
port's skirmish station launching its `Starting_Spawned_Units`, which retail never does; SKIRMISH_SETUP §10 already
listed this as "[port, open]".

### Port consequence

1. Skirmish stations (anything created from a `Team_NN_*` marker): hangar spawn flag **off** at creation, for human
   and AI; carry the flag through `Next_Level_Base` upgrades. With the flag off the hangar makes no entry draws.
   Tier 1 (unit counts) and, through the removed draws, the synced draw sequence.
2. Hangar of a skirmish space station with the flag off: launch only the pending start-force re-delivery of the
   first allied player (player-list order) with a non-empty pending list; one type per launch, normal launch delay.
3. Start-force re-delivery (SKIRMISH_SETUP §2.4, currently "[port] none"): arm when the player's start-force list
   empties; after `Garrison_Reinforcement_Delay_Seconds` copy the start force to the pending list. Needed for parity
   once a side loses its whole start force; not needed to fix the idle-Empire symptom.
4. Structures built or pre-built on a pad copy the pad's flag; pads take theirs from map mini-chunk 0x29 (0 when
   absent) or 0 from the swap. Bought ships keep the create default (1).
5. `Set_Garrison_Spawn` keeps working as the only script override.

### Open

- Whether the 15 map objects with mini 0x29 = 1 are campaign land structures (expected: garrison buildings) was not
  resolved to types [L].
- Free-for-all skirmish (unprefixed "Space Station" markers are not swapped): how those maps get their stations was
  not read; every shipped FoC space skirmish map checked here (Geonosis) uses team markers [L for the rest].
- The held-timer case of 0x4ad310 (Q2) [M].

---

## Q4. Is_Like in the goal proposal pass

Added 2026-11-12 (second pass, Ghidra decompile only). Hypothesis under test: slot 9's "already active" test treats a
candidate (type T, target t) as active when an active goal on the **same target** has a type that is "like" T.

### Short answer

- **Verified [H].** The active-set lookup 0x721a50, which slot 9 (galactic 0x6b84f0, land 0x6b8710, space 0x6b8900)
  calls on the goal system's active set (+0x38), is **not** an exact (type, target) test. It finds the per-target
  entry for the candidate's target (goal +0x58, exact pointer; null for global goals) and walks **every goal stored
  for that target**; for each stored goal of type A it asks 0x6b83c0(A, T) = "is T in A's resolved like-set". The
  first hit returns true, slot 9 returns 1, and the candidate is skipped: **not evaluated and not counted**.
- **The like-set** is a resolved pointer set at goal type **+0x50** (hash set of type pointers; list head +0x58,
  buckets +0x68, mask +0x80). +0x38 is only the parsed name list. The set is built once after all goal files are
  loaded (0x6b7fa0, called for every type by the goal-type manager load 0x5e6c10):
  1. the type itself is inserted (so the exact (T, t) case is the same code path);
  2. for every name in the type's `Is_Like` list that resolves to a goal type X: X is inserted into this type's
     set **and this type into X's set**. Unknown names are ignored.

  So the relation is **reflexive and symmetric, not transitive**: T is like A when T == A, or A lists T, or T lists A.
  The direction question is therefore moot; formally the lookup reads the **active** goal's set.
- **Same target only.** Different targets never interact. All global goals (no target) share the null-target entry.
- **Container searched by slot 9: the active set (+0x38) only**, i.e. the goals kept by the last Commit (state 1
  Active or 2 Executing), whatever their category. The lookup does not read the goal's state, plan or category.
  It does **not** search the proposed set (+0xd8, the candidates accepted earlier in the same pass) and not the
  potential plans. Two like candidates on one target can both be evaluated and both be proposed in one pass; the
  proposed-set insert 0x721480 de-duplicates by exact (target, type) only [H].
- Numbers: Underworld, two active `Build_Underworld_Palace` on two planets: per planet the palace plus Barracks,
  Vehicle_Factory and Droid_Works are skipped; the port already skips the palace, so **3 × 2 = 6** more: 152 → 146,
  exactly the retail count. Rebel: `Build_Economic_Structure` lists 15 types and has **17 like types** after the
  symmetric step (two more types list it), so two active goals remove up to 2 × (17 + 1) = 36 candidates (fewer
  when a like type is not in the player's source list, is not proposable or does not Apply) [H for the rule, M for
  the arithmetic: the XML lists were grouped by a script here].

### Evidence

- 0x6b84f0: takes a reference on the candidate, calls 0x721a50(goal system +0x38, candidate); true → return 1. Else,
  if the candidate has a target: reach = target vf+0x38, then reach vf+0x30(level = type +0x2c, −1, 0, 0); null →
  return 1. Else 0. Nothing else is on the path: before `Evaluate`, 0x6cb120 calls only 0x6ca110 (type), the
  target's `Applies` (vf+0x20), the goal constructor 0x691710 and slot 9 (vf+0x48) [H].
- 0x721a50(set, goal): outer hash map keyed by the target pointer (goal +0x58); the value is an inner hash map
  type pointer → goal. It iterates the inner map's whole list and calls 0x6b83c0(node key = the stored goal's type,
  the candidate's type = first pointer of goal +0x48). No exact-key find is done on the inner map [H].
- 0x6b83c0(A, T): hash-set find of pointer T in A +0x50; returns found [H].
- 0x6b7fa0(type): insert self into +0x50; for each 0x20-byte string in the vector +0x38..+0x40, look the name up in
  the goal-type manager (0x140b31120, name → type at node +0x30); when found, insert it into self +0x50 and self
  into its +0x50 (0x6b78f0 = set insert) [H].
- Shipped lists: the four Underworld build goals list each other (already mutual). The infrastructure lists are not
  all mutual (list sizes 13, 14 and 15); the symmetric step evens them out per pair.

### Every reader of the like-set (goal system) [H]

Only two functions read type +0x50: 0x721a50 (through 0x6b83c0) and 0x692d30. Their callers:

| where | what it does |
|---|---|
| slot 9: galactic 0x6b84f0, land 0x6b8710, space 0x6b8900 | 0x721a50 on the active set +0x38: skip the candidate (above) |
| Resolve 0x6c8970, greedy phase | "already kept or active" = 0x721a50 on the **kept set of this Resolve** (a local set: the kept incumbents plus the goals accepted so far in this Resolve), then on +0x38. Pool[0] therefore **stops the greedy phase** when a like-type goal on the same target is already kept. By then +0x38 has had this category removed (0x7218c0(+0x38, cat), right after the classify step), so the second test only sees other categories' active goals [M for "this category only": 0x7218c0 was not read line by line] |
| Resolve 0x6c8970, weighted random phase | the same two 0x721a50 tests keep a Pool entry out of the weight table |
| Resolve 0x6c8970, greedy phase: 0x692d30(accepted goal, lastOld) | true when both have a type, the **targets are equal** and lastOld's type is in the accepted goal's like-set. It is tested **after** the goal has been accepted (plan refreshed, inserted into the kept set, activation success recorded, Activate, budget add): the goal stays accepted and the greedy phase then ends. AI_GALACTIC §1.6 step 5 lists it among the stop conditions without saying that the goal is kept and without the same-target condition |

No reader in Commit 0x6c8710 itself, in slot 10 (accept, 0x6b8660), in the budget or in the proposed-set insert.
Inside Resolve an old candidate (a replaceable incumbent with eff < best) is **not** in the kept set, so a like-type
proposal on its target can be accepted in its place; the lastOld test covers that case.

### Global_Exclusions is separate [H]

- Different field (name list at type +0x90), different store (goal system map +0x260: type → int), different scope
  (per type, **any target**); read by 0x6ca110 (proposability) and by Resolve's type checks.
- **Incremented** by Activate 0x693550, only when the goal's state is < 1: for each listed name that resolves to a
  type, 0x6ca8c0(goal system, type) adds 1 (creating the entry at 1); then state := 1. Resolve calls Activate when
  it accepts a goal that was not active yet (greedy and weighted phases). So the count rises **at Commit, when the
  goal is accepted**, not when its plan starts.
- **Decremented** by Release 0x693750, only when the state is not 3: if the state is 1 or 2 the goal is taken off
  the goal system's +0x2a0 list (0x6cafe0) and each listed type gets 0x6cb070 (subtract 1, never below 0); then
  state := 3. Release is called by Resolve (Pool leftovers that were active, failed re-checks), by maintenance and
  by the plan-end paths [M for the plan-end callers: four call sites outside Resolve were not re-read here].
- AI_GALACTIC §1.4 lists 0x693550 / 0x693750 / 0x6ca8c0 / 0x6cb070 together as "release decrements"; precisely,
  0x693550 + 0x6ca8c0 are Activate + increment, 0x693750 + 0x6cb070 are Release + decrement.

### Question 4

Not needed: the hypothesis holds, and nothing else sits between the type test and Evaluate in 0x6cb120.

### Port consequence

1. Goal types: after loading all goal files, build per type a like-set = {self} ∪ the listed types ∪ every type
   that lists it (resolve by name, skip unknown names, no transitive closure).
2. The "goal is in set S" test = **S holds a goal with the same target (both null for global goals) whose type's
   like-set contains the candidate's type**. Use it in:
   - slot 9 (all three modes) against the active set: skipped candidates are not evaluated, do not count in the
     quota or in evalsLastPass, and make no draw;
   - Resolve's greedy phase (stop) and weighted phase (not in the weight table), against this Resolve's kept set
     and the remaining active set.
3. Keep the proposed-set insert exact (target, type).
4. Greedy lastOld rule: accept first, then stop when the accepted goal and lastOld have the same target and like
   types.
5. Global_Exclusions unchanged: +1 per listed type at Activate, −1 at Release.

Tier 1 (which goals are proposed and accepted); it also changes the pass length and so the Commit cadence.

---

## Q5. Goal ETA at Resolve and planet-queue reservations

Added 2026-11-12 (third pass, Ghidra decompile only; shipped XML for the data). Trace under test (Heart of the Maw,
Rebel AI): retail starts Tank_Buster_Squad at Naboo at 156, Infantry_Squad at Naboo at 559 and only then the Naboo
mining facility at 807; the port accepts `Build_Economic_Structure` @Naboo (ETA 24.0) in the same Commit as
`Build_Ground_Forces` and starts the mine first (164).

### Short answer

- **Q5.1 [H].** "Plan can be activated" 0x6c4a50 is a **full re-solve** for a proposal (PP+0x100 == 0): it clears the
  first-solve byte 0x140b3a524, runs the solver 0x6c3da0 and, when the PP is still valid, **copies PP+0x48 to goal
  +0x7c (ETA) and PP+0x44 to goal +0x78 (net cost)**. Both Resolve phases then run 0x6129c0 (fund) and
  `0 < AllowedTime < goal +0x7c` on those fresh values, and only then 0x6c2d60. For a PP that is already reserved
  (PP+0x100 set: an old candidate taken from the pool) 0x6c4a50 is only the validity test 0x6c45e0; ETA and cost are
  not touched.
- The re-solve's queue time per planet and queue is 0x53bb20(Q, 1, 0) = **Q+0x50 + 0x53bda0(Q)**:
  - Q+0x50 = the value the build market stored at that queue's **last run** (at most about 1 s old, not live):
    0x401ba0 at that run (head's remaining time + the rest of the production queue) plus one term per task that
    stayed in or was claimed into the queue's task list (pay-as-you-go owner: max(btime, fundsTime); otherwise btime
    when fundsTime == 0, else nothing). It is only refreshed while the queue has a task list object (Q+0x10 ≠ 0).
  - 0x53bda0 = Σ build time of every live slot with flag 0, **including the slots made by goals accepted earlier in
    this same Commit**, in this or in an earlier category (their 0x6c2d60 ran already), and the slots of kept goals
    that were re-asserted already. Slots of active goals whose category has not been resolved yet are still flag 1
    (0x6c0be0, Commit step 4) and are left out.
- **Category order [H]:** Commit 0x6c8710 copies the category enum list (0x140b31088, list order) and sorts it with
  0x6c76f0, a std::sort over reverse iterators with "weight(a) < weight(b)" (0x612ee0, the normalised budget
  weight). Forward order = **descending weight; equal weights keep the enum order** (always the insertion-sort
  branch: at most 32 categories). This settles the [M] of AI_GALACTIC §1.6 step 3. Step 4 (0x6c0be0 for every active
  goal with a PP) runs for all categories before the first Resolve.
- **Q5.2 [H].** The first 0x6c2d60 of a PP makes one slot per selection i that is not a free-store entry (skip list
  0x140b3a5d0) and whose PP+0x180[i] planet has a production object: 0x53b810(queue of that planet picked by the
  type's selector (Q2), type, PP). PP+0x180[i] comes from the re-solve's ETA pass 0x6639f0:
  - structure / generic type (0x5C / 0x5B / 0x5F / 0x60): the reach object's target planet. `Build_Economic_Structure`
    @Naboo makes **one slot in Naboo's ground queue** (the mining facility is DUMMY_GROUND_STRUCTURE);
  - any other type: the planet with the strict minimum of
    (queue time + travel) × (1 + (base level − required level) × K), K = 1.0, over the reach list's planets that are
    revealed and can build the type, first wins ties; the chosen entry's queue time then grows by the build time, so
    later units of the same solve see it.
  - `Build_Ground_Forces` has no target. 0x6bfcf0 then takes the reach object of the **first entry of the AI's
    planet-target table** (goal system → context +0x38 → vf+0x40 → element 0) with level 5. The candidate list is that
    object's level-5 list, and the **travel term is still added**: straight-line distance from that first planet to
    the candidate / (speed × faction speed × FPS) [H code; M that element 0 is the galaxy's planet index 0: the same
    table is indexed with 0x3f8500(planet) in 0x6955a0 / 0x695c90].
- **Q5.3: rule (a) [H mechanism, M for this trace].** Nothing in the market orders by slot position or by arrival:
  - the start pick (0x53cd60 step 5) is the **least slack** over the queue's whole task list (goal ETA − elapsed −
    btime at this planet, strict <, first wins), limited to tasks that are buildable here and (pay-as-you-go or
    fundsTime ≤ 0). Slots are not read. So (b) cannot put the tank buster before an already claimed mine: the mine's
    slack (ETA ≤ 28.8 − elapsed − 24) is far below the ground units' (ETA ≥ 36 − elapsed − 12 or 8);
  - a bound structure task (bid −1, planet = target, 0x6956f0) is claimed at the target's next market run whatever
    the queue holds (step 3 tests only planet and queue index) and is always kept (step 2). There is no positional
    wait, so (c) does not exist.
  - Hence the retail order implies that the Naboo mine goal was **not active** at 156 and at 559. The code path that
    gives that is the time stop: the mine's accept re-solve read Naboo's ground queue > 4.8 s, so ETA > AllowedTime
    (24 × 1.2 = 28.8) and the proposal was rejected with an activation failure, in the greedy phase (which also ends
    that phase) and again whenever the weighted phase drew it.
- **Q5.4 [H].** An unbound task takes the lowest bid; a planet bids
  (Q+0x50 + btime(T, planet) + travel) × (1 + level surplus × K) × cost(T, planet) / cost(T, none), travel only when
  the task has a destination. The planet **does not bid when fundsTime < Q+0x50** (and drops its bid if it held the
  task's choice). A funded task has fundsTime 0, so **any queue with something producing or claimed refuses it**;
  Kamino with a mine (24 s) refuses. The port's `fundsTime < queueTime` test is the same rule.

### Evidence

- 0x6c4a50: PP+0x4c clear → false. PP+0x100 set → PP+0x4c := 0x6c45e0(), return. Otherwise 0x6c0e60, byte
  0x140b3a524 := 0, 0x6c3da0(PP); when PP+0x4c is still set: goal +0x7c := PP+0x48, goal +0x78 := PP+0x44, and
  PP+0x1a8 += 0x373500(type, mode) per selection (population). 0x6c47c0 has the same re-solve arm for a kept goal
  whose PP was never reserved [H].
- Resolve 0x6c8970, greedy loop, in this order: category credits (0x613af0) > 0; +0x2a0 empty; type enabled and not
  excluded; not in the kept set / active set (0x721a50); **0x6c4a50; 0x6129c0; 0x692860(goal, 0) against goal +0x7c**;
  0x6c2d60; remove from the pool (0x7227a0); insert into the kept set; activation success + Activate 0x693550 when
  new; budget add 0x610d30 in galactic mode; lastOld / limit tests. A failure of the three bold tests records an
  activation failure (0x58a5d0(…, 0)) and leaves the loop **without removing the goal from the pool** [H].
- Weighted phase: the weight table takes pool entries passing 0x6129c0 (on the goal's current +0x78 / +0x7c), the
  type tests, the two 0x721a50 tests and eff > 0. The drawn goal is removed from the pool, then 0x6c4a50, 0x6129c0
  and the time test run again; a failure releases it (budget release + 0x693750 only when it was active) and records
  an activation failure; the loop continues. A goal that failed the greedy phase can therefore be drawn and
  **re-solved a second time** in the same Resolve (its own synced draws again) [H].
- AllowedTime 0x692860(G, 0): no PP → `Time_Limit` (type +0x30); else 0x6c0450(PP) × `Build_Time_Delay_Tolerance`
  (type +0x34). 0x6c0450 = Σ 0x371230(selection type, no planet) × the mode's build-time factor; it reads the
  selection the re-solve just wrote [H].
- 0x6639f0(reach, selection, level, out PP+0x180, flag): flag 0 → each pool entry starts at
  0x53bb20(0x559b40(planet +0xd0, pool index), 1, 0); flag 1 → 0x401ba0. Structure arm: out[i] = reach target;
  structureSum += btime(T, list[0]); when list[0] is the target, pool entry 0 is marked used and grows by the same
  build time. ETA = max(used entries of both pools, structureSum). The travel term reads the reach object's target
  (reach +0x10 → vf+0x30) with no "has a target" test [H].
- 0x6bfcf0: goal +0x58 null → reach = (first element of the table returned by context +0x38 vf+0x40) vf+0x38, level
  5; otherwise the target's reach object and the goal type's level (+0x2c) [H].
- 0x53bb20(Q, extra, X = 0): `extra ? 0x53bda0(Q) + Q+0x50 : Q+0x50`. Q+0x50 writers: only 0x53cd60 (set to 0x401ba0
  at each run, then += per kept task in step 2 and += for the one claim in step 3) [H].
- 0x695c90 (bid): returns at once for bid < 0 (bound). Path test only when the task has a destination or its goal a
  target. Company with no destination and no goal target: refused when the planet's ground slots are full
  (0x3f9530 ≥ 0x140b168c8). wait = 0x53bb20(Q, 0, 0); `fundsTime (0x6490d0) < wait` → no bid, choice cleared if it
  was this planet. A lower bid, or the same planet, replaces the choice [H].
- Data: `Build_Ground_Forces` is category Offensive, Global, tolerance 2.0; `Build_Economic_Structure` is category
  Infrastructure, tolerance 1.2. Build times 15 / 10 / 45 / 30 (tank buster, infantry, light tank, mine); the trace
  shows × 0.8 (362 and 242 frames; mine 24 s).

### The trace under these rules [M]

1. Commit at ~97. Offensive is resolved before Infrastructure (needs weight(Offensive) ≥ weight(Infrastructure); a tie
   also gives Offensive first). `Build_Ground_Forces` is accepted; 0x6c2d60 makes its three ground-queue slots, at
   least one of them at Naboo.
2. Infrastructure: the mine @Naboo re-solve reads Naboo's ground queue = Q+0x50 (0) + that slot (12 or 8 s, or both),
   ETA ≥ 32 > 28.8 → rejected.
3. Market: Kamino and MonCalimari (mines, 24 s) refuse the funded ground tasks; Naboo claims one task per run and then
   refuses the next (its Q+0x50 > 0). The infantry waits on the pending list until the tank buster is done (518), then
   bid, claim one run later, start: 559 (41 frames = one market period + offset).
4. Later Commits: a slot at Naboo is cleared only when its unit starts **at Naboo** (market step 5, same type and PP).
   Once the infantry produces there, Naboo's ground queue time is its remaining time; the mine can be accepted from
   about frame 657 (remaining ≤ 4.8 s). Its bound task is then claimed behind the running item and starts at the
   first market run after the queue is idle: 807, 6 frames after 801. A task accepted only after 801 could not start
   that early (Commit, plan start, `Sleep(1)` in the plan script, claim), so the accept fell inside 657..~750, which
   is what rule (a) predicts.

Not excluded [L]: the mine goal failing 0x6129c0 (Infrastructure funds after two 500-credit mines) for the same
period. It would not explain the start 6 frames after the infantry left.

### Port against retail

Already the same: `PlayerAi::plan_activatable` re-solves and copies cost / ETA / nominal time
(`galactic_ai_impl.hpp`; the comment above `resolve_pp` in `galactic_pp.cpp` still says the goal keeps Init's ETA,
which is stale); `GoalSystem::resolve` runs re-solve, fund, time, reserve in retail's order;
`PlayerAi::category_order` is a stable descending sort; `reserve_pp_queues` makes slots at accept; the market's bid
refusal and least-slack start match.

Differences that can give the port ETA 24.0 for the Naboo mine in that Commit:

1. **No-target source choice** (`GalPp::source_list` / `recompute_eta`, `galactic_pp.cpp`): the port uses the
   player-level flood and adds **no travel term** without a target, so every idle planet costs 0 and the units go to
   the first idle planets of that list. Retail uses the first planet-target's level-5 list and adds the travel time
   from that planet, so the choice is "queue + distance to planet 0, scaled by the base-level surplus". A different
   source planet means no slot at Naboo and the mine passes the time test. Port change: for a goal without a target
   use `pa.stage_list(planet 0, 5)` and add the travel term measured from planet 0 (also in Setup C's 0x664210 list,
   A2M_T04 Q2: "the player's default reach object").
2. **Queue time on re-solves** (`GalacticAiWorld::Impl::planet_queue_time`): the port reads the production queue live
   and adds btime per Queued task. Retail reads Q+0x50 as stored at the queue's last market run, with
   max(btime, fundsTime) per listed task for a pay-as-you-go owner. Port change: keep a per-queue cached total written
   by the market service (step 1 value 0x401ba0, then the step 2 / 3 terms, as `queue_time` in `galactic_build.cpp`
   already computes) and return cached + unsuspended slots. Step 1 there should also be 0x401ba0 (head + rest), not
   the head alone.
3. **Category order in that Commit**: if the port's weights put Infrastructure first, the mine is accepted before the
   ground forces' slots exist. The sort itself matches, so this would be a budget-weight difference, not a Resolve
   one. Check first: log `category_order()` and the slots at Naboo's ground queue at the port's Commit at 99.

No change is needed in the market for this trace: with the mine goal not accepted, its task does not exist.

### Open

- Which of differences 1-3 the port hits at frame 99 was not determined (no run in this pass) [L].
- The value of the first planet-target entry for Heart of the Maw (which planet the no-target travel term measures
  from) [M].
- Retail's per-Commit weights for Offensive and Infrastructure at ~97 were not derived from the budget equations.

---

## Q6. Commit category order: the sort key and when it is written

Added 2026-11-12 (fourth pass, Ghidra decompile only; shipped XML for the equations). Trace under test (Heart of the
Maw, Rebel AI): the port's raw budget values at the passes ending at 56 and 99 are Offensive 7.53 and Infrastructure
19.91 (3.30 if the structure tokens were read fresh at 99), so the port resolves Infrastructure first at 99. The
question is whether retail can resolve Offensive first at its Commit near 97.

### Short answer

- **Q6.1 [H].** The key is the category's **normalised weight** (category +0x3C, read through 0x612ee0; a category
  that is not a budget member reads 0.0). Not the raw value, not the pool A (+0x44), not P or the rates. Order =
  **descending** weight. The sort is the insertion-sort arm of 0x6c76f0 (strict "<" on a reversed range), which is
  stable: equal weights keep the base order. The base order is the node list of the `AIGoalCategoryType` enum
  object (0x140b31088 +0x10), not the budget's member list and not the template's `<Budget>` order. That list
  belongs to a hash map (value → name) [M: its node order is the map's, which need not be ascending enum value; it
  only matters between categories of exactly equal weight, in shipped Rebel data the `Zero` ones and non-members].
- **Q6.2 [H].** The weight is rewritten **every proposal pass**, by 0x614590, called from the goal service 0x6cacd0
  in the same call and immediately before Commit 0x6c8710 (when the pass-complete byte +0x184 is set). The income
  tick 0x614340 and the reset flag never touch +0x3C (they move money only, and copy +0x3C to the applied weight
  +0x38). 0x614590 calls `Evaluate` on every member's equation every time, start value = that category's previous
  normalised weight (kept only when the evaluation fails); negatives → 0; then w / Σw (1 / member count when Σw = 0).
  "Fresh" stops at the leaf cache: every token inside the equation goes through its node's cache (Q6.3). Init
  0x6137b0 is the same evaluation with start value = the field's initial content.
  Normalising divides every member by the same Σ, so the order by normalised weight **is** the order by clamped raw
  value of that same pass.
- **Q6.3 [H].** Lifetimes are whole **seconds**; the test is in **global logic frames**:
  hit ⇔ stamp ≠ 0 and (L = −1 or NOW ≤ stamp + L × FPS), NOW = 0x140b0a320, FPS = 0x140b0a340 = 30 (0x64d740 /
  0x65a780). Not the mode frame, not the Lua clock, no float seconds. A store (0x64d990) sets value and stamp = NOW;
  a hit does not restamp. Confirmed at the registrations:

  | token | compute | node | L | valid through |
  |---|---|---|---|---|
  | player `StructureCount` | 0x66b3c0 | visible (0x665ec0) | 4 | stamp + 120 |
  | player `HasStructure` | 0x668a30 | visible | 3 | stamp + 90 |
  | player `NumStructure` | 0x66aa60 → 0x668490 | visible | 3 | stamp + 90 |
  | target `StructureCount` | 0x65ebd0 | visible (0x659bd0) | 2 | stamp + 60 |
  | target `HasStructure` | 0x65f3b0 | fogged, mask 0x200 (0x659ab0) | 13 | stamp + 390 |

  - The player tokens loop over the context's planet table, keep the planets whose owner token equals Self, and read
    `Target.<token>{caller's parameters}` through the normal path (0x4dd1d0), so each planet's own leaf cache is
    read and written too. A stale player entry hides fresh planet entries; a fresh player read can still sum stale
    planet entries.
  - **Early invalidation:** none for visible leaves (all player tokens and target `StructureCount`). No entry is
    ever removed, and nothing in the production, object-create or budget code touches a node's map: the only
    writers are the store 0x64d990 and the 0xE recompute. Only the fogged target `HasStructure` can be refreshed
    early: a signal 0xE on that planet for the system's own player whose mask contains 0x200 (the owner's mask is
    −1) recomputes every entry of the node and restamps it (AI_GALACTIC_TOKENS §9.9). Raisers are the ones listed
    there (presence refresh 0x3a51e0 of the planet for that player, conflicts). A production-queue change by itself
    is not among the traced raisers [M: the 0x3a51e0 caller list is not complete].
  - The proposal pass reads the same nodes with the same keys, so an entry stamped by a goal equation serves the
    budget equation and the reverse.
- **Q6.4: (c) [H for the mechanism, M for the trace].** Neither (a) nor (b) holds:
  - (a) is out: the key is evaluated in every pass, also before the first income tick, and the pools are not read.
  - (b) is out for a Commit near 97. `BasicRebelInfrastructureBudgetAllocation` is
    1.1 × (…) × (1 + Maintenance) × (1 + 2 × (0.5 > mines / planets)) + 10 × (number of the three `HasStructure`
    == 0). 19.91 = 3 × 3.30 + 10: the ×3 comes from player `StructureCount{mine}` (L = 4) and the +10 from
    `HasStructure{mine}` (player L = 3 over the planets' L = 13). Whatever frame s first stored
    `StructureCount{mine}` (Init, or with the stamp-0 rule the first later read, at the latest the reweight at 56),
    it was before the first Commit could queue anything, and the entry stays a hit through s + 120 ≥ 120. So at 97
    retail also has the ×3: Infrastructure ≥ 9.91 > Offensive 7.53, and 19.91 while the planet entries keep the +10
    (through s + 390 unless a 0xE on the mine's planet). **Infrastructure sorts first at 97 in retail as in the
    port** [M: assumes retail's inputs at Init equal the port's].
  - Later passes: from s + 121 the ×3 can go (13.30 while a planet `HasStructure{mine}` entry is still the old 0),
    and only after the planet entry is refreshed does it reach 3.30 and fall behind Offensive. This also fits retail
    still funding large Infrastructure goals later: the weight is recomputed each pass.
  - So if retail accepts `Build_Ground_Forces` and not the Naboo mine at that Commit, the cause is **inside Resolve
    of the Infrastructure category** (resolved first), not the category order: the greedy phase's category-credits
    gate (0x613af0: A ≤ 0 → nothing accepted), the fund test 0x6129c0, or the time stop of Q5 with a Naboo queue time
    that does not come from this Commit's Offensive slots (Q+0x50 from the market's last run, or kept goals of
    Infrastructure re-asserted before it). Which one is not decided here [L].

### Evidence

- 0x6cacd0: pass step 0x6cb120; when +0x184: 0x614590(goal system +0x190), 0x6c8710, +0x184 := 0, quota.
- 0x6c8710: copies node +0x10 (int) of every node of the list at 0x140b31088 +0x10 into a vector, calls
  0x6c76f0(end, begin, count, budget). 0x6c76f0 for ≤ 32 elements: walks from end − 2 down to begin; an element moves
  towards the end only past neighbours with a strictly greater weight (two 0x612ee0 calls per compare). Result:
  descending, stable.
- 0x612ee0 returns category +0x3C; 0x613af0 returns "A (+0x44) ≤ 0" (true also for a non-member).
- 0x614590: per member in list order, +0x3C := Evaluate(start = +0x3C), clamp, Σ; galactic only: A := P, R := I,
  per goal A −= a, c := 0, re-add := 1; second loop +0x3C /= Σ. No test of the reset flag or of any clock.
- 0x6137b0: same loop, then +0x38 := +0x3C, A := P := credits × w, I := R.
- Registrations: planet evaluator 0x659ab0(node, ev, 0x200, ev, &0x65f3b0, 13), 0x659bd0(node, &0x65ebd0, 2); player
  evaluator 0x665ec0(node, &0x668a30, 3), (&0x66b3c0, 4), (&0x66aa60, 3).

### Port against retail

- `PlayerAi::category_order` (stable sort, descending normalised weight) and `PlayerAi::budget_weights` (every
  pass, start = previous weight, before Commit) match [H]. Only difference: the base order is the port's category
  index, retail's is the enum map's node list [M]; visible only on exact ties.
- `leaf()` in `galactic_ai_tokens.cpp` matches the hit rule, the stamp-0 rule and the frame unit [H]. The lifetimes
  above are the ones to check at its call sites (4 / 3 / 3 / 2 / 13).
- No change to the order or to the cache is supported by the code. Do not special-case Offensive before
  Infrastructure. Look at the port's Resolve of Infrastructure at 99 instead: A after the reweight and after the
  kept goals are re-added, the fund test for the Naboo mine, and the Naboo ground queue's stored time (Q5).

### Open

- Which Resolve test rejects (or never reaches) the Naboo mine in retail at that Commit. A retail trace of the
  Infrastructure pool A and of the proposals drawn at the second Commit would settle it.
- The node order of the `AIGoalCategoryType` map (tie order).
- Whether starting a production item raises 0x3a51e0 → 0xE for the owner (early refresh of planet `HasStructure`).

---

## Q7. Retail run with a human Empire (2026-11-13): what decides t03 and t04

Run `tools/probe_mod/a2f_budget/out/20261113-053156` (`run_a2f_empire.cmd`, README section 11; Heart of the Maw as
Empire, Medium, hands off, last frame 9469, kit exit 0). Measurements are [H]; the readings of them are tagged.

### Q7.1 The Rebel's Naboo mine (t03)

| frame | retail | 
|---|---|
| 0..55 | Rebel wOFF 7.533, wINFRA 19.910 in every sample: the port's values. Infrastructure is resolved first, as Q6 says |
| 55 | `BuildEconomicStructurePlan` starts twice (MonCalimari, Kamino); mines begin at 89 and 98 |
| 97 | a third `BuildEconomicStructurePlan` starts (unalloc 500). **The mine goal is accepted at this Commit**, so the time stop of Q5 does not reject it here |
| 109 / 140 / 156 | Corellian_Gunboat at Naboo (space), Rebel_Light_Tank_Brigade at Yavin, **Rebel_Tank_Buster_Squad at Naboo** |
| before 520 | the plan of 97 is gone with no end record (released by the goal system; the next start of the same script says prev=none) |
| 520 | the plan starts again; 559 Rebel_Infantry_Squad begins at Naboo (the tank buster ended at 517) |
| before 733 | released again |
| 733 | third start; the infantry ends at 800; **the mine begins at Naboo at 807**, ends 1492, plan exit 1498 |

Reading:
- `BuildEconomicStructurePlan` sleeps 1 s before `AssembleForce`; `BuildGroundForcesPlan` calls `Produce_Force()` at
  once and with no destination. The ground tasks therefore exist about 30 frames before the mine's task [H scripts].
- The tank buster is built at Naboo, the one Rebel planet with barracks and an idle ground queue (Kamino and
  MonCalimari are building mines and refuse a funded task, Q5.4; Yavin has no barracks). **A task made by
  `Produce_Force()` without a destination is offered to every planet: no path test and no travel term** [H behaviour].
  This settles `A2L_T03_RE_ANSWERS.md` Q5.3 / open item 3 (BT +0x98 null): the planet is the lowest bidder, not the
  selection's source planet PP+0x180.
- Naboo claims one task per market run (Q5.3), the tank buster first because it is older on the pending list. The mine
  is claimed behind it; at the next Commit (196) the kept mine goal is re-solved with Naboo's ground queue above
  4.8 s, so ETA > AllowedTime (28.8 s) and it is released. The same happens once more behind the infantry. The time
  stop of Q5 is the mechanism, but at the **later** Commits, not at 97 [M: the Commit frames are taken from the plan
  starts 55 / 97 / 196 / 307 / 520 / 733].

Port before the change: the ground tasks had `dest` = the selection's source planet (Kamino, MonCalimari), Naboo
failed the delivery path test for them and never bid, so the mine was alone at Naboo and started at 164; the tank
buster started at Kamino at 870. Port after the change (`create_build_tasks`: no stage → no destination, except the
spawn place of a magic plan), seed 0: tank buster at Naboo 133, mine plan released at 197, restarted 522, infantry at
Naboo 536, released 621, restarted 736, mine at Naboo 784, done 1474. The remaining 23 frames against retail (156 /
559 / 807) are the phase of Naboo's ground queue, which is drawn from the synced generator at galaxy build (tier 3).

12 seeds (`tools/parity`, `--seeds 12`): `econ.income_cycle1[Rebel]` 814.7 in every seed → 634.2 in 10 seeds (retail
634.2), `[Underworld]` 2240 → 2150 in 10 seeds (retail 2150); both PASS. `outcome.planets_final[Empire]` 3.75 → 4.0
(the Empire lost Tatooine in 3 seeds, retail never).

### Q7.2 The Underworld and Conquer_Opponent @Tatooine (t04)

- Channel D, Underworld > Tatooine: `Is_Good_Invasion_Target` 8.25 (0..90), 8.86 (120..360), 9.26 (390..600), 19.256
  from 900 on. `First_Attack_Allowed` is 0 up to 6600 and **1 from 6900**; `Should_Perform_Unrestricted_Grab_Space` is
  0 throughout; **`Should_Conquer_Opponent_Planet` is 0 up to 6600 and 24.07 from 6900** (= 1.25 × 19.256). So retail's
  desire is not zero: no factor of the equation differs from the port's.
- Budget at the Commit of 6841 (the first after the attack age): credits 2442.0, **Offensive pool 1487.8**,
  Infrastructure 954.2. Production: Underworld_Saboteur_Team at Felucia 6884, **Crusader_Gunship at Felucia 6886
  (1400)**; Offensive pool 236 at 7039. A second Crusader_Gunship at 8219 after the next payments.
- In the Q2 reference runs every Underworld `Build_Space_Forces` start but the first has unalloc 1400 (one
  Crusader_Gunship), and the Underworld starts no Crush / Conquer_Opponent plan in 54000 frames. The port's
  Conquer_Opponent @Tatooine proposals cost 1600 (ConquerOpponentPlan) or 1720 (CrushPlan) net.
- Reading [M]: at 6841 the goal passes its desire test in retail too, but the Offensive pool (1488) funds only the
  1400-credit `Build_Space_Forces`; whether Conquer_Opponent starts is the fund test 0x6129c0 on that pool, not a
  perception factor. Nothing to port.
- Retail's three reader-2 reference runs are one trajectory: their production starts are identical frame for frame
  after removing the start offset (62 / 55 / 1023). This run follows it up to about frame 1500 and then differs (first
  difference: Rebel_Tank_Buster_Squad instead of Rebel_Infantry_Squad at Kamino at 1524). The retail "distribution"
  of `scenarios.json` is therefore two samples, not four.

Port, 12 seeds after the Q7.1 change: the Underworld starts Conquer_Opponent @Tatooine in 3 seeds (was 4), first at
6989 / 8221 / 9591, and takes no Empire planet. `outcome.planets_final` is Rebel 5 / Underworld 3 (retail's outcome)
in 6 seeds and Rebel 4 / Underworld 4 in the other 6: the Rebel does not take Shola (seeds 0, 2, 4, 9) or loses it
again to an Underworld Conquer_Pirate @Shola (seeds 7, 10). Before the change the Rebel also held Shola in 6 of 12
seeds; that count did not move. It is the outcome of the Rebel's Conquer_Pirate / Conquer_To_Reconnect attack on
Shola, graded against a retail side with one outcome.

### Open

- Why the Rebel fails to take or hold Shola in half of the port's seeds (retail: taken near 7500 in both
  trajectories). Filed in the backlog from bkg3.
- More retail trajectories for `hotm_galactic`: a run with a different kit perturbs the draw sequence enough to give
  a new sample (this run); the references need such runs up to tick 13501.

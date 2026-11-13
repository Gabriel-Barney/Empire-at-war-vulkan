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

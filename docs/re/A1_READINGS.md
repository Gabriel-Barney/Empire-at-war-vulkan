# A1 readings: tactical AI provisional readings settled (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe (Ghidra 12.2 export in `re_reference/`). Prose and tables only; no code.
This file settles the provisional [M] / [L] readings in the A1 row of `docs/PHASE4.md`, which the code marks
"PROVISIONAL" in `engine/ai/tactical` headers. It does not repeat AI_TACTICAL (AT), AI_TACTICAL_PERCEPTION (ATP) or
TACTICAL_GAPS (TG). Read it alongside them. Duplicate zone names (A1 t03) are covered in TG §6.8.

Conventions:
- Addresses are RVAs (image base 0x140000000).
- Confidence tags: [H] read directly; [M] inferred from strong evidence; [L] guess. **probe** means the item needs an
  in-game run.
- F = mode frame counter. GC = GameConstants (base 0x140b15800).
- Zone offsets refer to the AIHintZoneClass layout in TG §6.8:
  - +0x50: passability cell set (child 7);
  - +0x90: threat-cell list;
  - +0xd0: hint map;
  - +0x110: link map.

## 0. Status

| § | Reading (PHASE4 A1 row) | Verdict | Confidence now |
|---|---|---|---|
| 1 | Hint zones = .ted 0x10B AI zones [M] | confirmed | [H] |
| 2 | Land zone-graph link direction [M] | confirmed: Edge_Weight(i→j) needs zone j's link map to hold zone i, and uses n(j) | [H] |
| 3 | Path threat ≤ tolerance per zone after the source; path capped at 1000 zones [L/M] | **corrected**: a single test on the wrapping sum of the path weights (threat = high 16 bits × 100); 1000 caps the entries summed; cost = low 16 bits / 10 | [H] |
| 4 | Position → zone = nearest zone centre [L] | **corrected**: the first zone in list order whose cell set holds cell ((x+10)/20, (y+10)/20) | [H] |
| 5 | Reachability test after the evaluation in Reachable_Target [M] | **corrected**: the test runs before the evaluation | [H] |
| 6 | Space admission = land list minus Stay_In_Transport [L] | **corrected**: a separate list of conditions | [H] |
| 7 | Damage-tracked amount = DamageResult::after_multipliers [L] | **corrected**: the actual hull + shield loss (no overkill); plus team key and type gates | [H] (type-flag names [M]) |
| 8 | Space grid bounds = MapData::playable_box [M] | **corrected**: the space **camera** box (0x103 minis 07/08) | [H] |
| 9 | FreeStoreService F = 600 vs ~20.1 s | settled statically: first eligible at F = 600 (unit service F = 61), then the next 3-frame Execution service | [H] static; probe optional |
| 10 | memory_counted_when_not_fogged (ATP open 3a) | confirmed | [H] |
| 11 | `snapshot −= v` on add and remove (3b) | confirmed, with loop order | [H] |
| 12 | Zone threat-cell list = packed cells read as (col, row) [M] | **corrected**: each cell → world (x·20, y·20) → threat cell, de-duplicated, in hash-set order | [H] |
| 13 | nFow rounding floor(x + 0.5) [M] | **corrected**: rounds away from zero (ceil for x ≥ 0) | [H] |
| 14 | First re-projection check at registration + 300 [M] (ATP open 8) | **corrected**: random stamp in the past; check at the first service; +300 per service | [H] |
| 15 | Destroyed objects leave every player's memory lists [M] | **corrected**: memory entries are removed only for players that see the object as fogged | [H] |
| 16 | Concentration per entry, hard-point value [M] | **corrected**: per entry, but with the representative type's AI_Combat_Power; no self exclusion | [H] |
| 17 | Get_Most_Defended_Position best starts at 0 [M] | **corrected**: best starts at −1.0, so the first cell is taken without a draw; every later tie draws | [H] |
| 18 | TimeLastSeen computed directly [M] | confirmed | [H] |
| 19 | Land transport-landing objects always added [M] | **corrected**: friendly-flag and game-state gates; added with no attenuator | [H] gates / [M] gate meaning |
| 20 | HardPointHealth = mean of that type [L] | confirmed | [H] |
| 21 | IsValidBombingTarget (location 0) [L] | **corrected**: a real test (fog, base shields, cover radius) for unit and location | [H] call / [M] meaning |
| 22 | Hints [L] | **corrected**: built from zone minis 08/09; no shipped FoC zone has any | [H] |
| 23 | IsType [L] | **corrected**: global evaluator keyed by the object's own type name id | [H] |
| 24 | Location.ForceUnnormalized of a land unit = point danger [M] | **corrected**: zone danger whenever the evaluator holds a zone | [H] |
| 25 | Reachability source = the AI's home zone (port choice, follow-up question) | **corrected**: there is no home zone. Each call site has its own source, and the goal-proposal filter has no source at all (a per-zone flood around the target) | [H] |
| 26 | TimeLastSeen land cells with flags & 0x22 count as 0 | **corrected**: 0x22 = Steep \| Water in the passability cell's **base** word; such cells (and cells outside the passability grid) are left out of both the sum and the count | [H] |
| 27 | Fog at battle start | everything starts fogged with last-seen 0; own and allied REVEAL units uncover their circles on their first service; no start radius and no map-wide reveal for AI players | [H] / [M] where tagged |
| 28 | Free-store entry +0x30 = the `Lock_Current_Orders` order lock [M] (a1f t03) | **corrected**: +0x30 is the owning PotentialPlan pointer. Set when a goal is accepted or kept in Resolve_Category (0x6c2d60), not at proposal; cleared at every Commit start (0x6c0be0) and by the PP destructor. `Lock_Current_Orders` is an object counter (+0x3a8) that On_Unit_Service does not test | [H] |
| 29 | TEAM containers in the threat grid (a1f t04) | teams with UNIT_AI get one whole-object entry, no TEAM exclusion; members (no UNIT_AI) get nothing. 0x396380(team) = the company type (team data +0x58), 0x3973b0 = `Create_Team_Type`. Power = the company's load-derived sum × clamp(`Max_Squad_Size` / `Company_Units` count, 0, 1), a static share, not live fill (stormtrooper team: 126 × 0.5 = 63); radius from the first member type (140) | [H] |
| 30 | A1 [M] / [port] choices reviewed (a1f t06) | **change**: §12 cell order (exact stdext hash_set emulation; zone +0x90 order differs from evaluator +0xb0 order), §7 clamp the recorded hull loss, §19 gate (game type 1..4 or no parent id, else the parent object's type), §21 bombing test gets the real BASE_SHIELD test and a 3-D prevention distance, Reinforce placement (8-spot ring per ranked point). **Keep**: §15 full removal, §14 stamp draw point, Reinforce mixed keys (the original's), TF link mask −1 (+0x94 is a CellPassabilityType mask, unset in every shipped plan); IsInside*Shield leaves are always 0 in shipped data | [H] / [M] where tagged |
| 31 | Why the original's AI-owned story fleet never invaded (S10 PROBE A, human Empire; a1g t05) | **explained**: Lua `fleet.Invade()` (GameObjectWrapper 0x57ada0) executes an InvadeEvent stamped with the **local** player, and the executor 0x48ff50 keeps only units owned by that player. A fleet the local player does not own lands nothing, silently. Not the Move_To block, not the AI taking the fleet | [H] |

---

## 1. Hint zones are the .ted 0x10B zones [H]

- Reading chain:
  - The land mode's terrain post-load step is LandModeClass vf46 0x3b8bc0 (mode vtable +0x170, the TERRAIN_WATER_POST
    step).
  - It calls the zone reader 0x5a9100, which reads the 0x10B chunk into AIHintZoneClass objects in chunk order.
  - These objects fill the land mode's hint-zone list (+0x3a0).
- The layout and the id remapping are in TG §6.8.
- Links resolve by the 02 editor id through the pointer-remap table, never by name.

## 2. Link direction [H]

Edge_Weight(i → j) (0x694010):
- The edge exists only when zone **j**'s +0x110 link map contains zone **i**.
  - The lookup (0x5a9af0) uses mask −1, so any non-zero flag counts.
- hop = (uint32)(int64)(3-D centre distance / 10.0) & 0xFFFF.
- weight = n(j)·65536 + hop, or 0x10000 + hop when n(j) = 0.

So the direction already in `zone_reachability.hpp` is right; raise its tag to [H].

Shipped-data check (FoC + Patch2 land maps):
- There are 47 748 links. Every flag value is non-zero (seen: 1, 2, 3, 4, 32, 34, 128, 256), so no link is dropped by
  the mask.
- Only 2 links in one map have no reverse link. Getting the direction wrong would therefore change almost nothing in
  shipped data.

## 3. Path threat tolerance (ZoneReachabilityClass slot 7, 0x73f5f0) [H]

### 3.1 Preconditions

Slot 7 returns 1e18 ("unreachable") unless all of these hold:
- the source target has a zone (target vf+0x40 non-null);
- the context's mode is land;
- the reachability level is 6..10;
- the execution path finder exists.

### 3.2 Tolerance per level

| Level | Tolerance |
|---|---|
| No_Threat | 0 |
| Low_Threat | GC +0x157c |
| Medium_Threat | GC +0x1580 |
| High_Threat | GC +0x1584 |
| Any_Threat | 1e18 |

### 3.3 Path and sum

- The path is Get_Path(sourceZone → thisZone) (0x690f10), a list of per-hop all-pairs weights. The final entry is 0.
- The sum (0x691250) is S = uint32 wrapping sum of the entries [0, min(1000, length − 1)).
  - 1000 limits how many entries are summed. It does **not** refuse long paths.

### 3.4 Acceptance and returned cost

- Acceptance (0x6913a0): accept iff (float)((S >> 16) · 100) ≤ tolerance.
  - The high half is the summed node costs n(j) plus one per hop whose n(j) = 0.
- Returned cost (0x691370): (float)(S & 0xFFFF) / 10, which is the summed hop distances in tenths.

### 3.5 Consequences

- Every hop adds at least 1 to the high half, so a path's threat is at least 100 × hops.
  - No_Threat accepts only source zone == target zone (S = 0, cost 0).
  - Low_Threat at its shipped value of 1000 allows at most 10 zero-threat hops.
- An unreachable target gives an empty path, so S = 0xFFFFFFFF:
  - threat = 65535 × 100, which fails every level except Any_Threat;
  - with Any_Threat it passes with cost 6553.5.
- The test is on the **sum** over the path, not on each zone's threat.
- The low half can carry into the high half when the summed hop distances exceed 0xFFFF tenths. That carry is part of
  the original's arithmetic.

## 4. Position → zone [H]

- Land mode vf48 0x3b7f50:
  - cell = ((int)((x + 10) / 20), (int)((y + 10) / 20)), truncated;
  - a position outside the passability grid gives no zone (null).
- Otherwise vf55 0x3b7c10 returns the **first zone in hint-zone list order** whose +0x50 cell set contains
  (cellx | celly << 16).
  - Zones that share a cell resolve to the earlier zone.
  - A cell in no zone gives null.
- No distance to zone centres is involved.

## 5. Reachable_Target order (0x698360) [H]

For each candidate target, the steps run in this order:

1. Applies test.
2. **Reachability**, only when the target has a reachability object (target vf+0x38) and a source is given:
   - if its slot 7 returns 1e18, the target is skipped.
3. Binding and Evaluate.
4. Range test.
5. Selection.

So a filtered target is never evaluated. It makes no leaf-cache stores and no `#` random draws. The port must move the
reachability test ahead of the evaluation.

## 6. Free-store admission [H]

### 6.1 Space (0x6904b0)

Space admission is a separate list of conditions. It is **not** "the land list minus Stay_In_Transport". An object is
admitted when all of these hold:
1. Its type is neither a squadron member (type +0x29) nor a company member (type +0x2a).
2. It qualifies by one of:
   - SPECIAL_WEAPON behaviour (0x2F; obj +0x361);
   - or all of: not Is_Dummy, has STARSHIP_COMBATANT (0x13) or TEAM (0x16), and has a locomotor.
3. Its type lacks the DUMMY_ORBITAL_STRUCTURE (0x60) and DUMMY_STAR_BASE (0x5C) behaviours.
4. It has no TACTICAL_SUPER_WEAPON (0x2C; obj +0x35e).
5. Its owner is the AI player.
6. obj +0x2b0 and +0xf8 are empty (not contained or attached).
7. It passes the common base test 0x4d2c90.

The space list has **none** of these checks:
- TRANSPORT;
- Create_Team;
- Stay_In_Transport;
- INDIGENOUS_UNIT;
- DUMMY_GROUND_BASE;
- the bombing-run manager set.

### 6.2 Land (0x690a10), for comparison

An object is admitted when all of these hold:
1. Create_Team is 0.
2. It has no TRANSPORT.
3. It qualifies by one of:
   - LOBBING_SUPERWEAPON (0x37);
   - or all of: its type lacks DUMMY_GROUND_STRUCTURE (0x5F), it has STARSHIP_COMBATANT or TEAM, it has a locomotor,
     and it is not Is_Dummy.
4. No INDIGENOUS_UNIT (0x20). For a TEAM, its representative type and every member type must lack 0x20.
5. Its type lacks DUMMY_GROUND_BASE (0x5B).
6. Its owner is the AI player.
7. Stay_In_Transport (+0x2d) is 0.
8. obj +0x2b0 and +0xf8 are empty.
9. It is not in the land mode's +0x38 manager set (the bombing-run manager, 0x4944e0).

Note: AT §5.1 calls the 0x20 / 0x5B / 0x5F tests "type bits". They are behaviour-in-type tests (0x370320).

## 7. Damage-tracked amount [H]

### 7.1 When an amount is recorded

- Take_Damage (0x3a9e30) calls the tracker update 0x58bc70, but only when the object has DAMAGE_TRACKING (obj +0x370).
- amount = (shieldBefore + healthBefore) − healthAfter − shieldAfter.
  - This is the actual hull + shield loss; overkill beyond the remaining hull is excluded.
  - The shield values are read (object +0x1e → +0xfc) only when the object is SHIELDED (+0x342); otherwise they
    are 0.
- The amount is recorded only when it is > 0.

### 7.2 What is updated

- Running total +0x8 += amount.
- Last-damage frame +0x50 := F.
- A per-attacker entry is added or updated only when the damage source's type has both +0x6b and +0x41 set.
  - [M] names: Is_Valid_Target and a combat-unit flag.
  - Entry key = the source object's id. When the source has a +0x2b0 parent that has TEAM, the key is the parent's
    id instead.
- vf16 0x58b920 removes one attacker's entry.
- 0x58ba40 is the time-till-dead estimate.

## 8. Space region-grid bounds [H]

- The space region grid (0x6b9da0) takes its rect from mode vf+0x208 = vf65 0x4d77e0.
  - That returns space mode +0x3a8.., the **camera box**: .ted 0x103 minis 07/08.
  - Its default is the playable box shrunk by 400 in X and Y (SPACE_AND_OPEN A.1).
- The playable box (minis 04/05 → +0x390, getter vf63 +0x1f8) is what the **threat grid** uses. The region grid does
  not use it.
- `map_bounds` must return the camera box for the space region grid.

## 9. FreeStoreService timing [H static]

### 9.1 Clock and comparison

- The script clock is now = (float)F × 0.033333335f (the float at 0x140b0a344).
- The comparison is done in double: fire when rate ≤ now − LastService.

### 9.2 Frame values

| F | now | Result |
|---|---|---|
| 600 | 20.0000019 | ≥ 20: the first eligible frame is **F = 600**, not ~20.1 s (AT §5.3 is wrong there) |
| 1200 | 40.0000038 | next service |
| 60 | 2.0 exactly | On_Unit_Service (rate 2): the comparison is not passed at F = 60, so the first eligible frame is **F = 61** |

- The service actually fires at the first Execution service (3-frame cadence) at or after those frames. [M] The F = 63
  quoted in PHASE4 fits an Execution service on multiples of 3 (the player's cadence phase was not traced).

### 9.3 Script start order (0x4d31e0)

1. Load the script.
2. Run Base_Definitions (Common_Base_Definitions sets LastService = nil).
3. Start the main thread (0x244740). [M] Whether this immediately runs main's first FreeStoreService call is not
   settled.
4. The engine sets LastService = 0.0 and LastUnitService = 0.0.

A nil or non-number LastService means "fire now".

The probe (AT §12 item 10) is now only a confirmation.

## 10. Cell-force memory entries counted when not fogged (ATP 3a) [H]

- In CellForce 0x6545d0, the memory-list branch skips an entry only when the object has UNIT_AI **and** 0x4f57d0
  reports it fogged for the player.
- Every other memory entry is counted, with the value from 0x655e50.
- 0x4f57d0 reports "fogged" when the player index is ≥ the fog array size.
- The double count is real; keep `memory_counted_when_not_fogged` = true.

## 11. Snapshot projection (ATP 3b) [H]

### 11.1 When the snapshot is updated

- 0x656a10 applies `snapshot −= v` (on add as well as remove) only when the object lacks both HIDE_WHEN_FOGGED
  (+0x34b) and TEAM (+0x348).
- Add does not clamp. Remove clamps.

### 11.2 Value and loop order

- v = power × ((cellW · cellH) / (r · 4 · r)).
- Loop order:
  1. rows outer, columns inner;
  2. per cell, players in player-list order (hostility test 0x39a4e0);
  3. then the live-list append.
- A destroyed destroyable hard point gets r = −1 and is not projected.

## 12. Zone threat-cell list [H]

The land-location evaluator vf13 (0x6d65d0) builds the list:

1. It walks the zone's +0x50 passability cell set in that set's list order.
2. For each cell it takes the world point (x · 20, y · 20) (globals 0x140b36b98 / 0x140b36b9c).
3. It maps that point to a threat cell (0x654210: truncate, clamp to the grid).
4. It inserts the threat cell into the evaluator's +0xb0 set, skipping duplicates.
   - The set is an MSVC stdext hash_set (0x5a8770), key = col | row << 16.
5. It copies the set into the zone's +0x90 list (0x6d66a0).

Consequences:
- The list holds **threat-grid** cells, de-duplicated. These are not the packed passability cells read as (col, row).
- Its order is the stdext hash_set list order:
  - hash (key ^ 0xdeadbeef) & 0x7fffffff, then a minstd step;
  - grouped by bucket, sorted within a bucket.
- That order matters for the float sums (zone danger) and for the tie draws in §17.
- The port either reproduces the order or records it as a [port] deviation.

## 13. nFow rounding [H]

- 0x22f9f0 rounds away from zero: ceil for x ≥ 0, floor for x < 0.
- nFow = ceil(extent / DesiredFOWCellSize), clamped to 0..512.
- When preset cell sizes are given, those are used instead.

## 14. First re-projection check (ATP open 8) [H]

- The UnitAI data-pack constructor (0x550ed0) sets two stamps, consuming **two synced draws per UNIT_AI data pack**,
  in this order:
  1. +0x20 = F + Get_Int(0, 300) − 300;
  2. +0x34 = F + Get_Int(0, 150) − 150.
- The service (0x4f6070) runs when +0x20 ≤ F and the threat grid exists:
  1. It registers the object with the grid if needed (0x6537d0).
  2. It compares the transform (+0x248..+0x274) with the stored copy (+0x68..+0x94).
  3. If they differ, it re-projects (0x659760).
  4. In both cases it does +0x20 += 300.
     - It does not re-base on F, so a stamp that is behind catches up by one step per service.
- The first check is therefore at the object's first service, not at registration + 300.

## 15. Destroyed objects and memory lists [H]

- 0x6585f0 removes memory entries only for players for which the object is **currently fogged**. It uses that
  player's snapshot rect.
- For other players the memory entries stay until the cell refogs. The original then reads stale data.
- After that, 0x656d10 removes the live-list entries, and the object leaves the owner list (+0x88).
- The port's full removal is a [port] deviation. It is safer and should stay, but it should be labelled [port].

## 16. Concentration (0x656800) [H]

### 16.1 Which entries count

- Live entries count when the entry's centre cell == the cell. An entry is skipped when it has UNIT_AI and is fogged.
- Memory entries count only when fogged, matched on the snapshot centre cell (+0xa0[p]).

### 16.2 Value and filters

- Each counted entry adds AI_Combat_Power of the object's **representative type** (0x396380), not the hard-point
  value.
- There is no exclusion of the location's own unit and no faction filter beyond these:
  - the category mask;
  - the owner's faction is not neutral;
  - relation == allyFlag.

## 17. Get_Most_Defended_Position [H]

Both versions behave the same way:
- Land is 0x6556c0 and iterates the zone threat-cell set (§12 order). Space is 0x655400 and iterates rows outer,
  columns inner over the rect.
- best starts at −1.0f, so the first cell is always taken, without a draw.
- Every later tie draws, including 0 == 0 ties: Get_Float01 > 0.5 switches to the new cell.
- If the grid mask (+0xc8) & the category mask is 0, it returns the 1e18 sentinel and makes no draws.

## 18. TimeLastSeen [H]

- 0x72e3c0 calls the evaluator's slot 10 directly, not the cached Unnormalized leaf.
- result = v × (double)30 / (double)F. F = 0 gives inf or NaN, as in the original.

## 19. Land transport-landing sums (0x6d4180) [H gates / M meaning]

### 19.1 When the extra objects are queried

After the base player sum succeeds, the function queries TRANSPORT_LANDING (0x36) objects.

### 19.2 When an object is included

An object is included only when all of these hold:
1. 0x39a2c0(obj, player, 0) equals the friendly flag asked for.
2. At least one of:
   - 0x28b060 / 0x28af80 holds. [M] These read the game mode manager: game type 1..4 with +0x38 set and +0x20 ≠ 1.
   - obj +0x54 == 0x3fffff.
   - 0x28aa50 holds. [M] This is "the mode has a conflict parent".
3. Its representative type's category matches the mask.

### 19.3 Value

It adds AI_Combat_Power with **no** attenuator or health factor.

## 20. HardPointHealth (0x6d28b0) [H]

- Without a Hard_Point_Type, it fails.
- Otherwise it is the double-accumulated mean of 0x385a70 over that type's per-type list (+0xb8 + t · 0x18).
- An empty list gives 0. No object means it fails.

## 21. IsValidBombingTarget [H call / M meaning]

### 21.1 The two versions

Both versions need the land mode; otherwise they fail. Each returns 1 iff all of these hold:
- the mode's +0x38 manager exists;
- Self exists;
- 0x494540(manager, Self id, pos) holds.

| Version | Address | pos |
|---|---|---|
| Unit | 0x6d72c0 | the object position |
| Land location | 0x6d5860 | the zone centre, after re-linking a unit-bound zone; no zone → 0 |

### 21.2 What 0x494540 tests

All of these must hold:
- the position is not fogged for the player;
- it is not inside an active BASE_SHIELD (0x39; type +0x21f4 radius; state 1);
- no object of the player whose type has a +0x1dfc radius > 0 covers it.

## 22. Hints [H]

- The Hints evaluator is built by 0x6deb90 from the zone's +0xd0 map (minis 08/09).
- A name is kept only when its name id exists and passes 0x4dd6d0.
- With no zone, the evaluator is empty (0x6dee50).
- Minis 08/09 appear in 36 EaW zones and in **no** FoC shipped zone. So `Hints.*` paths fail on FoC maps. [M] This
  matches FoC scripts not depending on hints.

## 23. IsType [H]

- The Type sub-evaluator (0x6d8040) is the global evaluator 0x4dd450 (map at 0x140b31910).
- It is keyed by the name id of the object's **own** type (obj +0x298), not the representative type.
- The per-type script evaluator is shared with the galactic side (AI_GALACTIC_TOKENS).
- A type with no evaluator makes the path fail.

## 24. Location.ForceUnnormalized (0x72cf10) [H]

### 24.1 Which danger is returned

- When the evaluator holds a zone (+0xf0), it returns the zone danger (0x653e90 over the +0xb0 threat-cell set).
  - This holds even when the evaluator is unit-bound: it uses the stored zone, last re-linked.
- Point danger is used only when there is no zone:
  - at the unit's position;
  - at the rect centre when there is no unit.

### 24.2 Self

Self = binding +0x58 → +0x50. [M] That the zone sum receives Self.

---

## 25. Reachability: sources, tolerances and the two tests (follow-up, 2026-10-31) [H unless tagged]

### 25.1 Two different tests

ZoneReachabilityClass (vtable 0x8cf750; one per land region target) answers reachability in two ways. Both
use only levels 6..10 (No / Low / Medium / High / Any_Threat) and the tolerance table of §3.2.

| Test | Slots | Source | What is compared with the tolerance |
|---|---|---|---|
| **Path sum** | slot 7 (0x73f5f0) | a second target given by the caller (its zone, target vf+0x40) | the wrapping sum of the Get_Path weights from the source zone to this zone (§3): one test over the **whole** path (up to 1000 entries), threat = high 16 bits × 100 |
| **Flood** | slot 5 (0x73f8c0, "is the source zone in my set"), slot 6 (0x73f710, "pick a zone from my set") | none: the set is built around **this** target's zone | each zone's own zone danger (0x653e90, the raw sum over the zone's threat cells, AI player [M which player field]) separately; a zone above the tolerance is not entered, so the flood stops there |

### 25.2 The flood (0x740430 → 0x5a7880 → 0x5a7390)

- Per (target, level) a cached set: a list of zones in BFS order plus a bit set by zone index.
  - Rebuilt when used if it was never built or is more than 10 s old (frame − stamp > 10 × FPS).
  - Built only in land mode.
- Graph: a global zone graph (0x140a28810, built once by 0x5a7da0):
  - nodes = the land mode's zones in list order;
  - **undirected** edges: for each zone i, for each zone j in i's link map (any flags) with index j > i, add the
    pair i–j to both neighbour lists. A link stored only in the higher-index zone is ignored. Shipped data has 2
    such links (§2).
- A zone passes when its danger == 0 or danger ≤ tolerance.
- Start = this target's zone:
  1. If the start passes: BFS from it. Neighbours are visited in list order, and each passing neighbour is
     added. The start is always first in the list.
  2. If the start fails: each neighbour in turn, skipping visited ones:
     - a failing neighbour is marked visited;
     - a passing one runs its own BFS.
     The largest of these BFS results is kept, and the start zone is not in it.
  3. The set is empty only when the start zone **and every direct neighbour** fail, or when the zone is not in
     the graph.
- Slot 6 (level, linkMask, exclude, include):
  - with linkMask −1 and no lists, it returns the target of the first zone in the set;
  - otherwise it returns the target of the first zone in set order for which all of these hold:
    - this zone's link map holds that zone with (flags & linkMask) ≠ 0;
    - the zone is not in `exclude`;
    - the zone is in `include` when `include` is given.
  - Empty set or a level outside 6..10 → null.
- Slot 5 (source, level, noRefresh) = bit test of the source target's zone index in this target's set. It
  refreshes the set first unless noRefresh is set.

### 25.3 Call sites in tactical (land)

| Call site | Test | Source (path FROM) | Level / tolerance | Effect |
|---|---|---|---|---|
| **Goal proposal skip**, land slot 9 (0x6b8710) | flood, slot 6 (mask −1, no lists) | **none** | the goal type's `Reachability` (goal type +0x2c) | the candidate (type, target) is skipped when the target's region set is empty, i.e. the target zone and all its neighbours have zone danger > tolerance. No path from any AI unit or home is measured. A target without a reachability object (target vf+0x38 null) is **not** skipped by this test; a region target with a level outside 6..10 always is (slot 6 returns null) |
| **Lua `FindTarget.Reachable_Target`** (0x698360) | path sum, slot 7 | the 6th argument: an AI target, or a game object / task force wrapper resolved through the target list (vf+0x40) to its target [M resolution]. **No 6th argument → no reachability test at all** | the 4th argument | candidates whose slot 7 returns 1e18 are skipped before Evaluate (§5). Only `AI_Plan_Underworld_TyberManagerLand.lua` passes a source in shipped tactical scripts (the Tyber unit; level `threat_threshold`). BusyTacticalFreeStore, the hero assist scripts and the evaluators pass none and use `Any_Threat` |
| **Tactical FreeStore Setup A** (0x7287a0), stage choice per TF | flood, slot 6 | none (the set is around the goal target's zone; when the goal target has no reachability object, the region at the goal target's position via target-list vf+0x48) | the goal type's `Reachability`; linkMask = TF definition +0x94; exclude = stages so far when TF +0x98 (RequireUnique); include = them when +0x99 (MatchingStage) [M names] | stage = the slot-6 result, or the goal target when null |
| Tactical FreeStore Setup A, distance key per free unit | path sum, slot 7 | the region at **each unit's position** (target-list vf+0x48 on unit +0x78) | always **7 (Low_Threat)** | not a filter: (cost)² is added only when finite. If no stage is finite, the key is the 2-D distance² to the goal target |
| **TF.Reinforce landing points** (0x6eaf60) | flood, slot 5, then path sum, slot 7 | the region at **each candidate reinforcement point** | the TF goal's `Reachability` | points whose zone is not in the destination's set are dropped; the rest are sorted by the slot-7 cost. Without a region on either side, the 2-D distance is used |
| `TF.Move_To(dest, threat, useZonePath)` (0x73c360 → 0x73bc50) | zone route through the global zone graph (0x5a6d00), tolerance = threat × 2 | the region at the TF position | the 2nd argument | only with `useZonePath` true. No shipped land plan passes the 3rd argument, so the threat number has no effect in shipped data |

No other tactical caller compares a slot-7 result with 1e18 (a search for 1e18 compares over the AI code range finds only
0x698360 and 0x7287a0).

### 25.4 Consequences

- The original has no "home zone". Plan proposal never measures a path. A Low_Threat (1000) goal on a land region
  is refused only when that region's zone and all its neighbours each hold more than 1000 hostile zone danger.
- The path-sum rule of §3 (≥ 100 threat per hop) only acts where a source is given: Tyber's Reachable_Target, the
  FreeStore distance key (a soft term), and Reinforce point ranking.

## 26. TimeLastSeen and the 0x22 cell test (0x655ed0 rect, 0x6561d0 zone) [H]

This was read from the instruction stream; the decompiler drops the sums.

### 26.1 Per threat cell

For each threat cell (zone: the evaluator's threat-cell set in list order; rect: rows then columns):
1. The cell centre is (x0 + (col + 0.5)·cellW, y1 − (row + 0.5)·cellH).
2. fogged = FOW_Point_Test(mode, Self's player index, centre, force = 1) (0x35f3b0). It uses real fog for AI
   players regardless of `AIUsesFogOfWarLand`.
3. In land mode only (mode vf+0xe0 == 1):
   - Passability cell = (int)((x + 10) / 20), (int)((y + 10) / 20), using the grid header at 0x140b36b90.
   - The threat cell is **skipped** (not counted, not summed) when:
     - the passability cell is outside the grid; or
     - its **base** passability word (cell record +0x10, from .ted 0x10A) has bit 0x02 **Steep** or 0x20
       **Water** (MOVEMENT §1.1 enum).
   - The effective word (+0x14, objects stamped on the cell) is not read.
4. Otherwise count += 1, and when fogged: sum += F − lastSeen[player][threat cell] (int, added as float).

### 26.2 Result

- If sum > 0: result = sum / (count × FPS). Otherwise 0.
- AI_TACTICAL_PERCEPTION's "counts as 0" is wrong: a skipped cell does not enlarge the denominator.

### 26.3 Other users

- A scan of the AI code range (0x590000–0x760000) for tests of 0x22 finds only these two functions.
  - The zone danger (0x653e90), the flood, the path finder node costs and the threat projection do **not** skip
    Steep or Water cells.

## 27. Fog state at battle start [H unless tagged]

### 27.1 Start state

- Land tactical start (FOG_OF_WAR §1.3): every non-neutral player, AI players included, gets a FOW grid.
  - All states start at 0 (fogged) with count 0.
  - The land FOW-active flag is set.
- The threat grid's last-seen arrays (+0x70, per player × threat cell) are sized at creation and filled with **0**
  (0x657fc0).
  - A last-seen stamp is written only by the refog event: a FOW cell going from seen to fully fogged sets
    lastSeen = F.
  - A cell that was never seen keeps 0 and contributes F − 0 = F.

### 27.2 What reveals cells

- Every REVEAL (behaviour 0x18) object is marked dirty at mode start and reveals on its next service.
  - The service interval is 1 frame [M].
  - Revealed circles use `Land_FOW_Reveal_Range` × weather and battle scale, for the owner and every allied player.
  - A count 0 → 1 on a state-0 cell sets the state to 0xEF at once, which is "not fogged" for game logic.
  - So from the first frames, the cells inside the AI's own (and allies') unit and structure reveal circles are
    visible. Everything else stays fogged.
- No start-radius reveal and no map-wide reveal exist for AI players:
  - `SetupPhaseEnabled` is False in shipped GameConstants, so the setup-phase reveal and binarise do not run.
  - Reveal_All happens only through Force Sight, the overrun reveal (multiplayer game types, ≥ 30 s, 3:1 power),
    Lua / story calls, or a loaded save.

### 27.3 TimeLastSeen at the first AI service (frame F)

- For a zone, it is (fogged counted cells / counted cells) × F / 30 s, with Steep / Water / off-grid cells left
  out.
- Normalised TimeLastSeen (§18) = that fraction.
  - 1.0 only for zones with no counted cell inside any reveal circle.
  - Lower for zones near own units.
  - 0 for fully seen zones.
- It is **not** F/30 for every zone.

## 28. Free-store entry +0x30 (a1f t03) [H unless tagged]

### 28.1 Entry layout

- The free store's unit map is at free store +0x40: an MSVC hash list, key = object id, bucket = (id ^ 0xdeadbeef) & mask.
- Each list node is 0x38 bytes (allocated in 0x4d19e0):

| Offset | Size | Content |
|---|---|---|
| +0x00 / +0x08 | 8 + 8 | list next / prev |
| +0x10 | 8 | the game object (key: its id at obj +0x50) |
| +0x18 / +0x20 / +0x28 | 24 | vector of AIBuildTask pointers that claimed this unit (FREESTORE_ALLOC path; filled at admission, pruned by 0x4d4ee0 and 0x4d5150) [M purpose] |
| +0x30 | 8 | **owning PotentialPlan** (PotentialPlanClass*, 0x210 B). 0 = unreserved |

- Admission (0x4d2c90) builds every new entry with +0x30 = 0.
- Removal (0x4d5000, vf+0xa0; galactic override 0x6907d0) erases the whole node. A unit that leaves the store and
  comes back gets a fresh entry with +0x30 = 0.
- The debug owner string 0x5259d0 confirms the meaning: +0x30 = 0 prints "Freestore", otherwise "<goal>(<plan>)",
  read through PP+0x20 (the goal).

### 28.2 `Lock_Current_Orders` does not touch +0x30

- `Lock_Current_Orders` (0x57cd40) increments an object counter, obj +0x3a8 (0x3a8e50(obj, 1)). The free store
  subscribes to the object's signals 10 and 11.
- The free store's signal handler (0x4d4aa0) decrements the counter on signal 10 or 11 (0x3a8e50(obj, 0)) and
  re-applies the default abilities (0x3a82a0). `Unlock_Current_Orders` (0x581fd0) decrements it too.
- 0x39ad40 ("is locked") is true when obj +0x3a8 > 0 (or for some locomotor states).
- The lock is read by FreeStore Setup A: a locked unit is not a candidate.
- **On_Unit_Service does not read the lock.** The loop in 0x4d5a70 tests only entry +0x30.

### 28.3 PotentialPlan fields used here

- PP+0x100: reserved flag. Set by the first 0x6c2d60 call; never cleared.
- PP+0x4E: "reservations suspended" flag. Set by 0x6c0be0; cleared by 0x6c2d60 and by Init 0x6c0d00.
- PP+0x104: solve counter. The constructor (0x6be9c0) sets −1; each solver run (0x6c3da0) first adds 1. So the first
  solve (Init, at proposal) runs with 0.
- PP+0x108: the PP's unit list (one object pointer per selected free unit; null = "to be produced").
- PP+0x198: mode (0 galactic, otherwise tactical).

### 28.4 Writers

| Function | Writes | Called from | Moment in the goal's life |
|---|---|---|---|
| 0x4d2c90 admission | new entry, +0x30 = 0 | free store Add_Object (vf+0x98) and the per-mode admission scans | the unit enters the store |
| **0x6c2b20**, from **0x6c2d60** (reserve / refresh) | +0x30 = PP for every PP+0x108 unit still on the store. It overwrites any other PP's value (no check) | Resolve_Category 0x6c8970: the greedy accept and the weighted random accept, just before Activate; step 3, each active goal that is kept with a valid plan. Maintenance 0x722a30: each surviving goal | **accept** of a new proposal; **re-assert** for kept goals in every Commit |
| **0x6c2320** (unreserve) | +0x30 = 0 where +0x30 == PP (only if PP+0x100) | **0x6c0be0**, Commit_Proposed_Goals 0x6c8710 step 4, once for every active goal with a PP, before any Resolve_Category; the PP destructor 0x6bec20 | start of every Commit: all reservations are suspended. Destructor: the goal is released or its PP replaced |
| 0x6c27d0 (PP, unit) | +0x30 = 0 if == PP. A unit not on the store is re-admitted (vf+0x98, fresh entry) | TaskForce vf49 0x5ff610 (a unit leaves a task force: `Release_Unit`, `Release_Forces`, plan end 0x5f6a50 step 3); AIBuildTask FSM 0x64aec0, state WAIT_FOR_FREESTORE_ALLOC, when the claimed unit is ready | the unit leaves the plan. It is also removed from PP+0x108, so later re-asserts skip it |
| 0x6bf3d0 (PP, unit) | admits the unit if needed; +0x30 = PP; adds the unit to PP+0x108 (into a null slot of the same type, else appended) | ReinforceBlockStatus 0x6a1440 = its signal-0x19 handler ("reinforcements unloaded" from the landing transport), for each unloaded unit (its TEAM container instead, once) of a task force's Reinforce; land only, the space status does not call it (§32.1) | a running plan receives a new unit |
| 0x4d5000 removal | erases the node | vf+0xa0: the free store's signal handler on destroy (0x10) or owner change (0x1a); a task force taking the unit | death, capture, or the TF takes the unit |

- Nothing else writes node +0x30. All users of the map lookup helpers (0x4d2930, 0x4d2940, 0x6be900) were read.
- Unit death needs no separate clear: the node is erased.

### 28.5 Readers

| Function | Test |
|---|---|
| 0x4d5a70 free-store service | `On_Unit_Service(unit)` runs only for entries with +0x30 == 0. No other per-unit test (no lock test) |
| 0x7287a0 FreeStore Setup A | a candidate needs PP+0x104 == 0 **or** +0x30 == 0. The engaged test passes when +0x30 == this PP |
| 0x6c45e0 (from 0x6c47c0, reserved PP) | the plan is **invalid** if a PP+0x108 unit still on the store has +0x30 ≠ 0 and ≠ this PP (stolen) |
| 0x4d3c60 (fs, obj, PP, any) | on the store and (any or +0x30 == PP). Its only callers (galactic `Is_Unit_In_Transit` 0x68f270, `Is_Unit_Safe` 0x68f410, `Move_Object` 0x68f5b0) pass any = 1, so they ignore +0x30 |
| 0x5259d0 | debug owner string only |

Lua `Get_Object_Count` (0x4d4690) and `Is_Object_On_Free_Store` (0x4d48c0) do **not** read +0x30:
- `Is_Object_On_Free_Store(obj)`: map membership only.
- `Get_Object_Count()`: the map size (+0x10). With a boolean argument it walks the list and counts entries whose
  obj +0x337 == −1 (true) or ≠ −1 (false) [M: meaning of +0x337 not read].

### 28.6 Order of events (shared by galactic, land and space)

1. **Proposal** (slot 10, `Find_Plan_For_Goal` 0x6bae50): Init 0x6c0d00 runs the first solve (counter 0).
   - Setup A **ignores** reservations.
   - Nothing is reserved.
2. **Commit start** (0x6c8710 step 4, 0x6c0be0), for every active goal's PP:
   - PP+0x4E = 1;
   - its queue slots are flagged (0x53bee0 with 1);
   - its +0x30 entries are cleared.
3. **Resolve_Category**, per category in budget-weight order:
   - Kept active goals: validity 0x6c47c0 → 0x6c45e0 fails if another PP took a unit meanwhile. Then 0x6c2d60
     clears PP+0x4E, re-flags the queue slots (0) and sets +0x30 = PP again.
   - New proposals at accept: 0x6c4a50 **re-solves** (counter ≥ 1, so units with +0x30 ≠ 0 are skipped and PP+0x108
     is rebuilt). Then 0x6c2d60 sets PP+0x100 = 1 and reserves: the production queue slots, the tactical
     reinforcement-pool and build-pad slots, the tactical credits, and +0x30 = PP on the new PP+0x108 units.
4. **Finalize**: kept goals go to Planning (0x6bad60). Discarded proposals' PPs are destroyed; they had reserved
   nothing.

Consequences:
- A unit is reserved from the accept of its goal until one of: the task force takes it off the store
  (Produce_Force / Collect), it leaves the PP (0x6c27d0), or the goal is released.
- Step 2 suspends every reservation first. So a goal kept in a later category can lose a unit to a goal accepted in
  an earlier category. Its validity check (0x6c45e0) then fails.

### 28.7 Shared code

- Tactical and galactic free stores share all of this. LandFreeStoreClass (0x8b6b68), SpaceFreeStoreClass (0x8b6a38)
  and GalacticFreeStoreClass (0x8b63f8) derive from AIFreeStoreClass (0x878f80). The node layout, admission insert,
  service loop and Lua methods are base-class code.
- The PP functions (0x6c2d60, 0x6c2b20, 0x6c2320, 0x6c27d0, 0x6c45e0, 0x6bf3d0) and FreeStore Setup A (0x7287a0, the
  first marshaller in all three lists) are mode-independent.
- Only their PP+0x198 ≠ 0 branches add the tactical reinforcement-pool reservations (free store vf+0xe0 / +0xe8 /
  +0xf0 / +0xf8) and build-pad reservations (0x4d2b00 / 0x4d34b0 / 0x4d6440, pad table +0xe8..+0x110). Those are
  separate tables, not +0x30.

### 28.8 The movement lock counter obj +0x3a8 (0x3a8e50) and the AI release paths (OI abilities t01, 2026-11-01)

One signed int per game object, obj +0x3a8. Every movement lock in the game shares it: script order locks, ability
`Disable_Movement`, hyperspace arrival and the other callers (39 functions call 0x3a8e50).

**0x3a8e50(obj, lock)** [H]:
- `lock` = 0 (release): counter := max(counter − 1, 0). Nothing else: no move is resumed, no signal, no abilities.
- `lock` ≠ 0: only when the counter is 0 (first lock):
  1. the movement coordinator (mode vf+0x228) and the unit's formation (0x4261d0: locomotor pack obj +0xa8, field
     +0x1f0; none without a locomotor);
  2. with a formation, 0x424850 detaches the unit through the formation's coordinator (+0x220, 0x4f84e0) into a
     formation of its own (only when the unit really is in that formation);
  3. 0x39bcb0: unless a mode flag (vf+0xf8) is set, the unit is inside a container (obj +0x2b0 with +0x348 set), or a
     locomotor-type check fails [M: vf+0x238 against mode vf+0xe0]: when the coordinator accepts the unit (0x425e30)
     and no formation of the coordinator holds it (0x426140), it gets a new formation (0x426010: land or space
     kind by coordinator +0x20 = 1 / 2) and is added to it (vf+0x40).
  Then counter + 1. Later locks only add 1.
- So the first lock splits the unit from its group into a formation of its own. It does not by itself zero the speed
  [M: whatever freezes a Disable_Movement user reads 0x39ad40 elsewhere; not traced].

**0x39ad40 "movement locked"** [H]: true when obj +0x3a8 > 0; or the unit's locomotor (behaviour slot 1, obj +0x333 set)
answers vf+0x1d0 true; or its team container (obj +0x2b0, +0x348 set) has a locomotor that answers vf+0x230 and
vf+0x1d0 true.

**Script locks** [H]:
- `Lock_Current_Orders` (0x57cd40), AI-owned units only (error otherwise), mobile only ("is immobile; locking not
  supported"): when 0x39ad40 is false, 0x3a8e50(obj, 1) and the player's mode free store (player +0x360 → 0x4b06d0 →
  +0x50 → +0x38) subscribes to the object's signals 10 and 11. When 0x39ad40 is true: warning "%s is already locked.
  Script cannot stack locks." and nothing changes. A team (obj +0x348 set) whose container locomotor answers vf+0x230
  false (and not mode type 2) locks each member the same way [M: branch conditions]; a member of a team container locks the container
  (obj +0x2b0).
- `Unlock_Current_Orders` (0x581fd0), AI-owned only: when 0x39ad40 is true, 0x3a8e50(obj, 0) and unsubscribe 10 / 11;
  otherwise warning "is not locked". **No** Active_By_Default. Because the counter is shared, an unlock on a unit
  frozen only by an ability releases the ability's lock (the ability's later release then clamps at 0).

**Free store signal handler 0x4d4aa0** (AIFreeStoreClass vf1), signal 10 or 11 (the locked unit's order finished):
unsubscribe 10 and 11, **0x3a8e50(obj, 0)**, then **0x3a82a0** (Active_By_Default) unconditionally. Only units a script
locked are subscribed, so this decrement pairs with that script lock. This is the **only** 0x3a82a0 call in vf1
(the other branches handle signal 0x10 destroy and 0x1a owner change); free-store admission does **not** re-apply
Active_By_Default. **Corrects TACTICAL_GAPS §1.2** ("released to the free store").

**Task force release 0x5ff610** (vf49): after 0x6c27d0 (free-store re-admission), `if (!0x39ad40(unit)) 0x3a82a0(unit)`.
It releases no lock. The test is "movement locked", not "HUNT active" (TACTICAL_GAPS §1.2 wrote "not hunting").

**Interplay with ability `Disable_Movement`** [H]:
- INTERDICT (0x3a2240) and MISSILE_SHIELD with `Disable_Movement` (+0x10c) call 0x3a8e50(obj, 1) on activation and
  0x3a8e50(obj, 0) on deactivation (INTERDICT only while ability 3 is active, 0x39b950).
- A script cannot lock a unit an ability holds (0x39ad40 true). A unit locked by a script can still be locked by an
  ability (counter 2); either release order leaves the counter at 0 after both.

**Port mapping** [port]: the AbilitySystem counter (`AbilitySystem::lock_movement`, the `Disable_Movement` handlers)
is the ability share of obj +0x3a8; the script share is `ScriptObjectState::orders_locked` (so script locks do not
trigger the ability system's speed-0 freeze). `Lock_Current_Orders` refuses when either share or a suspended
locomotor is set; `Unlock_Current_Orders` clears the script share, else releases one ability lock. The free-store
path clears the script share when the AI sees the unit idle (stand-in for signals 10 / 11), then the host applies
Active_By_Default; the task-force path applies it unless movement is locked (HUNT active stands in for the locomotor
vf+0x1d0 term [M]).

### 28.9 What the movement lock blocks: readers, locomotor term, script locks, lock / unlock pairs (OI abilities t02, 2026-11-01)

Mode type (mode vf+0xe0): 0 galactic, 1 land, 2 space. "Locked" below = 0x39ad40 true. "Stop" = 0x3a9880 (locomotor
vf+0x138(vf+0x188()), i.e. set its idle state; leave every coordinator 0x426d50; 0x39bcb0) or the coordinator-system
stop 0x4261f0 (same per unit, used by INTERDICT / MISSILE_SHIELD).

**1. The lock does not stop or slow the unit.** [H] Nothing in 0x3a8e50 or 0x39ad40 touches speed, the locomotor
state or its suspend flag (pack +0x260). The first lock splits the unit off: `Split_From_Formation` (0x4f84e0) makes a
new formation in the same coordinator and **copies the old formation's move list** (+0xf8, 0x40f1a0) into it, so the
unit keeps its current move alone. Callers that want a freeze **Stop first, then lock** (INTERDICT, MISSILE_SHIELD,
DEPLOY_TROOPERS, SELF_DESTRUCT with `Stop_When_Activated`, the targeted infantry abilities); callers that want "finish
this move undisturbed" **move first, then lock** (Confuse, hero-clash MoveAction, TransportLanding unload). The order
matters: once locked, the unit can no longer leave its coordinator.

**2. Readers (callers of 0x39ad40; no other code reads obj +0x3a8 except save/load chunk 0x21 in 0x39ce40, the
object init 0x3989a0 := 0, and the MISSILE_SHIELD inline +/− in 0x38a350)** [H unless tagged]:
- *Move orders, coordinator level:* `Coordinator::Move_To` (0x4f8200) returns without doing anything when **any**
  member is locked (the whole order is dropped for every member). (Before that it also does nothing when, in space,
  the order's target object (block +0x28) is a TEAM member, or for type 3 with no target.) Coordinator remove-unit (vf+0x20 0x4f7d30, also via the
  space override 0x5d5130) **refuses a locked unit** unless obj +0x3a0 bit 0x02 is set [M: dying]. Coordinator add
  (0x4f6b60 via 0x438b40) refuses a unit that some coordinator already holds (0x397260 → 0x426140).
- *Group move* (0x2a7b80, every player move event and most AI moves): has no lock test, but for a locked unit the
  leave-old-coordinator step is refused, so the add to the new coordinator is refused too: **the locked unit is left
  out and keeps its current order; the rest of the group gets the move normally** [H code, M as a whole]. Before that
  it still cancels BARRAGE (5), 0x22, 0x29 and (flag) HUNT on the locked unit.
- *Formation Set_Move* (0x41fd30, used by Divert, coordinator re-issue, space composition): walks the members in order;
  the first locked member makes it **return** (no move appended, nothing started for any member). Members before it have
  already had BARRAGE cancelled. Because the first lock gives the unit a formation of its own, in practice only that
  unit's own orders are dropped.
- *Chase response* `0x4160a0` (Guard_Chase / Idle_Chase / Attack_Move_Response range): 0 for a locked unit, so it never
  leaves its spot to chase. Land formation building 0x43c1b0: a locked unit does not recruit others (type +0x16a4).
  Space composition 0x5d58f0 (SpaceMovementCoordinator vf12): a locked unit does not gather nearby ships.
- *Abilities:* `Can_Activate` 0x39b480 case 6 **HUNT** fails while locked; `Toggle_Behavior_Ability` 0x3a4070 refuses
  to **switch on** HUNT, LURE, AVOID_DANGER, DEPLOY_TROOPERS, SELF_DESTRUCT while locked (switching off is allowed).
- *Lua:* `Move_To` / `Move_Internal` 0x57da90 warning "...object %s is locked into existing orders. Attack_Target
  fails." (string copied from Attack_Target) and nothing; `Attack_Target` 0x5730f0 same warning, nothing; `Divert`
  0x575710 warning "unable to divert - object %s has had its movement locked.", false; `Can_Move` 0x5744a0 (tactical)
  = coordinator accepts (0x425e30) **and** not locked. Warnings go through 0x248620, errors through 0x248190 [M names].
- *UI (local player only, no sim effect):* 0x3afe0 = "some selected unit is not locked"; the move UIs 0x3b360 and
  0x3bc30 play the refusal SFX (DAT_140b301c8) and send no event when all are locked; 0x3b090 (land) sends the event
  and only picks ack vs refusal SFX; the galactic fleet move UI 0x3bf20 refuses a locked fleet. 0x3b070 = !locked
  predicate (no direct caller found).
- *AI:* free store Setup A 0x7287a0 skips locked units (and +0x3b4 hero-clash units); task-force release 0x5ff610
  (§28.8); `SpaceTaskForce` 0x6e5aa0 refuses a locked recruit; the movement block statuses' member collection
  (0x739200, 0x73a650, 0x73c6f0, 0x6affb0) skip locked members (no signal subscription, no order) [H code, M which
  class each is]; ConcentrateFire apply 0x6f5550 skips locked allies (tests the TEAM container when there is one);
  LURE 0x62b270 adds a lured unit's TEAM members to the new coordinator only when not locked.
- *Hyperspace:* cancel 0x5c9800 and SimpleSpace arrival finish 0x624dc0 release one lock **only if** 0x39ad40 is true.
- *Attack orders:* `Order_Attack` / `Issue_Auto_Target` (0x38d730) have no lock test, so a locked unit still takes a
  target and fires from where it stands; the approach move is refused by the rules above [M].

**3. The locomotor term vf+0x1d0 and the container terms** [H unless tagged]:
- vf+0x1d0 = **0x42cbf0** in every locomotor class (LocomotorCommon and all twelve derived vtables); Fleet answers 0
  (0x13b7d0). It returns the **current path record's byte +0x5c** (pack obj +0xa8, path stack +0x138/+0x140 of 0x70-byte
  records, top = 0x5569b0); false with no pack or an empty stack. It is **not** HUNT, not suspension (that is vf+0x210
  0x42cd10 = pack +0x260) and not hyperspace (that is vf+0x2b0 0x42c640 = state ∈ {4, 5, 0x28, 0x2b, 0x2c}).
- The byte is the path's waypoint-list flag (path +0x18 sub-object +0x44; copied by 0x5c45d0 / 0x54e4e0). The path
  builder 0x417520 sets it when the order's **move block byte +0x5c** is set [M: local flag traced, final store not].
  Setters of block +0x5c = 1: **`Divert`** (0x575710, bytes 1,0,0,1) and the **TransportLanding unload exit move**
  (0x644330). Cleared by 0x3ac210 (the unit's and its TEAM container's current path), called by the unload finish 0x6438a0.
  Other meaning: MovingVsStopped 0x684f90 lets a mover with this flag push a stopped blocker that has a locomotor
  aside (0x6859a0). Name: "forced move" (diversion / unload exit) [M].
- So "movement locked" is also true **while a unit runs a Divert or unload-exit move**, until that path is popped (path
  finished, replaced or cleared) [M duration]. A second Divert during a Divert is refused.
- Container term: vf+0x230 is true only for **LandTeamContainer** (0x17dc70; 0 elsewhere), so it applies to members of
  land infantry squads: the squad container's current path flag counts for every member. vf+0x228 is true only for
  **LandTeamInfantry** (squad members); vf+0x220 is true for every locomotor except Fleet / Starship and Team (0x6305a0).

**4. `Lock_Current_Orders` 0x57cd40 / `Unlock_Current_Orders` 0x581fd0, exact** [H]:
- Owner = player of obj +0x58 (0x294bc0); none → return silently. Not AI (0x2814d0 false) → **error** "GameObjectWrapper
  ::Lock_Current_Orders - object %s does not belong to an AI player; cannot lock." (Unlock: "...; cannot unlock.").
- Free store F = player +0x360 → 0x4b06d0(·, mode type) → +0x50 → +0x38; listener F+0x28. Per locked object:
  subscribe F+0x28 to the object's signals **10 and 11** (0x220e90); unlock unsubscribes both (0x220eb0).
- Lock, no locomotor (slot 1) → **error** "...Lock_Current_Orders - %s is immobile; locking not supported.".
- Lock, object has TEAM (obj +0x348) **and** mode ≠ 2 **and** its locomotor vf+0x230 is false (a land team whose
  container is not a LandTeamContainer; galactic teams): for each TEAM member: not locked → 0x3a8e50(member, 1) +
  subscribe the member; locked → **warning** "...Lock_Current_Orders - %s is already locked.  Script cannot stack
  locks." (member's type name). Return. In space (mode 2) a team container skips this and locks itself.
- Otherwise, locomotor vf+0x228 true (a LandTeamInfantry squad member): lock the **container** obj +0x2b0 (null →
  silent return) if it is not locked, subscribe the container; else the "already locked" warning. vf+0x228 false: lock
  the object itself the same way. Lua returns nothing.
- Unlock first checks obj **+0x3b4** (set while in a hero clash): **error** "...Unlock_Current_Orders - object %s is
  locked by a hero clash.  Cannot unlock." Then **land mode (1) and the object has TEAM**: for each member, locked →
  0x3a8e50(member, 0) + unsubscribe, else **warning** "...Unlock_Current_Orders - object %s is not locked.". Otherwise
  the object itself: locked → release + unsubscribe, else the same warning. Lock never tests +0x3b4.
- Asymmetry [H code, M effect]: Lock on a land infantry squad container (LandTeamContainer) or via a squad member locks
  the **container**; Unlock on the container walks the **members** (not locked → warnings) and leaves the container
  locked; Unlock on a member unlocks only the member. The container's script lock then ends only through the free store
  signal 10 / 11 handler.
- SpaceFreeStoreClass has its own vf1 **0x6907c0** with the same signal 10 / 11 branch as 0x4d4aa0 (unsubscribe,
  0x3a8e50(obj, 0), 0x3a82a0).

**5. Lock / unlock pairs (all 40 callers of 0x3a8e50)** [H unless tagged]:

| Who | Lock | Unlock |
|---|---|---|
| Lua script lock | 0x57cd40 | 0x581fd0, free store vf1 0x4d4aa0 / space 0x6907c0 on signal 10 / 11 |
| INTERDICT 0x3a2240 (Disable_Movement) | activation: stop 0x4261f0, then lock | off while ability 3 active |
| MISSILE_SHIELD (inline in 0x38a350) | on: stop 0x4261f0 **always**, deploying types vf+0x118(1), then +1 with Disable_Movement | off while active: −1 (clamped); also −1 whenever the inner `Can_Activate(t, 1)` fails, on or off (the expiry path), without an "is active" test [M: can unbalance] |
| DEPLOY_TROOPERS behaviour | service vf23 0x58f6a0: stop, lock, start deploying (count < max) | vf24 0x58f600 and dtor 0x58f4f0: unconditional −1 |
| SELF_DESTRUCT behaviour | vf23 0x630950: stop + lock only when the record has `Stop_When_Activated` (+0x1b1) | vf24 0x630840 / dtor 0x6306f0 when ability 0x27 is active (no Stop_When_Activated test) |
| Infection, GrenadeAttack, RemoteBomb abilities | vf30 0x6cc640 / 0x700860 / 0x719c90: face target (0x3a8710), stop, lock | vf4 0x6cc8d0 / 0x700d00 / 0x719ee0 when done: re-enable targeting + locomotor (vf+0xb8), −1, signal 0x29 |
| CableAttack ability | 0x6f3800: stop, lock | vf4 0x6f2d50 when its counter runs out |
| Confuse behaviour | 0x6364e0: group move to a random point (synced draws), then lock unit and its TEAM container, flag obj +0x3a2 bit 0x80 | vf6 0x636960: when bit 0x80 set, clear it, −1 on unit and flagged container |
| Hero clash (MeleeHeroClash) | 0x6abde0 locks both heroes (+0x18, +0x20) | vf5 0x6ac940, 0x6acb80, vf2 0x6acc30: −1, re-enable, clear +0x3b4 |
| Hero clash MoveAction | 0x5ea910: `Move_To` (0x4f8200), then lock | End vf9 0x5ea820; stuck fallback in vf7 0x5eaab0 |
| TransportLanding unload | 0x644330: spawned unit gets the exit move (block +0x5c = 1), then lock unit and its TEAM container (skipped when the 0x35e4c0 +0x30 flag is set [M]) | finish 0x6438a0: −1 on every unloaded unit and container, clear the path flag (0x3ac210), signal 0x19 |
| Hyperspace arrival, Fighter | vf+0x290 0x5cb020 immediate path (state 4) or state 5 0x5cd010; cinematic 0x5c9a80 | finish 0x5c9d40 (unconditional, from states 4 and 0x2c); cancel 0x5c9800 (if locked) |
| Hyperspace arrival, SimpleSpace | vf+0x290 0x6251d0 immediate path; state 5 0x627590; cinematic 0x5c9a80 | finish 0x624dc0 (if locked); cancel 0x5c9800 (if locked) |
| Hyperspace arrival, Team (space squadron) | vf+0x290 0x62f800: leave coordinators, lock container, suspend | service 0x630210 → 0x630390: −1, resume |

All arrivals lock **when the unit enters hyperspace-in** (state 5, or at once on the immediate / cinematic path) and
unlock **when it lands** (finish: teleport to the destination, show, resume, locomotor idle, signal 0x19) or on cancel.

**Implementer summary** (replaces the speed-0 freeze) [H unless tagged]:
- Keep one shared counter; lock/unlock as in §28.8. The lock never changes speed and never stops the unit by itself.
- `Disable_Movement` abilities (INTERDICT, MISSILE_SHIELD): on activation **Stop the unit** (clear its orders/path,
  locomotor idle, own coordinator), then lock. MISSILE_SHIELD stops even without `Disable_Movement`. Turning in place is
  not forced off; facing orders go through the same refused move path [M].
- While locked, **refuse new move orders for that unit**: a coordinator or formation move containing a locked member is
  dropped whole; a group move skips the locked unit (it keeps its current order) and moves the others.
- A unit locked mid-move (script lock, Confuse, hero-clash, unload exit) **keeps executing its current move**.
- Locked units do not chase (no guard / idle / attack-move response), still acquire targets and fire in place [M], and
  cannot switch on HUNT / LURE / AVOID_DANGER / DEPLOY_TROOPERS / SELF_DESTRUCT.
- Lua: Move_To, Attack_Target, Divert refuse with the warnings above; Can_Move is false; Lock/Unlock with the exact
  owner / immobile / team / hero-clash rules of item 4.
- Replace the "HUNT active" stand-in with: the unit's (or its land infantry squad container's) **current path carries
  the forced-move flag** (set by Divert and by the transport-unload exit move, cleared when that path ends or by the
  unload finish).
- AI collectors (free store, task forces, block statuses, concentrate fire, lure) skip locked units.
- Hyperspace arrivals: lock at hyperspace-in start, release on landing or cancel.

**Port (OI follow-up t03 / t04, 2026-11-01).** The shares: `AbilitySystem::immobilize` (Disable_Movement, Grenade
toss, Confuse, SELF_DESTRUCT with `Stop_When_Activated`; per-caller flags keep each pair balanced, saved from ability
save v2), the script's `orders_locked` and a running Divert (`lua::script_movement_locked`: the unit's current move goal
is still the one Divert recorded). `TacticalBattle::movement_locked` combines them. The speed-0 freeze is gone;
`order_move` / facing orders / the AI's group moves and `move_unit` leave locked units out, an attack order does not
approach, the stand-in and the TaskForce release path skip locked units; HUNT and the other behaviours refuse to switch
on. Lua Lock / Unlock follow item 4 (owner, immobile, team, squad member / container, warnings); Move_To /
Attack_Target / Divert warn and refuse; Can_Move = has a locomotor, not suspended [inferred], not locked. [port] Not
modelled: the hero-clash +0x3b4 refusal and lock (no hero clash), hyperspace-arrival locks (no arrival model), the
unload-exit, Infection, Remote_Bomb and cable locks (their phases are not modelled), the free-store signal 10 / 11
release of a script lock (the AI's idle stand-in stays); DEPLOY_TROOPERS locks and unlocks in one call (no net lock).

### 28.10 Implemented (a1g t01, 2026-11-01)

Tactical (`TacticalFreeStore` entry `owner_pp`) and galactic (`unit_reserved`, the derived free store) follow 28.4-28.6
through the shared `GoalSystem` (new hook `suspend_reservations` at Commit step 4; kept goals call `refresh_plan`).
On_Unit_Service skips only reserved entries; the order-lock skip is gone. Port decisions:
- [M] The accept re-solve (0x6c4a50) draws its own synced numbers (Plan Setup C start team, IgnoreTarget contrast),
  since it runs the same solver. Replaying the first solve's draws was tried: it changed the land outcomes little.
- [port] The goal keeps Init's cost / ETA after the re-solve.
- Not modelled: the queue-slot flag 0 / 1 of 0x53bee0, the tactical credits reservation, and the Setup A lock test
  (0x39ad40).
- 0x6bf3d0 (a1g t04 follow-up): each unit a land TF.Reinforce delivers joins its plan's PP+0x108 (`reinforce_join_pp`:
  the first null slot of its type, i.e. a pick of another source, keeps that slot's task force / team; else appended).
  [port] Task-force units stay off the free store, so the unit is not admitted and +0x30 = PP is set only if it is on the
  store; the delivered entry's type stands for the new object (not perceived yet); a filled slot's pool / pad
  reservation of this PP is released. The space poll (SpaceReinforceBlockStatus) does not call it: confirmed, its own
  handler 0x723ac0 only adds the unit to the task force and removes it from the free store (§32.1, a1h t05).
- ✅ a1h t07 (2026-11-02): a galactic build task of an AllowFreeStoreUnits plan takes its unit through
  `free_store_supply` (0x4d2bd0: the first free-store unit of the type at a planet), which no longer reads
  `unit_reserved`; a plan whose reserved unit is taken fails its validity test (0x6c45e0) at the next Commit.
- Effect: a proposal's first solve no longer skips units an earlier proposal of the same Commit picked, so more goals
  are accepted with smaller forces. The FoC land skirmish (`--skirmish-ai foc-land`) now ends inside 14400 frames on
  seeds 7, 10 and 12 of 1-12 (before: 1, 2, 4, 5 and 6 of 1-6), so the outcome test on the default seed 1 fails [open].

## 29. TEAM containers in the threat grid (a1f t04) [H unless tagged]

Example throughout: a land skirmish, company `Imperial_Stormtrooper_Squad` (18 `Company_Units` =
`Squad_Stormtrooper`, `Max_Squad_Size` 9, `Create_Team_Type` `Stormtrooper_Team`, no `AI_Combat_Power`),
spawned as two `Stormtrooper_Team` containers of 9 members each.

### 29.1 Which objects are in the grid

- Registration (0x6537d0) needs only the UNIT_AI behaviour (0x12). There is **no TEAM exclusion**:
  - GOM add hook 0x2b3670: mode type ≠ 0 and obj +0x344 (UNIT_AI slot) ≠ −1 → 0x4f5950 → 0x6537d0;
  - first UNIT_AI service 0x4f6070: registers when the pack has no entries yet.
- `Stormtrooper_Team` has UNIT_AI, so each team gets entries. A team has no hard points (0x294df0 = 0), so it gets
  **one whole-object entry** (hard-point index −1).
- Members (`Squad_Stormtrooper`) have no UNIT_AI: 0x6537d0 returns at once and 0x4f5950 never runs. Members have no
  entries and are **not** in the owner lists (+0x88).
- Order at spawn (team helper 0x2a5a60): the generic create 0x29f810 skips its own 0x2b3670 call for objects with a
  TEAM slot (obj +0x348 ≠ −1). The helper calls 0x2b3670 itself, after it has set team data +0x50 / +0x58 and added
  the members. So the first registration already sees the company type (§29.2).

### 29.2 Representative types of a team

Team data pack = obj +0x110 (TeamDataPackClass, ctor 0x4ec680 zeroes +0x50 / +0x58). The helper 0x2a5a60 fills:

| Field | Set by | Value |
|---|---|---|
| +0x50 | 0x406210 | the company's `Create_Team_Type` (+0x1e58) when set; else the company type (the "Team" fallback type is only used to spawn) |
| +0x58 | 0x406180 | the company type (the helper's type argument; always non-null there) |

- **0x396380(team)** = +0x58 (via 0x404be0) when non-null, else 0x3973b0. Checked in the disassembly.
  For the example: `Imperial_Stormtrooper_Squad`.
- **0x3973b0(team)** = FLEET (4) → fleet +0x70; else TRANSPORT (5) → transport +0x48 (the carried company type,
  §32.2); else TEAM → +0x50 (0x404c20); else, or when the chosen field is null, the object's own type (+0x298). For the example: `Stormtrooper_Team`.
- A container spawned without the helper (both fields 0) gives its own type for both [M: no such path traced].

### 29.3 AI_Combat_Power of that type

- The field +0xdf0 defaults to 0.0. No shipped GroundCompany (0 of 119) and no `Stormtrooper_Team` has the tag.
- It is **derived at load** (0x377500, OBJECT_TYPES §6.2 / §9.5):
  - a type with `Company_Units` gets +0xdf0 = the float sum, in list order, of each member's power;
  - here each member is plain 7 (the variant copies `Stormtrooper`'s 7, 0x36c000), so 18 × 7 = **126**.
  - The team type `Stormtrooper_Team` has no member list, so its value stays **0**.
- 0x370e60(T) at run time:
  - when the mode type ≠ 0 (tactical) and T has `Company_Units` (+0x960 > 0; else `Squadron_Units` +0x980 > 0),
    and the first list entry's type has `Create_Team` (+0x67):
    `power = clamp(T.Max_Squad_Size (+0x1e44) / list count, 0, 1) × T.+0xdf0`;
  - otherwise `power = +0xdf0`.
- So the "squad fill" is a **static share of the company per team**: `Max_Squad_Size` / `Company_Units` count. It
  does not depend on how many members are alive.
  - This corrects "scaled by squad fill" in GALACTIC_ECONOMY ~l.699 [M] and L1_S4_REMAINING ~l.89.
  - A last, partial team counts a full share too (e.g. 10 units / size 3 → 4 teams × 0.3 P).
- Example: 9 / 18 = 0.5 → **0.5 × 126 = 63.0** per team, i.e. 9 × 7.

### 29.4 Entry radius and position (0x653a50)

- Type for the range: R = 0x396380(obj). If R has `Squadron_Units`, use its first entry's type. Else, if it has
  `Company_Units`, use its first entry's type. Else use R itself.
- For a team this is the **first company member type** (`Squad_Stormtrooper`). The team type's own
  `Targeting_Max_Attack_Distance` is not read.
- r = 0x3749d0(that member type, **the team object**):
  - the ability, garrison (obj +0x3a7) and deployed (0x39a390) tests read the container;
  - `SPREAD_OUT` has no range override, so the example gives r = 140;
  - then the usual steps: + speed × look-ahead (0x370f00 = min(member max speed, team speed 0x4049e0); shipped
    look-ahead 0), max(r, half cell diagonal), min(r, land cap 500);
  - area = 4r² = 78400.
- Position = the container's own position (obj +0x78 / +0x7c).
  - At spawn it is the members' centroid.
  - After that, each TEAM service (0x405b10) sets it to the centre of the members' joint bounding box. It is skipped
    when the locomotor's vf+0x230 says so [M: when that applies].
  - Re-projection follows the container's transform (§14).

### 29.5 Values for one fresh team (9 live members, land)

| Use | Formula | Example |
|---|---|---|
| Entry power (add 0x656a10 / CellForce 0x6545d0) | 0x370e60(0x396380(obj)) | 63 |
| Threat layer per covered cell | 63 × cellW × cellH / (4 × 140²) | 63 × cellW·cellH / 78400 |
| CellForce per covered cell | the same × (1 − (1 − h) × A) | |
| §16 concentration (0x656800), per entry in the cell | 0x370e60(0x396380(obj)), no area or health factor | 63 |
| Player sums 0x656400 (Friendly / EnemyForceUnnormalized of a player, Game.ForceUnnormalized) | per registered object: 63 × (1 − (1 − h) × A) | 63 at A = 0 |

- Team health h = 0x396df0 → 0x405230 = the **mean of the members' hull fractions**: Σ over non-null list entries
  ÷ the list count, float, in list order (from the disassembly).
- Category tests use the mask of 0x396380's type. That is the company, whose mask 0x377500 replaces with the OR of
  its members' masks (Infantry). A TRANSPORT object (obj +0x337 ≠ −1) uses its own type for the mask and power
  (0x6537d0, 0x656400, 0x6545d0).

### 29.6 Player sums iterate the owner lists

- 0x656400 walks, for each player passing the relation filter, that player's grid owner list (+0x88) in
  registration order. Only objects registered by 0x6537d0 are in it, so **teams count once and their members never
  do**.
- It sums per object, not per entry: hard points are not split.
- So a team's contribution stays at 63 until the container dies, however many members are lost; only h moves when
  A > 0. When members die, h is the mean over the members still in the list [M: dead members leave the list].

### 29.7 Rule for the port's battle host (PerceptionObject of a TEAM container)

- **In the grid:** `has_unit_ai` = the container type has UNIT_AI (true for shipped teams), and
  `team_or_hide_when_fogged` = true.
- **Members:** `has_unit_ai` = false. They are not in the grid or the owner lists.
- **`type` (0x396380):** the company type the team was spawned from. Use the container's own type only for a
  container with no company.
- **`rep_type` (0x3973b0):** the company's `Create_Team_Type`, else the company type.
- **`combat_power`:**
  - start from the company's derived `AI_Combat_Power` = Σ over its `Company_Units` of each member's power (load
    rule, OBJECT_TYPES §9.5);
  - multiply it by clamp(company `Max_Squad_Size` / `Company_Units` count, 0, 1) when the first member type has
    `Create_Team`;
  - it is a constant per company type, not tied to live members. Example: 63.
- **`max_attack_range`:** 0x3749d0 of the company's first member type (`Squadron_Units` checked first), evaluated
  with the container's state. Example: 140.
- **`position` / transform:** the container's.
- **`health`:** the mean of the members' hull fractions.

## 30. A1 [M] / [port] choices reviewed (a1f t06) [H unless tagged]

Eight port choices left by A1, checked against the binary. "Port" = the code today; "Rule" = what to implement when
the verdict is **change**.

### 30.1 §12 zone threat-cell order: first insertion vs stdext hash_set → **change** (medium)

- Port: `TacticalPerception::init` (tactical_perception.cpp ~1132-1145) keeps one list per zone in first-insertion
  order of the .ted child-7 cells. The same list feeds zone danger, TimeLastSeen, the flood, path node costs and
  Get_Most_Defended_Position.
- Original: three containers of one MSVC `stdext::hash_map<uint32, uint8>` type (insert 0x5a84f0 / 0x5a8770,
  rehash 0x2d3220, grow 0x3f28e0, copy-assign 0x6d66a0, ctor 0x5a8c40 / 0x72c560). Iteration is the container's list
  order. No pointer, address or seed enters it, so the order is a pure function of the insertion sequence and
  **can be reproduced exactly with integer code**.
  - h(k) = minstd((k ^ 0xdeadbeef) & 0x7fffffff): q = x / 127773, r = x % 127773, v = 16807·r − 2836·q (int32),
    v += 0x7fffffff if v < 0. Bucket = h & (B − 1).
  - Start: B = 8, max load factor 1.0 (both ctors).
  - Insert k: duplicate (equal key in its bucket) → nothing. Else, when (float)(size + 1) / (float)B > 1.0, grow
    first: B' = B × 8 while B < 512, else the next power of two ≥ size + 1; rehash; then place k.
  - Place: bucket empty → append k at the list end. Else insert k into the bucket's run, keeping the run ascending
    by key (after the last key < k, else before the run's first).
  - Rehash(B'): walk the old list in order, placing each node with the same rule. Result: buckets in order of their
    first member in the old list, each run ascending.
  - Copy-assign (dest := src): dest list = src list, then rehash dest to n' = max(8, size): keep dest B if ≥ n';
    else dest B × 8 if dest B < 512 and that is ≥ n'; else n' rounded up to a power of two. The dest regroups, so
    its order differs from src whenever its B differs (e.g. 65..256 cells: src B = 512, dest B = 128 / 256).
- Chain per zone:
  1. Zone +0x50 = the .ted child-7 cells (0x5a8260), inserted in file order (key x | y << 16).
  2. Evaluator +0xb0: 0x6d65d0 walks +0x50 in its list order. Each cell → world (x·20, y·20) → threat cell
     (0x654210) → insert key col | row << 16 (0x72cd00).
  3. Zone +0x90 := evaluator +0xb0 by copy-assign (rcx = zone + 0x90, checked in the disassembly).
- Readers: evaluator +0xb0 order is used by Location.ForceUnnormalized (0x72cf10) and the zone TimeLastSeen (0x72e410).
  Zone +0x90 order is used by the flood (0x5a7390 / 0x5a7880), the path node cost (0x6940e0), the TF zone route
  (0x5a6940) and land Get_Most_Defended_Position (0x69f3a0 → 0x6556c0).
- Why change: §17 draws on every tie, so the last tied cells in iteration order win most often (1/2, 1/4, …).
  Chunk order (raster-like) biases defended positions toward one side of the zone; hash order does not. Float sums
  differ only in rounding.
- Rule: a ~60-line `stdext` order emulator, run once at load per zone. Keep two orders per zone: `eval_order`
  (steps 1-2) for ForceUnnormalized / TimeLastSeen, and `zone_order` (step 3, dest B starting at 8) for the flood,
  the path costs and Most_Defended.
- Test vector (alamo_ai_tactical_tests): insert the 16 keys of threat cells (c, r), r = 0..3 outer, c = 0..3 inner,
  into an empty set → B = 64. Order (c, r): (0,0) (0,1) (1,0) (1,1) (2,0) (2,1) (3,0) (3,1) (0,2) (0,3) (1,2) (1,3)
  (2,2) (2,3) (3,2) (3,3). After the first 5 keys (B = 8): (0,0) (0,1) (1,0) (2,0) (3,0).
- Not reproduced, stays [port]: vf13 never clears +0xb0. A unit-bound evaluator that re-links on a zone change
  (0x6d5690, 0x6d5760, 0x6d5860, 0x6d5950, 0x6d5a10, 0x6d5ac0) adds the new zone's cells to its old set and copies
  that **union** into the new zone's +0x90. That zone's flood and Most_Defended then see foreign cells until another
  evaluator re-links into it. [H code; M how often it fires]
- Implemented (a1g t02): `StdextHashOrder` (tactical_perception.hpp), run per zone in `TacticalPerception::init`;
  `zone_threat_cells` = +0x90 order, `zone_eval_cells` (evaluator leaves) = +0xb0 order. Test vector in
  alamo_ai_tactical_tests. The re-link union above stays [port] (not reproduced).

### 30.2 §7 overkill not clamped → **change** (small, low priority)

- Port: tactical_battle.cpp ~1763-1786 records `absorbed + to_hull`. `to_hull` = the full hit x
  (combat_system.cpp ~1690, `v.health = old − x`, no clamp).
- Original: the health setter 0x3a89d0 clamps health to [0, max] (maxss / minss). Apply_Hull_Damage (0x3ab890)
  sets old − x through it. When the object cannot die (+0x3a1 & 0x80), it then restores max(1, old). Take_Damage
  (0x3a9e30) records (shieldBefore + hullBefore) − hullAfter − shieldAfter. That includes the killing hit, capped at
  the remaining hull, and 0 hull loss for a cannot-die object.
- Today nothing reads a dead victim's entries (it leaves the store the same tick), so the gap shows only for
  cannot-die objects (port flag `kUnitCannotDie`, not set by the host yet) and for a future running total (+0x8).
- Rule: hull part = hullBefore − hullAfter, with hullAfter clamped to ≥ 0 and replaced by max(1, old) for cannot-die
  objects (e.g. `min(to_hull, hullBefore)`, and 0 when the cannot-die restore ran). Test: a tracked 50-hull victim
  hit for 200 records 50.
- Implemented (a1g t03): `DamageResult` / `DamageEvent::hull_lost` (combat), recorded by the host and
  `record_combat_damage`; `to_hull` keeps the raw hit. Tests: alamo_tactical_combat_tests, alamo_ai_tactical_tests.

### 30.3 §19 game-mode gate → **change** (small)

- Port (tactical_perception.cpp ~464-478): includes an object when `w.campaign || o.rep_type < 0`, and adds
  `o.combat_power`. `w.campaign` is also true in land skirmish (battle_ai.cpp ~308).
- Original (0x6d4180, disassembly at 0x6d42a4..0x6d42d1): mpOrSkirmish = GMM current mode (+0x38) ≠ 0, GMM +0x20 ≠ 1,
  and game type (+0x18) ∈ {1, 2, 3, 4}. 0x28af80 tests {1, 2, 4} (networked) and 0x28b060 tests 3. [M] 3 = local
  skirmish: the network code treats 0 and 3 alike as local. For each TRANSPORT_LANDING object in the GOM behaviour
  list (0x36) order, with 0x39a2c0(obj, P, 0) == friendly:
  - src = obj when mpOrSkirmish or obj +0x54 (parent-mode id) == 0x3fffff;
  - otherwise src = the parent-mode object with that id (0x28aa50, OBJECT_RUNTIME §1.4). When it is not found, the
    object is skipped;
  - T = 0x3973b0(src). It needs T ≠ null and T's category & mask ≠ 0. Then sum += 0x370e60(T), the **type's**
    power (no health or attenuator factor).
- Rule: add `skirmish_or_mp` to PerceptionWorld (game type ∈ {1..4}) and `parent_object` per object (+0x54; −1 =
  none). Include as above. Power = the AI_Combat_Power rule of `rep_type` (for a TRANSPORT, 0x3973b0 gives transport
  +0x48: the carried company type, §32.2), not the landing object's own power. Test: one landing object in (a) skirmish,
  (b) campaign with a resolvable parent (the parent's type's power), (c) campaign with an unresolvable parent
  (skipped).
- Implemented (a1g t03): `PerceptionWorld::skirmish_or_mp` (host: the battle is a skirmish; no MP), `parent_object`
  (store parent-mode id) and the `parent_rep_type` callback (host: the galactic object's type, -1 when not found; the
  port's galactic company object has the company type as its own type, which is the original transport's +0x48).
  Tests (a)-(c) in alamo_ai_tactical_perception_tests. The landing object's own carried type: §32.2 (a1h t08).

### 30.4 §21 `inside_shield` not filled → leaves **keep** 0; IsValidBombingTarget **change** (small)

- Port: `PerceptionWorld::inside_shield` is never set by the host. So IsInsideEnemyShield / IsInsideFriendlyShield
  (tactical_perception.cpp ~755, ~867) return 0. The bombing test (~481-497) skips its shield step and uses a 2-D
  prevention distance.
- Original leaves (0x6d7210 unit, 0x6d5760 location) → 0x3b8790. It walks the land store list (+0x2f0) for an
  object with the BASE_SHIELD slot (+0x36b), but reads the active state through behaviour **0x10 (SHIELDED)**
  (vf+0x10(0x10) → +0x28 → +0x1c8 → +8 == 1, disassembly 0x3b8818). No shipped type has both BASE_SHIELD and
  SHIELDED: 14 BASE_SHIELD types, none with SHIELDED, and STARSHIP_COMBATANT is the only implicit behaviour. So the
  leaves are **always 0** in shipped data, which the port already gives.
- Original bombing test 0x494540 uses behaviour 0x39 (checked at 0x49461b). The point fails inside **any**
  player's active BASE_SHIELD: GOM list 0x39 order, `Base_Shield_Radius` (+0x21f4) > 0, state == 1,
  sqrtf(dx² + dy² + dz²) < r. The prevention test is **3-D**: dx² + dy² + dz² ≤ (+0x1dfc)², over Self's objects.
- Rule: give the bombing test its own host callback (e.g. `in_active_base_shield(p)` = the existing
  `TacticalBattle::l1_base_shield_covers`), and make the prevention distance 3-D. Leave `inside_shield` unset, or
  require SHIELDED on the shield object if it is ever wired. Test: a location inside an enemy or own active shield →
  IsValidBombingTarget 0; IsInsideEnemyShield stays 0.
- Implemented (a1g t03): `PerceptionWorld::in_active_base_shield` (host: `TacticalBattle::l1_base_shield_covers`, land
  only) and the 3-D prevention distance; `inside_shield` stays unset. Tests: alamo_ai_tactical_perception_tests.

### 30.5 §15 full memory removal on destroy → **keep** [port]

- Port: threat_grid.cpp ~341-357 removes the entries from every player's memory lists.
- Original (0x6585f0): removes memory entries over the snapshot rect only for players with 0x4f57d0(pack, p)
  (fogged). It stops at a destroyed hard point (r < 0). Then it removes the live entries (0x656d10), the owner list
  entry (+0x88) and the signal subscription.
- The other players keep pointers to the destroyed pack's entries until the cell refogs. CellForce (§10) reads them,
  which reads freed memory: undefined behaviour that no port can reproduce deterministically. Full removal is the
  only deterministic choice. It is already labelled [port].

### 30.6 §14 stamps drawn at first fill → **keep** [port]

- Port: battle_ai.cpp ~651-661 draws Get_Int(0, 300) and Get_Int(0, 150) when `fill()` first lists a UNIT_AI object.
- Original: the attach loop 0x38cb30 calls UnitAIBehavior vf+0x38 = 0x4f52f0 (vtable 0x87af98) → 0x390140 → the
  pack ctor 0x550ed0, inside object creation (0x3989a0), in the type's behaviour-list order, for every UNIT_AI object
  in any mode.
- Same draw count and value distribution, and stamp ≤ F either way, so the first check is at the first service and
  the 300-frame phase is uniform (§14). Only the synced-stream position differs. Parity is checked by behaviour
  (CLAUDE.md), so moving the draws into object creation (cross-module: objects / spawn) gains nothing.

### 30.7 TF.Reinforce mixed cost / distance keys → keys **keep** (now [H]); placement **change** (medium)

- Port: `tf_reinforce` rank (tactical_ai.cpp ~1906-1925) and `reinforce_landing_point` (~1848-1884). It drops points
  outside the flood set. The key is the slot-7 cost for a point with a zone, or the 2-D distance (double) for one
  without. It takes the **single** best point and moves from it toward the target by min(d, r/2).
- Original 0x6eaf60 (land only, mode vf+0xe0 == 1). D = region target at the destination position (target list
  vf+0x48), with its reachability object (vf+0x38). Level = the TF goal's `Reachability`.
  1. Candidates: the land store list (+0x2f0) in order. The type's `Reinforcement_Enabling_Radius` (+0x1e20) must be
     > 0, and 0x39a2c0(obj, player, 0) must hold.
  2. Key (float): R = region target at the point. If D's reachability or R is null, key = sqrtf(dx² + dy²) to the
     destination. Otherwise the point is skipped unless slot 5(D, R, level) holds, and key = slot 7(D, R, level).
     A failed path sum gives 1e18; that point is kept and sorted last. **The mixed keys are the original's.**
  3. Sort in a float multimap: ascending, and equal keys keep list order.
  4. For each point in key order, for θ = 0, π/4, …, 7π/4 (float step 0.7853982 while < 6.2831855): try to land the
     **last** pending unit at (px + (r/2)·cos θ, py + (r/2)·sin θ, pz), r = that point's radius, via mode vf+0x280.
     On success, pop the unit and take its population off the cap read once (0x2acb10). Then pop trailing units
     that are null or do not fit (this clears the "all placed" flag). Stop when the cap is < 1 or no unit is left.
  5. It returns true only when every unit was placed and none was skipped.
- Rule: keep the keys, but compute them in float, sort stably, and use the 8-spot ring per point. Units go one per
  accepted spot, cascading to the next point. The landing does not move toward the target. Test: two allied
  reinforcement points and three pending units → spots (p1 + r/2·(1, 0)), (p1 + r/2·(0.7071, 0.7071)), … in that
  order.
- **Implemented (a1g t04, 2026-11-01):** `reinforce_points` / `reinforce_ring` / `reinforce_place` (tactical_ai.cpp),
  test `test_reinforce_landing` (alamo_ai_tactical_world_tests). cos / sin via `dmath::sincos(float)`. Note: the float
  loop θ += 0.7853982f while θ < 6.2831855f reaches 6.283185 (< 6.2831855) after eight steps, so it tries **nine**
  spots, the ninth at θ ≈ 2π (≈ the first spot); the port follows the loop. [port] choices: a null pending entry (no
  type, or gone from the host's pool) is dropped from the TF, a non-fitting one stays pending for a later Reinforce;
  the trailing pop also runs once before the first spot; without Use_Reinforcement_Points every spot is the target.
- **Drop validity (a1g t04 follow-up, 2026-11-01; layout per TACTICAL_GAPS §4.2.1):** `TacticalAiHost::land_drop_ok(player,
  key, p)`, asked before each spot's Reinforce order. The battle host (`BattleAi::land_drop_ok`) lays out the company with
  `land_drop_layout` (0x5abc00 teams / 0x40f970 grids, centred on p, turned by the side's yaw), the same slots
  `TacticalBattle::order_reinforce` deploys, and refuses the spot unless every slot is inside the playable box and passable
  for its member's MovementClass, not fogged for the player, clear of every blocking footprint (oriented slot box vs the
  object's box after the formation-box broad phase; movers ignored for the AI, proximity mines never block) and passes the
  radii (enabling <= with Use_Reinforcement_Points, prevention <, a POWERED object blocks only while its owner has base
  power). [port]: the layout yaw is the side's deploy facing (the per-player entry facing is [M]); blocking objects are the
  battle units and LAND_OBSTACLE objects; footprint boxes are Custom_Hard extents x Scale_Factor, else the land model
  box, else the movement radius; the +0x1b60 substitute member type and the landing-transport rule are not modelled; the
  human deploy clamps each slot to the playable box and moves only blocked slots to the nearest passable cell.
- **Ring spot count from the disassembly (a1g follow-up, 2026-11-01): nine spots per point [H].**
  - θ lives in xmm6. It is reset to 0 (xmm9, `xorps` at 0x6eb115) for each point at 0x6eb361.
  - Each try ends with `addss xmm6, [0x140b3adb8]` at 0x6eb56a, then `comiss xmm11, xmm6` and `ja 0x6eb380`, so the
    loop continues while limit > θ (strict `<`). This also runs after a failed landing. The test is at the loop's end,
    so the θ = 0 try always runs; the only exits before it are the cap and pending-list checks at 0x6eb380.
  - θ is accumulated, not recomputed as i·step. All the math is SSE single precision (`addss` / `comiss` /
    `mulss`); there is no x87 code in 0x6eaf60.
  - Step 0x140b3adb8 is a function static, set once at 0x6eb6f8: `movss` float π (0x1408007dc = 0x40490fdb) then
    `divss` 4.0 (0x1408007e0), giving 0x3f490fdb = 0.78539819.
  - Limit xmm11 = [0x140819b3c] = 0x40c90fdb = float(2π) = 6.28318548 (loaded at 0x6eb323).
  - Float accumulation gives θ = 0, 0x3f490fdb, …, 0x40afeddf (7π/4), then **0x40c90fda = 6.28318501**, one ulp below
    the limit, so a ninth try runs. The next value, 7.0685830, ends the loop.
  - The ninth spot uses cos ≈ 1 and sin ≈ −1.7e-7 of θ (calls 0x776150 / 0x776650). The direct-value shortcut, taken
    when the float exponent field is ≤ 0x1d000000, applies only to θ = 0. So the ninth spot is about the first spot
    again but not bit-identical.
  - The port's nine-spot loop matches. Its constants must be these exact bit patterns, with θ accumulated in float.

### 30.8 TF link mask −1 → **keep** (equal to shipped data); parsing optional (small)

- Port: Setup A stages call `flood_pick(..., 0xFFFFFFFF, ...)` (tactical_ai.cpp ~1283-1299, flood_pick ~723-739).
- Original: TF definition +0x94 starts at −1 (0x66f5d0). In the token loop (0x66fcc0), after the stage keywords, each
  token is offered to the `BitfieldConversionClass<CellPassabilityType>` converter (0x140b310c8, built by 0x5dd090),
  which writes +0x94. So +0x94 is a **CellPassabilityType link-flag mask** (Clear 1, Steep 2, Obstacle 4, RebelWall
  8, EmpireWall 0x10, Water 0x20, Shield 0x40, InfantryOnly 0x80, Impassable 0x100). AI_GALACTIC_2 §1.2 calls it a
  "unit-filter mask"; that name is wrong. Setup A passes it as slot 6's linkMask (0x7287a0 reads TF +0x94).
- No shipped FoC or EaW AI script has such a token in a task-force definition, so +0x94 = −1 in every shipped plan.
  The port's −1 is exact.
- Optional rule (for mods): parse a TF token that names a CellPassabilityType ([M] `|`-separated, case-insensitive)
  into the mask, and pass it to `flood_pick`.

## 31. Story `Invade()` on a fleet the local player does not own (a1g t05) [H unless tagged]

**Question.** In the S10 probe campaign "S10 PROBE A: TATOOINE" (`tools/probe_mod/s10_map_owners`, story script
`s10_story.lua`: `SpawnList` at Byss, `Assemble_Fleet`, `BlockOnCommand(fleet.Move_To(Tatooine))`, `Sleep(1)`,
`fleet.Invade()`, the pattern of the shipped `Story_Campaign_Underworld.lua`), the original behaved differently by human
side (2026-10-31 run, a1f t08):
- human **Underworld** (the fleet's owner is the local player): the invasion started at once;
- human **Empire** (Underworld = `AI_Player_Control Underworld, None`): the Underworld fleet reached Tatooine orbit and
  **never invaded**.

The port invades in both cases (`alamo_sim_battle_tests`, `test_probe_invade`).

### 31.1 Answer

Lua `Invade()` on a fleet object (`GameObjectWrapper.Invade`, 0x57ada0) does not invade as the fleet's owner. It builds
an `InvadeEventClass` on the stack and executes it directly. The event's issuing player is the **local player**, and the
executor lands only units that the issuing player owns.

So with a human Empire, the AI Underworld fleet's units (owner Underworld) are all filtered out. Nothing is queued, no
message is logged and Lua gets no error. The fleet stays in orbit. With a human Underworld, issuer and owner agree and
the land conflict is queued at once. Both probe outcomes follow from this one rule.

### 31.2 The chain

1. **0x57ada0 `GameObjectWrapper::Invade`** (Lua; returns no values, so there is nothing to block on):
   - more than 0 arguments: error "invalid number of parameters. Expected 0";
   - not a fleet (behaviour slot +0x336 = −1): error "this function is only valid on fleets";
   - walks the parent chain (+0x2b0) to an object with the planet slot (+0x335 ≠ −1). No such parent (in hyperspace):
     warning "this fleet is not at a planet. Cannot initiate invasion", nothing else happens;
   - enemy test 0x2824f0 (planet owner's player, **fleet owner's** player; owner index at obj +0x58, player via 0x294bc0).
     Not enemies: warning "fleet owner %s is not an enemy of planet %s owner %s. Cannot initiate invasion";
   - collects the fleet's contents (0x3965f0) and keeps the land units (+0x337 ≠ −1: transports / companies). None:
     warning "fleet contains no land units. Cannot initiate invasion";
   - otherwise: event ctor 0x48fce0 → 0x512c50 → **EventClass ctor 0x5126c0, which sets event +0x24 = local player id**
     (0x294a70 = PlayerList +0x30 index → player +0x4c). Then 0x4903c0(event, units, planet, **player = −1**, flag 1):
     it copies up to 127 unit ids (+0x48, count +0x244), sets the planet id (+0x248) and flag +0x24c, and changes +0x24
     **only when player ≠ −1**. So the issuer stays the local player. Then Execute 0x48ff50 is called directly.
2. **0x48ff50 `InvadeEventClass` Execute:**
   - **Prologue gates** (galactic mode object present):
     - mode type (vf+0xe0) = 0 (galactic) and the galactic pause / battle-pending counter 0x140b1540c (GMM+0x2c) > 0 →
       return;
     - single-player, the linked-battle flag 0x140b154d0 set (REMAINING_GAPS §6.4) and the local player in a queued
       conflict (0x2b0350) → return.
   - For each unit id: resolve it (0x2aca60). The unit is **kept only if its owner (obj +0x58) == event +0x24**.
   - For every resolved unit, kept or not: story check 0x326a30(story manager, unit, planet, 1), which asks each active
     plot through 0x52eb10. Any hit sets "suppress" [M: a story LAND_ON-style hook claims the landing]. Then
     0x328720(story manager, planet, unit).
   - After the loop: one more 0x326a30(story manager, 0, planet, 1).
   - Not suppressed and that last check false: if the kept list is non-empty, **`Land_Invade` 0x2b0520**(planet, kept
     ids, **attacker = event +0x24**, defender −1, map 0, at_front 0). An empty kept list → nothing, and there is no
     message on any path. Last check true: 0x3286f0 instead.

| probe variant | local player | fleet owner | kept units | result |
|---|---|---|---|---|
| human Empire (preferred) | Empire | Underworld (AI None) | none | no `Land_Invade`; the fleet stays in orbit (observed) |
| human Underworld (fallback) | Underworld | Underworld | all land units | `Land_Invade`(Tatooine, attacker Underworld) → land conflict vs Empire (observed) |

3. **Who else builds an InvadeEvent, and with which issuer:**

| caller | player argument to 0x4903c0 | issuer |
|---|---|---|
| 0x57ada0 Lua `GameObjectWrapper.Invade` | −1 | local player |
| 0x45f5c0 story reward `INVADE_PLANET` | −1. It goes through the outgoing queue (0x3b01f0 on 0x140b2ed18). **One event with one unit**: the first transport, in a fleet in orbit that is friendly (0x39a2c0) to the plot's player (+0x228 → +0x638), that passes the Param2 filter (§31.6) | local player (set by the event ctor; the single-player flush does not restamp it, §31.6) |
| 0x3d5640 / 0x3d6c50 galactic UI | −1 | local player (the human's own order) |
| 0x3d2900 | an object's owner (+0x58) [M: which object] | that owner |
| 0x433eb0 fleet-locomotor auto-land (STRATEGIC_GAPS §1.3) | a fleet member's owner (+0x58) | the fleet's owner [M] |
| 0x884c0 / 0xa7de0 | −1 | local player [L: callers not identified] |

The AI plan path does **not** use the event: `GalacticTaskForce.Invade` 0x6ce600 calls `Land_Invade` directly with
attacker = the fleet owner's player id (0x294bc0(owner)+0x4c). AI-plan invasions therefore work for any owner. Only the
object-level Lua `Invade()` and the story reward are tied to the local player.

4. **The local player is fixed during scripts.** The local index (PlayerList +0x30) is set by setup 0x2924d0 (by
   faction; it also sets `Is_Human` +0x62), reset to 0 by 0x2918d0 / 0x297630, and saved and restored around a player
   list rebuild (the caller near 0x297630). No story or AI code switches it while a script runs. [H for the writers
   found / M that none is missed]

### 31.3 The three candidates

1. **Move_To / BlockOnCommand on a non-human fleet: not the cause.** The fleet reached orbit in the original. If the
   block had ended early, `Invade()` would have hit the parent walk in 0x57ada0 and logged "not at a planet". The human
   Underworld run invaded, so the block ends with the fleet at the planet. The port's a1f t08 change (an order on a
   fleet finishes when the fleet stops) stays.
2. **Invade / Land_Invade with an AI attacker: the cause** (§31.1, §31.2). `Land_Invade` itself accepts any attacker
   ≠ defender (BATTLE_TRANSITIONS §1.3, §2.9.7). The AI attacker never reaches it, because the event lands only the
   local player's units and attacks as the local player.
3. **The AI taking the fleet: not the cause.**
   - The probe's Underworld runs AIPlayerType `None` (`PLAYER_NONE.XML`). It has no `GalacticFreeStoreScript`, only
     space/land goal sets (`BasicOffensiveSpaceSet`, `BasicLandGoalSet`, `AI_GoalSet_ExpansionGeneric_Land`) and the
     galactic template `None_Template`. So nothing galactic proposes a plan for the fleet or moves it [M: no galactic
     free-store script = no galactic free-store moves]. That matches the fleet staying in orbit.
   - Ownership does not change either way, so the 0x48ff50 filter result would be the same.

### 31.4 Consequence for the shipped campaign

`Story_Campaign_Underworld.lua` (FoC) spawns Empire (`Empire_Attack`, `empire_land_fleet.Invade()`) and Rebel
(retaliation, `rebel_land_fleet.Invade()`) land fleets and sends them against Underworld planets. In
`CAMPAIGNS_UNDERWORLD_STORY.XML` the human is Underworld (`AI_Player_Control Underworld, ScriptableHuman`; Empire,
Rebel, Hutts and Pirates are `None`). By §31.2 these `Invade()` calls land nothing in the original: the land fleet
arrives in orbit and stays there. The space fleet sent before it does fight, through the normal arrival conflict.
[H for the code path; not observed in the shipped campaign. An optional probe could confirm it; the port follows the
code.]

### 31.5 Port vs original, rule, test

- **Port today:**
  - `engine/lua/src/bind_players.cpp` `m_invade` issues the Invade event with **player = the fleet's owner**
    (`fl.owner`). The unit list holds all of the fleet's ground units. It has no enemy test and no warnings.
  - `GalacticSim::invade` (`engine/galactic/src/campaign.cpp`, Invade event case) then queues the land conflict for
    that owner.
  - An **empty** unit list there means "every ground unit of the player at the planet", which 0x48ff50 never does.
- **Rule** (module `lua`, `m_invade`; no galactic change needed):
  1. Refusals as 0x57ada0, in order, as Lua warnings, with nothing issued:
     - not a fleet;
     - not at a planet (moving / in hyperspace);
     - planet owner not an enemy of the **fleet owner**;
     - the fleet has no ground units.
  2. Issuer = the **local player** (`LuaPlayerWorld::local_player()`, the same as `Find_Player("local")`), not the fleet
     owner.
  3. Units = the fleet's ground units whose owner == issuer, in content order, at most 127. If none: return, with no
     event and no message (the original drops it silently).
  4. Otherwise one Invade event on the direct path with player = issuer (as now). Return nothing.
  - Never send an empty unit list from this path, because the galactic executor reads an empty list as "all units".
- **Also from 0x48ff50, for the galactic Invade executor (module `galactic`; check whether it already holds):**
  - drop unit ids not owned by the issuer;
  - drop the whole event while the galactic pause / battle-pending counter is raised, or while a linked battle is
    pending and the local player is in a queued conflict.
- **Test** (`engine/apps/sim/tests/test_battles.cpp` `test_probe_invade`):
  - human-Empire run: give Underworld no galactic AI (`setup.players[1].ai = false`, the probe's `None`; otherwise the
    port's galactic AI may invade by its own plan). After the 900 ticks: `land_queued == 0`, `fought == 0`, the fleet
    still in orbit of S10_B, the story events FLEET / ARRIVED / INVADED fired, and no Lua message;
  - human-Underworld run unchanged: one land conflict "land S10_B Underworld vs Empire", fought once.
  - `engine/lua/tests/test_lua_players.cpp` `test_galactic_ops`: `Invade()` on a fleet the local player does not own
    issues no order (`orders_issued` unchanged); on a fleet it owns, one order with player = local.
- No probe is needed: the binary settles it, and it matches the 2026-10-31 run.

### 31.6 Story reward INVADE_PLANET (a1h t04) [H unless tagged]

Reward type 0x22 (reward switch case 0x22 → 0x45f5c0). It reads only Reward_Param1 (+0x50) and Reward_Param2 (+0x70).
It returns nothing. Every message is a debug-log line (0x467540); nothing reaches the player or Lua.

**0x45f5c0, in order:**
1. Param1 empty: nothing, no log.
2. Log "STORY REWARD - Invade planet %s".
3. **Planet.** Param1 is a game object **type** name (0x407e00, case-insensitive). The planet is the first object of
   that type in the current mode's object manager (0x140b15418 +0x18, list 0x2a87d0), as for SABOTAGE_STRUCTURE
   (L1P4_LUA_GAPS §3). There is no owner filter and no check that the type is a planet.
   - Not found (unknown type, or no object of that type): log "STORY REWARD FLASH OBJECT- Unable to find planet %s"
     (sic, copied from FLASH_OBJECT). The function then **carries on with a null planet**, and the orbit-list read
     0x395900 dereferences it, so the original crashes. With an unknown type name the log line already formats a
     name read through the null type. [H for the code; not observed]
4. **Planet friendly to the plot's player: nothing**, no log.
   - The plot's player is reward event +0x228 (owning plot) → plot +0x638 (owning player, STORY.md). [H; replaces the
     [M] in §31.2]
   - Friendly = 0x39a2c0(obj, P, 0): the owner (planet: planet data +0x6c; other objects: +0x58) is P or an ally
     (P +0x370 relation table [owner] == 0). An owner with no player, or a null P, is not friendly.
5. **Walk the planet's orbit list** (0x395900: planet data +0xc0 vector) in list order. For each orbit object O that
   is friendly to P (same test), take O's **direct** contents (0x3965f0 with expand = 0: the fleet's members, not
   the cargo of transports). For each member U, in content order:
   - skip U unless it is a TRANSPORT (+0x337 ≠ −1), i.e. a galactic land unit;
   - **Param2 filter**, only when Param2 is non-empty: the transport's company type (TRANSPORT behaviour, 0x4f3bb0 =
     transport data +0x48) has its upper-cased name (type +0xf8) compared with Param2 **byte for byte, case-sensitive**
     (length, then memcmp). On a mismatch: log "INVADE PLANET - %s does not match %s" (type name, Param2) and go to
     the next unit. The shipped Param2 values are upper case, so they match;
   - U's combatant (object vf+0x10 with id 0x13; ref-counted: vf+0 add, vf+8 release) must exist and answer true to
     its vf+0x138 (can fight in a land battle, BATTLE_TRANSITIONS §4.2) [M for the meaning of vf+0x138]. Otherwise go
     to the next unit, with no log;
   - **the first U that passes**: build **one InvadeEvent holding only U** (0x48fce0; 0x4903c0(event, [U], planet,
     player −1, flag 1)), add it to the outgoing queue (0x3b01f0 on 0x140b2ed18) and **return**. The rest of the
     orbit list is not looked at.
6. No unit passes: nothing (only the mismatch log lines).

So the reward sends **at most one unit in at most one event**. Param2 is a filter on which transport is sent (the
company it carries), not a requirement checked anywhere else. Without Param2 the first land transport of the first
friendly fleet is sent. The 127-unit cap of 0x4903c0 never matters here.

**Issuer and timing.**
- Issuer = event +0x24 = the local player, set by the EventClass ctor (§31.2 step 1). Player −1 leaves it unchanged.
- Correction to §31.2: the single-player flush 0x3b08c0 does **not** restamp the player. It only sets the execute
  frame (+0x28) to the current global frame and moves the event to the scheduled queue (0x140b15690).
- The reward fires inside tick N (story service) [M: story service runs in the tick]. The outgoing queue is flushed at
  step 8 of the **next** main-loop iteration and executed at step 10 (0x2c0c70) (MAIN_LOOP §1.1). So 0x48ff50 runs
  after tick N and before tick N+1, like a human order (COMMANDS §4.1).

**Then 0x48ff50 (§31.2):**
- The whole event is dropped, with no message, if at execution time:
  - the galactic pause / battle-pending counter (GMM+0x2c) is > 0; or
  - in single-player, the linked-battle flag is set and the local player is in a queued conflict (0x2b0350, below).

  The reward is not retried, because it has already fired.
- U is kept only if its owner == the local player. If kept, `Land_Invade`(planet, [U], attacker = local player,
  defender −1), unless a story check claims it (0x326a30 → 0x3286f0) [M].

**Consequences.**
- **Plot player = local player:** U is the local player's own transport and lands as the local player.
- **Plot player ≠ local player** (an AI faction's plot): U belongs to the plot player or one of its allies. It is
  dropped unless the local player owns it. Then nothing happens, there is no message, and the reward is spent.
- **Ally's transport first:** even with plot player = local player, if an ally's friendly fleet comes first in the
  orbit list with a transport that passes the filter, that transport is picked and then dropped. There is no
  fallback to a later unit.
- **Shipped data:** all four INVADE_PLANET events are in Underworld campaign plots:
  - `STORY_CAMPAIGN_UNDERWORLD.XML` M07: `Bespin_Story` / `TYBER_ZANN_TEAM_NO_PEACEBRINGER`;
  - `_TUTORIAL` and `_DEMO`: `Felucia` / `TYBER_ZANN_TEAM_PROLOGUE`;
  - `_TEST_CAMPAIGN`: `Bespin` with no Param2.

  The human is Underworld in those campaigns (`Underworld, SandboxHuman` / `ScriptableHuman`). So Tyber's transport
  lands as Underworld, and the follow-up STORY_DEPLOY event (Param1 = the same team type, Param2 = the planet) starts
  the LINK_TACTICAL mission. It works in the original.

**0x2b0350 "local player in a queued conflict"** (argument: the galactic GOM).
- It returns true when the local player id (0x294a70) is the attacker (+0x04) or the defender (+0x08) of **any**
  record in the **space** queue (GOM+0x470, count +0x478) or the **land** queue (GOM+0x488, count +0x490). Every
  record counts, not only the head.
- There is no separate test of a running battle: a battle counts only while its record is still in a queue.
- It has no single-player test of its own: callers (0x48ff50, 0x2be640 and others) check single-player
  (0x28af60 == 0) and the flag themselves.

**0x6ce600 `GalacticTaskForce.Invade`: confirmed direct.** It calls `Land_Invade` 0x2b0520 itself, with no
InvadeEvent. So none of 0x48ff50's gates or hooks apply: no pause counter GMM+0x2c, no linked-battle test or 0x2b0350,
no owner filter, no story checks 0x326a30 / 0x328720. It runs inside the script's own tick, with no queue.
`Land_Invade` has no pause gate either. Its gates, in order (warning = 0x248620, error = 0x248190; each returns
nothing):
1. No fleet wrapper (+0x120), the wrapper has no object, the fleet object is flagged removed (+0x3a0 bit 1), or the
   task force's unit list is empty: warning "No units in TaskForce %s, or no FleetObject".
2. The fleet (behaviour 4) has no orbit planet (0x432c40 = fleet data +0x40; null in hyperspace): error "taskforce is
   not in orbit at a planet".
3. The surface is not accessible (0x375220(planet type, planet)): warning "surface of %s is not accessible".
4. Take the fleet's direct members (0x432730 / 0x432710) that are TRANSPORTs (+0x337 ≠ −1), as ids (+0x50). There is
   no cap, no owner filter, no combatant test and no enemy test. None: warning "No transports in Taskforce %s.".
5. `Land_Invade`(planet's GOM (+0x2b8), planet id, ids, attacker = the fleet owner's player id, defender −1, map 0,
   at_front 0). It queues only when attacker ≠ the computed defender (BATTLE_TRANSITIONS §1.3). Then it returns a
   blocking object (0x511820, kind 0x80a, bound to the task force and planet by 0x511950, pushed by 0x256d40)
   [M: what the block waits for].

**Port.**
- Today: there is no INVADE_PLANET handler in the reward switch of `engine/apps/sim/src/story_session.cpp`.
- Rule:
  1. Run steps 1–6 above: Param1 = planet type name (first object of the type); Param2 = an exact company-type name
     filter; units = the direct transports of fleets in orbit friendly to the plot's player; combatant check.
  2. Issue one Invade order on the queued path with player = the local player and only the first passing unit.
  3. Let the galactic executor apply its gates and owner filter.
  4. A planet that is not found: log and stop (the original crashes).

### 31.7 Implemented (a1h t03 / t04, 2026-11-02)

- t03, Invade event executor (`GalacticSim::execute`, `EventType::Invade`): dropped (NotAllowed) while the galactic
  pause counter is raised, or while a story-linked battle is pending and the local player is attacker or defender of a
  conflict in either queue (`ConflictHooks::in_queued_conflict`, 0x2b0350; `ConflictManager` answers, the battle host's
  gate forwards). Then the owner filter as before. The galactic AI's `GalacticTaskForce.Invade` no longer issues the
  event: it calls `GalacticSim::invade` (Land_Invade) directly with the task force's player (0x6ce600), counted and
  logged like an order, so neither gate applies to it.
- t04, story reward INVADE_PLANET (`StorySession::invade_planet_reward`): §31.6 steps 1–6, one unit, issued as the local
  player through the pipeline's direct path at the reward's frame [port: the original queues it for the next command
  step]; a planet type that is not found is logged ("story: INVADE_PLANET unable to find planet") and dropped [port: the
  original crashes]; the combatant test vf+0x138 is the unit's GroundUnit kind [port]; the planet is the first planet
  row of the type (as SABOTAGE_STRUCTURE); the Param2 mismatch line goes to the session's Lua messages.
- ~~Not changed: `paused()` still applies the linked-battle flag to every player for the other commands (REMAINING_GAPS
  §6.4 "[port: for every player]")~~ done in a1i t03 (below).

**a1i t03 (2026-11-02): every caller of 0x2b0350 [H].** The call graph lists seven callers, and all seven use the same
test. With S = "the flag 0x140b154d0 is set, or the list 0x140b2b360 is non-empty (never written, REMAINING_GAPS §6.4)"
and "not multiplayer" (0x28af60(GMM) == 0), the **story block** is `S && !MP && 0x2b0350(GOM)`. 0x2b0350 compares
records with the **local player** id (0x294a70) only. None of the callers looks at the event's issuer, so an AI
player's command is blocked exactly when the human's is.

| caller | what it is | GOM passed to 0x2b0350 | effect of the story block | pause counter GMM+0x2c |
|---|---|---|---|---|
| 0x4b2360 | `GalacticModeClass` vf9, the galactic "paused" query (MAIN_LOOP §1.5 slot +0x48) | the mode's own (+0x18) | returns true | > 0 → true, tested first |
| 0x2be640 | GOM service tail (BATTLE_TRANSITIONS §1.4 step 2) | its own | the object service takes the paused branch (as for the counter) and the conflict processing does not run this tick | (galactic mode) > 0 → the paused branch |
| 0x3c4460 | `MoveThroughObjectsEventClass` vf4 | current mode's (0x140b15418 +0x18) | event dropped | galactic mode and > 0 → dropped |
| 0x48ff50 | `InvadeEventClass` Execute (§31.2) | current mode's | event dropped | same |
| 0x4b4d70 | `SpecialAbilityEventClass` vf4 (every kind) | current mode's | event dropped | same |
| 0x5af260 | `FleetManagementEventClass` vf4 (every op, 8 included) | current mode's | event dropped | same |
| 0x68f5b0 | Lua `GalacticFreeStoreClass.Move_Object` | current mode's | warning "Galactic mode is currently paused. Move not possible, try again later.", returns false | > 0 (any mode) → same refusal |

- The event executors return with no message. The block is tested before the pause counter, but both only drop.
- Not gated: `MoveObjectToObjectEventClass` vf4 0x3af410 (it resolves the object and calls 0x39f580, no prologue),
  `ProductionEventClass` vf4 0x524130, and `GalacticTaskForce.Invade` 0x6ce600 (§31.6).
- So the paused query and the five command gates all compute "pause counter > 0, or the story block". The only
  difference is the GOM: 0x4b2360 asks its own mode, while the executors ask the current mode. In galactic mode, where
  these events run, the two are the same [M for a galactic event executed while a tactical mode is current].

**Port (a1i t03).**
- `GalacticSim::story_block()` = `linked_battle_pending && in_queued_conflict(local player)` (the port is
  single-player only).
- `GalacticSim::paused()` = `pause_ > 0 || story_block()` (0x4b2360). Every port gate keeps calling `paused()`, so all
  of them now follow the original:
  - `fleet_management` (0x5af260);
  - MoveThroughObjects (0x3c4460);
  - `special_command` (0x4b4d70);
  - the Invade case (0x48ff50, now `paused()` too);
  - the galactic AI's free-store `Move_Object` (0x68f5b0);
  - the [port] unit-split path of MoveObjectToObject (the HUD's drag; it stands in for a FleetManagement split).
- The other readers of `paused()` follow 0x4b2360 too: the galactic AI service and its task-force MoveTo status, the
  free-store hop, the reveal delay and the story Lua clock.
- The whole-fleet MoveObjectToObject stays ungated, as 0x3af410.
- 0x2be640 is unchanged: `CampaignBattles` leaves its `story_sequence_active` seam unset (BT §2.9.3: the port starts a
  linked battle at once; see campaign_battles.hpp).
- Still [port]: `GalacticSim::update` freezes the mode frame only for `pause_ > 0`. 0x4b2360 also freezes it while the
  story block holds.
- Tests: `alamo_galactic_conflict_tests` `test_linked_battle_gates` (AI Underworld commands with the local Empire in no
  queued conflict → FleetManagement launch runs, SpecialAbility reaches its target check; after the AI's direct
  invasion of the Empire's planet → launch, MoveThroughObjects and SpecialAbility NotAllowed, MoveObjectToObject runs;
  flag cleared → not paused with the conflict still queued); `alamo_galactic_special_tests` (the flag alone, with no
  queued conflict, does not pause).

## 32. Reinforce delivery handlers and the transport carried type (a1h t05 / t08) [H unless tagged]

Read 2026-11-02 from the decompiled code, the RTTI of StarWarsG.exe and the call graph.

### 32.1 Who calls 0x6bf3d0: the land ReinforceBlockStatus unload handler; the space status does not

- **Vtables (RTTI).** ReinforceBlockStatus: primary 0x8b9990, signal-listener base at status +0x30 0x8b9a40 (COL
  offset 0x30), slot 1 = **0x6a1440**. SpaceReinforceBlockStatus: 0x8ce108 / 0x8ce1b8 (offset 0x30), slot 1 =
  **0x723ac0**. So both are the statuses' signal handlers (this = status + 0x30). 0x6bf3d0 has one caller, 0x6a1440.
- **0x6a1440** (land; signal 0x19 with ReinforcementsUnloadedDataClass data, sent by the TransportLanding unload finish
  0x6438a0 with the transport pack's ReinforcementsUnloadedDataClass at +0x98):
  1. Stops listening to 0x19 on the sender (0x220eb0).
  2. The data must be a ReinforcementsUnloadedDataClass (class chain); otherwise nothing more.
  3. In-flight count (status +0x40) − 1.
  4. Owner = status +0x28 (0x6021a0). When its class chain contains TaskForceClass (descriptor 0x140a441e0, returned
     by TaskForceClass vf4 0x5fcd90): a static "handled" list is cleared, then for each non-null unit of the data list
     (+0x10, count +0x18), in list order:
     - the unit's container (+0x2b0) stands for it when it has one; an object already handled in this call is skipped;
     - it is wrapped (GameObjectWrapper 0x5751d0, the task force's player) in a one-entry list and the task force's
       `Add_Force` (vf+0xa0; land override 0x6e9240) gets it;
     - **0x6bf3d0(PP, object)** with PP = task force +0xf0 (plan) → +0x88 → +0x68 [M field names]: admit to the free
       store if missing (0x4d2c90), +0x30 = PP, the PP listens to 0x1a / 0x10 on it, PP+0x108 gets it (§28.4).
  5. Otherwise (owner not a task force) [M: which caller makes such a status]: each unit (its container when 0x39b1a0
     says it is garrisoned) not yet in the status's own result list (+0x48, compared by wrapped object) is appended as
     a wrapper for player status +0x50 and removed from its owner's free store (owner's player +0x360, the AI of the
     current mode (0x4b03b0) → +0x50 → +0x38, vf+0xa0 = 0x4d5000).
- **0x723ac0** (space): the sender is the arriving object (listener at obj +0x38). In-flight count (status +0x78) − 1.
  Owner a TaskForceClass: `Add_Force` (space override 0x6e4e30) with that one object, then the object is **removed
  from the free store** (0x5f6fb0(plan) → +0x28 → +0x50 → +0x38, vf+0xa0 = 0x4d5000) [M: the chain ends at the same
  AI-player free store as 0x6bf3d0's]. **No 0x6bf3d0: no +0x30, no PP+0x108.** Owner not a task force: as land step
  5 for the one object, without the container step.
- **Port (a1h t05): matches, no code change.** `space_reinforce_poll` adds the spawned units to the task force and
  calls no `reinforce_join_pp`; `admit` skips task-force units, so the unit is not on the free store (the 0x4d5000
  removal). Land keeps `reinforce_joined` (a1g, §28.10). Test: alamo_ai_tactical_world_tests `test_group_orders`
  (space run): drops happen and no "reinforce unit" PP decision is logged.

### 32.2 Transport data +0x48 = the carried company type

- The pack at obj +0xc8 is **TransportDataPackClass** (ctor 0x4ed470), shared by TRANSPORT and TRANSPORT_LANDING (the
  cinematic landing fields of SCRIPT_GAPS, +0x50 / +0xbd / +0xc0 / +0xc4 / +0xc8 / +0xc9, are in it; the ctor's
  +0xc0 = 1.0 and +0xc4 = 2.0 are the Create_Cinematic_Transport defaults). The ctor sets +0x48 = null.
- Reader: 0x4f3bb0 (TransportBehaviorClass), used by 0x3973b0. Writers (call graph; a linear scan of .text for stores
  to +0x48 through a +0xc8 load finds only 0x4f45c7):
  - **0x4f45c0(transport, type)**, from 0x400f30 (galactic company production, STRATEGIC_GAPS §1.7): the company type.
  - **0x4ee230(pack, type)**, from 0x2b5fe0: +0x48 = type; when type ≠ null and pack +0x70 ≠ null, pack +0x70 → +0x30 =
    type +0x11c [M meaning].
- **0x2b5fe0 = the landing object of one land reinforcement** [H code, M names]:
  - campaign (the unit id is found in the parent mode, 0x28aa50): landing type = the galactic object's type, or its
    +0x1e68 [M: landing variant] when set; created at the drop point (z raised by Hover_Offset +0x1b68); obj +0x54 =
    the galactic object's id; TRANSPORT_LANDING attached by name; **+0x48 = 0x3973b0(galactic object)**, i.e. the
    galactic transport's company type;
  - skirmish (the id names a type of the player's reinforcement list): landing type = that type's
    Company_Transport_Unit[0] (+0x938 → +8), or its +0x1e68; **+0x48 = that type** (the reinforced company);
  - both then call 0x29bcd0(mode, landing object, carried type) [M: not read].
- Cinematic transports (Create_Cinematic_Transport 0x2cb7a0) never set it: +0x48 stays null.
- So **0x3973b0(landing object) = the carried company type** for a reinforcement drop in both modes, and the object's
  own type for a cinematic transport (the TRANSPORT branch is null, the TEAM branch is not tried).
- Effect on the §30.3 sums: a reinforcement drop counts its company type's power (0x370e60), not the dropship's (the
  shipped `_Landing` variants inherit AI_Combat_Power 10 from their base transports); a cinematic transport counts 10.
  The campaign parent route gives the same company type.
- **Port (a1h t08):** `ObjectStore::carried_type` / `set_carried_type` (TransportDataPack +0x48; only objects with
  TRANSPORT or TRANSPORT_LANDING; saved; hashed only when an object has one). The perception landing sum takes T from
  `rep_type` (the host's 0x3973b0) in skirmish / without a parent and from `parent_rep_type` in campaign; test (d) in
  alamo_ai_tactical_perception_tests. The port's galaxy needs no carried type: its company object's own type is the
  company type (`GalacticSim::representative_type`). [open] The battle host spawns no landing transports yet (units
  drop directly, TACTICAL_GAPS §4.2.1, a1h t06); when it does, it sets the carried type as 0x2b5fe0 does and fills
  `PerceptionObject::rep_type` from it.

## 33. Task-force Guard_Target and its block status (a1h t02) [H unless tagged]

**Question.** Shipped `Escort(tf, target)` (PGTaskForce.lua) calls `tf.Guard_Target(target)` without blocking, once per
second while no member `Has_Attack_Target()`. `SecureBuildPad` blocks on `Guard_Target(AITarget)` for up to 60 s, with
`Target_Has_Structure` as the break function. Four things to settle:
- what order the guard gives;
- when its block status finishes;
- whether anything re-orders the units to follow the target;
- what happens to an older status when a newer order arrives.

Port today: every Guard status stays alive until the plan ends and re-issues a guard order every 150 frames.

### 33.1 `LandTaskForceClass::Guard_Target` (0x6ebdc0)

**Arguments:** `(target [, threat [, useZonePath]])`, parsed like `Move_To`. The 3rd-argument type warning even reads
"LandTaskForceClass::Move_To -- Unexpected type for parameter %d", copied from Move_To.

| Target | Resolved to |
|---|---|
| AITargetLocation | its game object (target vf+0x30) when it has one, else its position (vf+0x68). A null AI target returns nil silently |
| GameObjectWrapper | the object (0x5791e0). A dead wrapper returns nil silently |
| Position | the point (+0x28..+0x30) |
| Task force | one member, picked with `Get_Int(0, n−1)` on the synced LCG (0x1ffb40; no draw when n = 1). An empty target TF gives the warning "target taskforce is empty.  Cannot guard." and nil |
| anything else | warning "Parameter 1 is not a valid destination", nil |
| no argument | warning "No destination set", nil |

- **threat:** a Lua number, default 1e18 (none). It is stored in the status (+0x50) and becomes the move block's
  threat tolerance (+0x54).
- **useZonePath:** a Lua bool, default false (+0x54). It gives the same zone-by-zone routing as `Move_To` (§25.3). No
  shipped land caller passes it.

**Status.** A new `LandMovementBlockStatus` (0x73baf0, 0x90 bytes, Lua id 0x819), set up with **mode 2**:
- object target: 0x73c4f0(status, tf, obj, threat, useZonePath, attack = 0, mode 2);
- position target: 0x73c360(status, tf, pos, threat, useZonePath, mode 2).
- Move_To uses mode 0 and Attack_Move mode 1, through the same two set-up functions.

**Return value.** Guard_Target returns **nil** when the status is already finished right after set-up (0x73cab0:
+0x40 set, e.g. no task force or a failed zone route). Otherwise it returns the status.

**Object set-up (0x73c4f0):**
- stores the object (+0x48) and the mode (+0x88);
- subscribes the status to the **object's** signals 0x1a [M: owner change] and 0x10 (destroyed);
- leaves the attack flag +0x56 at 0 for a guard, so **no `Order_Attack`** is issued (Attack_Move sets the flag;
  TACTICAL_COMBAT path A).

**Order given (object target, 0x73c140(status, obj, collect = 1); position targets use 0x73bf40 the same way):**
1. **Collect the members** (vf21, slot +0xa8, 0x73c6f0 [M: slot number]). For each TF member in TF +0x78 list order
   that is not movement-locked (0x39ad40):
   - if it is admitted to coordinators (0x425e30), take the member itself;
   - if it is not admitted and is a TEAM container, take each team member instead.
   - Each taken unit first has **signal 0xb raised on it** (0x220ed0).
   - The status then subscribes to the unit's signals 9, 10, 0xb, 0x10 and one more [M: 0x1a, which the handler
     accepts], and appends the unit to the all-units list (+0x58).
2. **Move them to one new coordinator.** Each listed unit that passes 0x425e30:
   - leaves its old coordinator (0x426d50);
   - joins **one new coordinator** (0x426010);
   - is appended to the pending ("still moving") list (+0x60).
   With no coordinator system the status finishes at once (+0x40).
3. **Call coordinator `Move_To` (0x4f8200)**, the same entry a player's move uses. The move-parameter block holds:
   - kind 3 (object) with the object, or kind 1 (position);
   - **flags = the mode = 0x02 (guard-move)**;
   - avoidance 0x3f;
   - threat = argument 2 (1e18 means none);
   - no hardpoint; the +0x60 word = 0x1000001.
   An empty coordinator is deleted (0x4260d0).

**Is it the player's guard order?** The player's guard is `EscortEventClass`: event id 0x15, which is COMMANDS order 21
"Escort" (constructor 0x3b0010, executor 0x3b0040). Its block is the same: kind 3, flags 2, mask 0x3f, threat 1e18, plus
an optional hardpoint. Differences:
- The player's executor first clears each unit's behaviour orders 5, 0x22, 0x29 and 6 (0x38f5d0). The AI path does
  not.
- The player's executor replaces a guarded team member by its container (obj +0x2b0). The AI path passes the object as
  given.
- Formation `Set_Move` (0x423c10) ignores the block when the target is inside a TEAM container in game mode 2 [M: mode
  2 = land]. So an AI guard of a single squad member does nothing [M].

**`Has_Attack_Target` (0x57a170)** is true when 0x397a40 returns non-null:
- first `Formation_Attack_Target(F, −1)` (TACTICAL_COMBAT, 0x41a2e0), which **skips guard blocks**;
- else the targeting pack's current target (obj +0xf0 → +8).
So for a guarding unit it is true only when the unit's own targeting has a target.

### 33.2 Block status: IsFinished, Result, signals

- **IsFinished** (vf19 0x73cae0):
  - if the zone-route-leg flag +0x55 is set, it first issues the next zone leg (0x73bc50(status, 0));
  - it returns true when +0x40 is set **or** the task force has no members (TF +0x80 − +0x78 = 0, 0x5fca10).
- **Result** (vf20) is the BlockingStatus default 0x17d070 (no value).
- **Polling is passive.** IsFinished never re-issues an order, with one exception: the zone-route leg above, which only
  happens with useZonePath. Legs are issued without re-collecting (collect = 0), so they raise no signal 0xb.
- **Signal handler.** The handler is 0x73ce80, on the SignalListener sub-object at +0x30; the offsets in this section
  are from the status.

| Signal | From | Effect |
|---|---|---|
| 9 | a unit | ignored |
| 10 (movement finished) | a unit | Remove the unit from the pending list. Fire Lua TF event 0x12 [M: `Unit_Move_Finished`] for the unit; for a team member, once for its container, when no other member of that team is still pending. Then run the pending-empty test below |
| 0xb, 0x10, 0x1a | the guarded object | Unsubscribe from it. 0x10 (destroyed) or 0xb: **finish** and release the object. 0x1a, any mode: finish when the attack flag is set and the object is no longer hostile (0x39a4e0). 0x1a, **guard** (mode bit 0x02): also finish when the object **is now hostile** to the TF's owner. Otherwise nothing |
| 0xb, 0x10, 0x1a | a unit | Unsubscribe the unit and remove it from **both** lists. If the pending list is now empty, run the pending-empty test. Then **finish when the all-units list is empty** |

**Pending-empty test** (every unit still listed has arrived):
- A zone route is in progress (+0x78 and +0x80 set): set +0x55, so the next poll issues the next leg.
- No object (a position move or guard): **finish**.
- An object:
  1. Mode bit 0x02 clear (move / attack-move): finish when the object is no longer hostile (0x39a4e0) or fails 0x5680b0
     (TACTICAL_COMBAT path B, step 3).
  2. Every mode: finish when the object's type has neither `Last_State_Visible_Under_FOW` (+0x88) nor
     `Initial_State_Visible_Under_FOW` (+0x89) **and** the object is fogged for the TF's player. This test
     (0x35f470(…, force = 1)) applies fog even to an AI player.
  3. Otherwise the status **stays open after arrival**.

**When an object guard finishes.** Any one of these finishes it:
- the guarded object dies (0x10);
- the guarded object receives signal 0xb, meaning it was given a new order (§33.3);
- the guarded object turns hostile (0x1a);
- the TF becomes empty;
- every unit of the status is gone (died, changed owner or taken by a newer order);
- after all units arrived, the object is fogged for the AI. This is unlikely, because the guards stand next to it [M].

It does **not** finish when the units arrive.

**When a position guard finishes:** when every pending unit has reported movement finished (signal 10), as `Move_To`
does; or when all its units are gone; or when the TF is empty.

**Modes 0 and 1 (AI_TACTICAL open item 3).**
- The status has **no arrival radius of its own**. "Arrived" means the formation's signal 10 (§33.4).
- The rest follows the table above.
- Object targets of Move_To / Attack_Move stay open after arrival while the object lives and stays a valid, hostile,
  unfogged target.
- This matches the space version (TACTICAL_COMBAT, SpaceMovementBlockStatus).

### 33.3 Superseded statuses

**Who raises signal 0xb.** The signal means "a new order replaces this unit's order" [M: name]. It is raised on a unit
by:
- every task-force movement collector: land 0x73c6f0; space 0x739200 and 0x73a650;
- the unit-level Lua `Move_To` / `Attack_Move` (`Move_Internal` 0x57da90);
- `Activate_Ability` (0x570a50) and `Turn_To_Face` (0x581cd0);
- the MoveToObject executor (0x39f380 / 0x39f580);
- garrison enter and exit (0x635ee0..0x636030);
- 0x6affb0, ConcentrateFire (0x6f5550), 0x3abb80 and 0x4ca930.

Unit auto-targeting does **not** raise it.

**What a newer order does to older statuses.**
- A newer TF order (Move_To, Attack_Move, Attack_Target, another Guard_Target …) raises 0xb on each unit it collects.
- Every older status that listens on those units removes them.
- An older status whose all-units list becomes empty **finishes**. A status that still holds other units stays open
  for them.
- A superseded status never acts again: it holds no order of its own to re-apply, and the units' movement now belongs
  to the newer coordinator.
- A unit belongs to at most one live movement status at a time [M: through signal 0xb; true for the land and space
  collectors read here].
- Plan end destroys all of the plan's statuses (AI_GALACTIC, "Plan end" step 1).

**`Escort`.** Each re-issue (once per second) creates a new status, and its 0xb finishes the previous guard status (all
its units are taken). Statuses therefore never pile up.
- The re-issue does restart the group move: units leave the old coordinator for a new one, and `Set_Move` sets the
  formation state to 0 (re-plan).
- The script's own comment says that re-issuing a guard "will cause the assisting escort to break off attack".
- The AI guard path has no explicit target clear: no 0x38f5d0 and no `Order_Attack`, and 0x4f8200 / `Set_Move` do not
  touch obj +0xf0 [M].
- So any break-off comes from the restarted movement and from the targeting scan's guard leash (`Guard_Chase_Range`,
  §33.4), not from a direct reset [M].

**`SecureBuildPad`.** `BlockOnCommand(Guard_Target(pad), 60, Target_Has_Structure)` normally ends by the 60 s timeout or
the break function, because the guard status stays open while the pad lives.

### 33.4 Unit-level guard behaviour (formation, flag 0x02) [M unless tagged]

**Following a moving object needs no further AI order** [H for the mechanism]. The formation's periodic update 0x4192b0
runs at most once per FPS = 30 frames (+0x1d8 = now + 30). With a kind-3 goal it does two things:
- **Guard, with the guarded object moving on a path** (its locomotor has a current path, 0x5569b0, with state ≠ 5):
  - when the formation is within R of the object, it **copies the object's path** (0x40dbc0);
  - it caps its speed (+0x21c, +0xa8) at min(own speed, 2 × the object's speed). Exact rule (2026-11-02, §33.4.1):
    the object's **effective max** speed, or its formation's speed while it moves in a formation, never its current
    velocity;
  - with more than one block queued and the object not moving, the guard block is popped.
- **Any kind-3 goal, with the object unfogged for the formation's owner:** the formation **re-paths toward the object**
  (0x417520) when both hold:
  - the object's current goal point (0x583150) is more than R from the formation's recorded goal point
    (+0x210..+0x218, set by `Set_Move`);
  - 0x4165f0 agrees. It always agrees for a guard block (flags 0x02) [H].
- **R** = half the lead member's max attack range (0x3749d0 / 2). For a guard block R is capped at a constant:
  0x140b276c0 = GC +0x11EC0 `Space_Guard_Range` (FoC **750**) in space (mode 2), 0x140b276c4 = GC +0x11EC4
  `Land_Guard_Range` (FoC **150**) on land (mode 1). This corrects the earlier "6c0 land / 6c4 space"
  (2026-11-02, §33.4.1).

**Arrival.** Once all members' locomotors report done, the formation has one block and its state is 1, 0x423760 raises
signal 10 on every member and sets state 3 [H]. The guard block stays as the formation's goal, so a later move of the
object re-paths the formation again.

**Auto-engage.** The unit's own targeting scan reads the first block's flags:
- flag 0x02 gives the response range `Guard_Chase_Range` (type +0x52c), measured from the slot goal;
- a candidate that is attacking the guarded object adds type +0x211c to that range (TACTICAL_COMBAT, 0x4160a0 steps
  2–5);
- the scan sets the unit's target (ordered 0), so `Has_Attack_Target` turns true;
- `Formation_Attack_Target` skips guard blocks, so path B never locks onto the guarded object (a friendly guarded
  object is not hostile anyway).

**Guard on a position.** The units move there in formation and stop (signal 10, state 3), and the status finishes.
They then auto-engage within `Guard_Chase_Range` of the slot goal. Whether they walk back after a chase was not traced
[L].

#### 33.4.1 Formation update 0x4192b0 for an object goal, exact (RE, 2026-11-02) [H unless tagged]
Notation: F = formation; blocks F +0xf8..+0x100, 0x68 bytes each; "last block" = blocks[n−1] (0x41a4e0 / 0x41af10).
Flag 0x02 = guard, block +0x24 = kind, +0x28 = object.
1. **Gates.** It returns when:
   - F has no members;
   - frame < F +0x1d8;
   - F has no lead (0x41a9b0, which also returns the lead position L);
   - F has one member and that member's locomotor state is 0x26 (Land_Fly circling).

   Otherwise F +0x1d8 := frame + FPS (30). T is the lead's type, read from its team container, else the company
   type (0x3973b0), else its own type.
2. **R** = `Get_Base_Attack_Range`(T, lead) / 2. When the last block has flag 0x02: R := min(R, `Space_Guard_Range`
   750) in space, min(R, `Land_Guard_Range` 150) on land (other modes: no cap).
3. **Guard follow / pop**. This applies only when the last block has flag 0x02 and an object, **and** T's
   `Space_Layer` (+0x16dc) is 0 or T has `Disregard_Space_Layer_For_Guard_Attack_Move` (+0xe9, unset in FoC).
   - FoC lead types with layer 0: squadrons, fighters, GroundCompany, HeroCompany.
   - Capital / frigate / corvette ships and solo `Land`-layer vehicles / heroes skip this step and use only step 4.
   - **The guarded object counts as "not moving"** when it has no locomotor pack, no current path (0x5569b0), path
     state 5, or a formation whose last block has flag 0x10 [L: meaning of 0x10].
     - If F has **more than one block**: the **last** block is popped (F +0x100 −= 0x68) and F state (+0x1d0) := 0.
     - If no remaining block names the object, F stops listening to the object's signals 0x10 / 0x1a / 0x2e
       (0x220eb0) [M: unsubscribe].
     - Then step 4 runs on the new last block. With one block nothing is popped.
   - **Moving** (the object is following a path):
     1. F speed (+0x21c and +0xa8) := the formation speed (0x41b2a0, slowest member). This resets any earlier cap.
     2. If |L − object position| < R (3D, strict), compute S:
        - S = the object's effective max speed `0x370f00`(company type, object), **not its current speed**;
        - when the object is in a formation whose last block lacks flag 0x10, S = that formation's speed +0x21c (the
          speed it is travelling at).
     3. The cap is min(F speed, 2 × S). Copy the object's current path into F's path (+0x68, 0x40dbc0); F speed :=
        the cap.
     4. Every member that is not deployed / deploying (or has type +0x23b0 ≠ 0) gets the new path (locomotor
        vf +0x158(0), vf +0x190(path), vf +0x148(0)). Its waypoint index (path +0x60 / +0x64) is set to the object's,
        so the escort joins the object's path at the object's progress.
     5. When the copied path has one node, F state := 3.
     6. Out of R: no copy and no cap (the speed reset above stands); go to step 4.
4. **Re-path** (when step 3 did not copy), with the last block.
   - It is skipped when the block's object is fogged for the lead's owner (0x35f470).
   - D = |F recorded goal (+0x210 / +0x214) − the block's current goal point (0x583150)| (2D).
   - **No re-path** when any of these hold:
     - space, the lead has no TEAM slot, the block is kind 3 on a team object, and 0x4f72e0 accepts it;
     - space, kind 3, and the lead's and object's locomotor packs share +0x1c4 / +0x1c8 (≠ −1) [L];
     - D ≤ R, and (the last block is not a non-guard object block, or 0x40edd0, or the object is not moving).

     In the D ≤ R case, idle members get their speed refreshed in one sub-case [L].
   - Otherwise, when 0x4165f0 agrees (always for a guard block), it re-paths with 0x417520(members, lead, L, 2) and
     re-issues the members' paths as in step 3.
5. **No re-path.** Step 4 runs only when n < 2, or when blocks[n−2] is not a guard block with an object, or when
   0x4165f0 agrees. When it ends without re-pathing (0x4165f0 false) or was skipped, and the last block lacks flag
   0x04 (player order), F re-forms (0x4220d0, when T has a `Space_Layer` and the mode is space) or releases
   (0x4105a0). A guard last block never gets here, because 0x4165f0 is always true for it.

For FIGHTER_FLYING §4.10 E (squadron escorting its carrier):
- R = min(half the squadron's base attack range, 750).
- Within R the squadron flies the carrier's own path at its index, capped at 2 × the carrier's max speed (or 2 × the
  carrier formation's speed while the carrier moves in a formation).
- When the carrier stops with no other queued block, the guard block stays; with a queued block it is popped.

### 33.5 Rule for the port

1. **`TF.Guard_Target(target [, threat [, useZonePath]])`:**
   - resolve the target as in §33.1 (a TF target gives one `Get_Int(0, n−1)` member);
   - issue **one** group move to the TF's collectable members: kind object (or position), **guard flag**, threat;
   - give no attack order and clear no target;
   - return nil when nothing could be set up, else a status.
2. **Do not follow the target by re-ordering from the status.** Following belongs to the unit / formation movement:
   - at most once per 30 frames, re-path to the guarded object when it is more than R from the recorded goal;
   - copy the object's path when it is moving.
   If the port's movement layer cannot do this yet, put the stand-in in movement, keyed to the formation's guard goal.
   It must never be a per-status timer that issues new TF orders.
3. **The status is passive.** It finishes:
   - **object guard:** when the object dies, gets a new order (signal 0xb) or turns hostile; when the TF is empty; when
     all its units are gone; or, after arrival, when the object is fogged for the AI;
   - **position guard:** when all units have arrived, when all units are gone, or when the TF is empty.
4. **Every new order removes its units from all older statuses.** This covers every new TF movement, attack or guard
   order, and also unit-level Lua moves, abilities and garrison.
   - An older status left with no units finishes.
   - **Superseded statuses must stop acting entirely**: they never issue orders again.
5. **Modes 0 and 1 use the same rules.**
   - Arrival is the formation's "movement finished"; the status has no radius.
   - Object targets stay open after arrival while the target is a valid, hostile, unfogged object.

> **Status 2026-11-02 (a1h t02):** implemented. `tactical_ai.cpp`: the Guard status is passive (Result nil); an object
> guard ends when the object dies or the TF empties, a position guard when no unit moves any more; the 150-frame guard
> refresh and the Move status's "attack-move follow" re-orders are gone; a new TF move / guard / attack order finishes
> the TF's older Move / Guard / AttackTarget statuses (`supersede`). `battle_ai.cpp`: object-goal group moves (Guard_Target,
> Attack_Move / Move_To on an object) get a follow record, re-pathed at most every 30 frames when the object is more than
> R = half the lead's attack range from the recorded goal; any newer order for a unit removes it from its record
> (AI save v3, hashed). [port] Not yet: the "turns hostile" and "fogged after arrival" ends of an object guard; unit-level
> Lua orders, abilities and garrison orders do not supersede statuses; the guard cap constant (0x140b276c0) is [L].

## 34. Battles that never end: reserve units, one plan library, slow station sieges (a1i t01 / t02) [H unless tagged]

**Question.** `alamo_headless --skirmish-ai foc-land --seed N --ticks 20000 --expect-battle-end` reached the 14400-frame
cap on 11 of seeds 1-12 (only seed 5 ended), and foc-station on seeds 1 and 6 of 1-8. A1h named two land causes:
fog-hidden Rebel infantry the Empire never reaches, and an undeployed Rebel reserve that kept the side alive under the
port's "reserve counts" rule (BT §3.1a said pool units never count [M]).

### 34.1 Reserve units never keep a side alive (0xb) [H]

- 0x344710 case 0xb walks only the **tactical** GOM's object list (0x2a9ff0 on the tactical mode's GOM, gom +0x38).
- A reserve unit is a galactic persistent object in the parent mode's lists (TACTICAL_GAPS §4.2). Its tactical object
  (the landing object, §32.2) is created only when it drops (0x2b5fe0). Until then nothing of it is in the list 0xb walks.
- 0x2c0340, the only 0xb trigger, reads no reserve or pool state.
- So a side whose last victory-relevant tactical object leaves loses by 0xb with reserves left. BT §3.1a item 1 is now [H].

**Port (a1i t01).** `TacticalBattle::check_victory` no longer counts a never-deployed participant (the rule came from
l1sys t06, `[port]` "not adopted until probed"). No probe is needed. Left [port]: the check is still polled each tick,
not event-driven. The "empty side gets its next pool unit" step in `reinforce` runs at the start of the next tick, after
the victory check of the tick in which the side emptied, so it no longer saves a side.

### 34.2 One plan library for every mode [H]

- AI_GALACTIC §1.9.1: the loader 0x4e7e30 loads `AI\`, `AI\SpaceMode\`, `AI\LandMode\`, `AI\StoryArc\` and
  `Interventions\` into **one** plan manager. A plan's folder does not restrict its mode; only its `Category` does.
- Shipped FoC data has two plans that serve a mode outside their folder:
  - `AI\SpaceMode\RetreatPlan.lua`: `Category = "Space_Retreat | Land_Retreat"`. It is the **only** plan for `Land_Retreat`.
  - `AI\AI_Plan_ExpansionGeneric_GenerateMagicCashDrop.lua` (AI root): the skirmish magic cash drop goals for land and space.
- The port's `tactical_plan_files` loaded only the mode's own folder, so the land AI had no `Land_Retreat` plan. In
  foc-land seed 1 the Rebel attacker (one company on the map against four) proposed `Should_Retreat_From_Land` 64 times,
  and each proposal ended with "no potential plan".

**Port (a1i t01).** `tactical_plan_files` returns every plan folder in load order for both modes. Plans whose
`Category` names no goal of the mode are never picked. foc-station CRCs are unchanged on seeds 1-8. Every foc-land seed
now ends when the Rebel retreats (§34.4).

### 34.3 Why the Empire never reached the hidden infantry (seed 1, before §34.2) [findings, not changed]

These stalls still happen in any land battle where neither side retreats:
- **Scouting never runs.** `Land_Scout_Area` was proposed about 6578 times and failed 6496 times ("failed Plan"). The
  LANDSCOUT task force needs `Infantry | Air | LandHero`. The Empire's only infantry (two Imperial_Stormtrooper_Squad
  companies) was held by `Clear_Infantry` task forces. Those are `Set_As_Goal_System_Removable(false)`, so the free store
  never released them.
- **`Clear_Infantry` never gets past its first Attack_Move.** It blocks on `BlockOnCommand(Attack_Move(AITarget))`. The
  TF Move status finishes only when every unit reports movement finished (§33.2). The AT-AT (speed 0.8) never reaches
  its formation slot: it circles 150-190 units short among stopped friendly infantry and keeps its movement goal. One
  plan blocked from frame 196 to 5819, the next from 5917 to the cap. This is arrival in a crowded slot, owned by the
  movement layer. Follow-up for movement.
- **Seed 4: point-blank misses.** Both sides attack-moved into one area and parked in interleaved slots 34-89 units
  apart, with targets in range. The last 600 frames had 252 shots and 4 hits (TACTICAL_COMBAT §2.9 port differences).
  Follow-up for combat.

### 34.4 Seed table (headless, `--workers 2`, battle cap 14400 frames)

| Run | Before (main d1c8ccf) | After (§34.1 + §34.2) |
|---|---|---|
| foc-land 1-4, 6-12 | cap | end: Rebel retreats (0xc), frames 3045-5175 |
| foc-land 5 | end: Empire all-destroyed, 11636 | end: Rebel retreats (0xc), 2877 |
| foc-station 2, 4, 5, 7, 8 | end: Rebel all-destroyed | unchanged (same CRCs) |
| foc-station 3 | end: Empire retreats, 11967 | unchanged |
| foc-station 1, 6 | cap | cap; with a 40000-frame cap they end at 16344 and 17340 (Empire retreats, starbase lost) |

### 34.5 Station seeds 1 and 6: a slow siege, not a stall (a1i t02) [findings, no change]

- `Empire_Star_Base_2` launches TIE fighter / bomber squadrons and a Tartan without limit (`Reserve_Spawned_Units_Tech_0
  ..., -1`, with a data comment saying it keeps spawning until the fighter bay hard point is destroyed). This matches
  STATION_HANGARS_SOUND §2.4 item 6: spawning stops only when every FIGHTER_BAY hard point is destroyed.
- In seed 1 the Rebel keeps 5-7 ships on the map (population cap) and holds about 93 000 power against about 12 000.
  Until about frame 12600 its capital ships target the fresh squadrons, not the station. Station hit points fall from
  3300 to 1245 at the cap and to 788 by frame 15600; the station is lost at 16344. Seed 6 behaves the same way.
- Nothing shows a port divergence in the victory or AI code. Two things were not checked against the original: whether
  Rebel capital ships should prefer the station or its fighter bays over fighters (targeting priority sets), and whether
  the fighter bay hard points are ever targeted. Follow-up for combat. The foc-station test stays on seed 3.

## 35. The land reinforcement landing object, its unload, and the A1i drop leftovers (a1i t04 / t05) [H unless tagged]

Read 2026-11-02 from the decompiled code (0x2b5fe0, TransportLandingBehaviorClass 0x642c70..0x644330, 0x2a9670, 0x396850,
0x371d20) and the vtables of StarWarsG.exe. Pack field names are [M]; behaviour is [H] unless tagged.

### 35.1 Spawn: 0x2b5fe0(GOM, unit id, pos, player, fromCommand)

- **Mode.** Skirmish (0x28afb0 true): the id names a type T of the player's reinforcement list (0x407db0); it must be in
  the player's skirmish pool (player +0x2c8 / count +0x2d0), else nothing. Campaign: the id is looked up in the battle
  record's reinforcement list (record +0x18 → list +0x20 / count +0x28, by player); not listed → nothing. The galactic
  object G = 0x28aa50(id); the record entry is overwritten with 0x3fffff (taken) whether or not G exists.
- **Landing type L.** Campaign: G's type (G +0x298), the galactic transport that carries the company. Skirmish: T's
  `Company_Transport_Unit[0]` (+0x938 → +8); T without one → no landing object. In both, L := L's
  `Landing_Transport_Variant` (+0x1e68) when set (FoC: `Imperial_Landing_Craft` → `Imperial_Landing_Craft_Landing`,
  `Imperial_ATAT_Barge` → `Imperial_ATAT_Barge_Landing`, `Gallofree_Transport` → `Gallofree_Transport_Landing`, ...).
- **Carried type C.** Campaign: 0x3973b0(G) (the transport's company type, §32.2). Skirmish: T.
- **Position.** (pos.x, pos.y); z = terrain height at (x, y) (land mode, 0x135140) + h, with h = L's `Hover_Offset`
  (+0x1b68). When L's byte +0xd8 is set [M: field name] and C's first `Company_Units` member M (C +0x958 → +8) has no
  `Create_Team` and a land model, h = max(Hover_Offset, (box[2] + box[5]) × M's `Scale_Factor`), box = M's model vf+0x58
  [M: which box fields those are].
- **Orientation.** The issuer's entry-facing record (mode +0x288[player +0x48], TACTICAL_GAPS §4.2.2 item 1), whole triple.
  Created with 0x29f810(GOM, L, player, pos, orientation, 1, 0).
- Campaign: landing +0x54 (parent id) := G's id (G +0x50). Skirmish: none.
- **TRANSPORT_LANDING** is attached by name (0x38c570). When the landing has a TransportDataPack (+0xc8): **+0x48 := C**
  (0x4ee230), then 0x29bcd0(GOM, landing, C) [M: not read].
- Campaign afterwards: G leaves its orbit fleet (fleet-merge signal, 0x4f3bd0 lands it on the planet): galactic bookkeeping.
- Skirmish with fromCommand: 0x2862b0(player, T) [M: pool count − 1]; when 0x280c10(player, T) [M], pack +0xbc := 1 (the
  unloaded units are registered with the player, 0x2b5f70) and 0x2861a0 [M].

### 35.2 TransportLanding behaviour (non-cinematic path; pack = TransportDataPack, obj +0xc8)

| Field | Meaning |
|---|---|
| +0x48 | carried company type C (§32.2) |
| +0x50 | state: 4 waiting (hidden), 1 landing, 2 landed / unloading, 3 taking off |
| +0x58 | landing dust particle object |
| +0x60 / +0x64 | timer: start frame, length in frames (0x4ad3a0: (int)(FPS × seconds); expired (0x4ad350) when start + length ≤ now) |
| +0x68 | members unloaded so far |
| +0x78 | unload offsets (Vec3 list, 0x643130) |
| +0x90 | next offset index (one per team / vehicle) |
| +0x98 / +0xa0 | ReinforcementsUnloadedDataClass (signal 0x19 data) / signal 0x1d data |
| +0xa8 / +0xb0 | the unloaded objects (list / count) |
| +0xb8 | frame the behaviour was attached |
| +0xbc | register the unloaded units with the player (skirmish) |
| +0xbd / +0xc9 | cinematic / persist (SCRIPT_GAPS §3.1; never set by 0x2b5fe0) |

- **Attach** (vf9 0x643b00): state := 4, +0xb8 := GOM frame; play `Land` (anim 0x2b) at speed 1.15. If no clip plays:
  0x643130 and one unload step 0x644330 at once. Otherwise hide the model (0x264c80(model, 1), obj +0x3a2 |= 1, 0x265b70,
  obj +0x3a7 := 1); obj +0x3a0 |= 0x20.
- **Service** (vf6 0x643c30, every frame). p = current clip frame / clip frame count; no clip → nothing happens.
  - **State 4:** clearance 0x642ef0. Skirmish, non-cinematic, first: d = the player's tactical list +0x230
    (Battlefield_Modifier `Reinforcement_Time_Multiplier`: integer frame deltas round(F × m) − F, TACTICAL_GAPS §2.3.11)
    summed, optional clamps +0x240 / +0x244 (flags +0x248 / +0x249); wait while now − (+0xb8) < d (empty: no wait). Then
    the landing's footprint box (0x38de80, AABB 0x20dd40, z half-extent 100 at the landing position) is queried in GOM
    +0xd8: another TRANSPORT_LANDING object in a state other than 3 / 4, not persist, whose footprint intersects, blocks.
    Blocked: 0x3a7b30(obj, 0, 0) [M: rewind]. Clear: **0x642c70**: show the model, create the terrain's
    `Transport_Landing_Dust_<Temperate|Arctic|Desert|Forest|Swamp|Volcanic|Urban>` at the landing (+0x58), state := 1.
  - **State 1:** p > 0.98 → state := 2; non-cinematic: timer := 2 s × (1 + s), s = the player's list +0x2a8
    (`Reinforcement_Deploy_Time_Multiplier` − 1 entries; `Plus` / `GreaterThan` reduce), then **0x643130** (offsets).
  - **State 2:** timer expired → **0x644330** (one unload step); with mode flag 0x35e4c0 +0x30 [M: an instant-unload mode]
    steps repeat while the state stays 2.
  - **State 3:** p > 0.5 → the dust particle is removed; p > 0.98 → parent id := 0x3fffff, the landing is removed
    (0x391310(obj, 1)).

**0x643130 (offsets).** C's members (`Company_Units`, count +0x960); M = the first member; S = M's `Unique_Ground_Unit`
(+0x1b60) when M has no land model, else M.
- M without `Create_Team`: 0x40f970(cell of S (0x41b170), count): one offset per member (the vehicle grid of TACTICAL_GAPS
  §4.2.1).
- M with `Create_Team`: k = min(`Max_Squad_Size`, count). Team layout 0x5abc00((hx, hy) of S, 20 × M's `FormationSpacing`,
  k) when C's `Create_Team_Type` lists `LAND_TEAM_CONTAINER_LOCOMOTOR`, else 0x40f970(cell of S, k); only its bounds
  (Bx, By) are kept. Team grid **0x40f970((3·Bx, 3·By), ceil(count / k))**: one offset per team. The factor is **3.0**;
  the drop validity 0x3b6170 uses **2.5** (TACTICAL_GAPS §4.2.1).
- Every offset is rotated by the landing's yaw (obj +0x8c, degrees; small-angle shortcut as 0x3b6170):
  (x, y) → (cos·x − sin·y, sin·x + cos·y), z unchanged.

**0x644330 (one unload step).** C = +0x48 (campaign with a parent id: 0x3973b0 of the galactic parent); i = +0x68.
- i ≥ count: **finish 0x6438a0**, state := 3, play `Takeoff` (0x2c) at 1.15; no timer.
- Member i has `Create_Team`: n = min(k, count − i) members (types i..i+n−1) are created at the **landing's position**
  (obj +0x78), orientation = the yaw of offset[+0x90] (0x20acd0; index 0: the landing's own orientation), owner = the
  landing's. Per member: 0x3ac530(m, 1); its TEAM block (+0x40 / +0x48) cleared; shown (0x3a8550) unless the local player
  cannot see the landing [M]; locomotor pack +0x38 := 0; appended to +0xa8; the behaviour listens to its signal 0x28;
  parent id := G's id (campaign); with +0xbc: 0x2b5f70 (register with the player). The team helper 0x2a5790 groups them
  (failure: members deleted, return). Unless the instant flag: one group move (0x2a7b80) of the team to landing pos +
  offset[+0x90] with move-block byte +0x5c = 1 (the forced-move flag, §28.9 item 3), then every member and its container is
  **locked** (0x3a8e50(·, 1)). +0x68 += n.
- Otherwise (vehicle): one object of member i at the landing position (with `Unique_Ground_Unit`: the substitute is created,
  then the member, which rides it, 0x379d30); same registration. Not instant: with a locomotor whose mode matches and type
  byte +0xd4 set [M] → coordinator move (0x4f8200) to landing pos + offset[+0x90] (snapped by 0x5a4c10 for the movement
  class +0x1688), facing = the landing's yaw, forced-move flag, lock the unit (and its container); otherwise placed at the
  offset at once (0x3abb80) and turned to the landing's orientation. +0x68 += 1.
- Then +0x90 += 1 and **timer := 2 s × (1 + s)** again.

**0x6438a0 (finish).** 0x2b6a20(GOM, landing) [M]. For every unloaded object: unlock its container (when garrisoned,
0x39b1a0) and itself (0x3a8e50(·, 0)), clear the forced-move path flag (0x3ac210), pack +0x38 := 1 on it and its
container, 0x29bcd0(GOM, unit, C). Then **signal 0x19** with the ReinforcementsUnloadedDataClass (+0x98; its list = the
unloaded units), sent on the galactic parent (campaign, parent id set) or on the landing, and signal 0x1d on the GOM;
+0xb0 := 0. 0x6a1440 (§32.1) handles 0x19: in-flight − 1, Add_Force, 0x6bf3d0.

**Timeline of one drop** (no modifiers): attach → state 4 (hidden) until clear → `Land` at 1.15 to 98 % → offsets; after
2 s team 0 (or vehicle 0) leaves on a locked exit move; every further 2 s one more; 2 s after the last: unlock all,
signal 0x19, `Takeoff` at 1.15; removed at 98 %. A 15-trooper company (3 teams) reaches its task force Land + 8 s after
the order.

### 35.3 Rule for the port's battle host (a1i t04)

- The land Reinforce (`order_reinforce`: human command and the AI's TF.Reinforce) creates the landing object as 35.1:
  L = the company's `Company_Transport_Unit[0]` → `Landing_Transport_Variant` [port: the port's galactic pool object is the
  company itself, so the campaign route L = G's transport type is the same type]; z = terrain + `Hover_Offset` (the +0xd8
  vehicle term is not applied: byte name unresolved [M]); facing = the side's entry-facing record; parent id = the galactic
  id; TRANSPORT_LANDING added; carried type = the company type.
- Offsets as 0x643130 (team grid factor 3.0, rotated by the record's yaw). Team t's members end at landing pos + R(ψ)·(O_t +
  R(φ_t)·m_j) (`land_drop_layout(shape, x, y, yaw, 3.0f)`: the drop layout's team slots around the 3.0 team offsets);
  vehicle i at landing pos + R(ψ)·offset_i [port: the exit move and the formation slots it gives are not simulated;
  units are placed there, moved to the nearest passable cell of their movement class when the slot is not passable].
- No `Company_Transport_Unit` on the company → no landing object; the members drop at the validity layout's slots as
  before [port: the original spawns nothing visible for such a skirmish type and still unloads nothing; kept so
  synthetic data without transports works].
- Timing: the unload steps use the RE timer, 2 s × (1 + s) per step, s = `AbilitySystem::player_list(ReinforcementDeploy)`.
  The `Land` / `Takeoff` clip lengths are unknown to the headless host [M], so landing and take-off take no time [port];
  no clearance wait (state 4) and no skirmish `Reinforcement_Time_Multiplier` wait (its constant F is unknown).
- **Two modes (`BattleConfig::landing_unload_timed`).** The port's tactical AI (`tf_reinforce`) takes the units it
  finds right after the Reinforce order and finishes the status at once; it has no in-flight count and no 0x19 handler.
  - **Untimed [port] (the default until a1j, §36.3):** every unload step runs at the order (members placed at their unload slots), the 0x19
    record is posted at once (`TacticalBattle::landings_unloaded()`, cleared when the next tick starts); the landing
    object stays for the steps' timers ((steps + 1) x 2 s x (1 + s)) and then takes off (removed at once).
  - **Timed:** the reserve leaves the pool at the order (deployed) and the company stays inside. `landing_service`
    (each tick, after the reinforcements): state 4 (`landing_hidden`) until no other landing of this battle in state 1 /
    2 overlaps its footprint [port: the drop footprint box, not the 0x38de80 AABB; cinematic landings are not seen];
    state 1 → 2 at once (no clip time) with the step timer; each expiry spawns one team chunk / vehicle at the landing
    position (`deploy` member range) and sends it on one group move to its exit target (the 3.0 team offset or the
    vehicle slot, snapped passable) [port: no forced-move flag], locked (`landing_locked` joins `movement_locked` and
    the abilities' lock hook); the expiry after the last member unlocks them, posts the 0x19 record and removes the
    landing. A company still inside its landing keeps its side alive [port: §34.1 counts only the landing object by its own type]. Saved (battle state v3).
    A landing destroyed while unloading drops the members not yet out [M]. Members spawn facing the side's entry
    facing, not the offset's yaw [port].
- Perception: 0x3973b0 of the landing object is C, so `PerceptionObject::rep_type` := `ObjectStore::carried_type`; the
  §30.3 TRANSPORT_LANDING sums count the company's power while it lands.

### 35.4 Drop leftovers (a1i t05)

1. **Deployed footprint (0x396850 rule 2).** Deployed extents apply when the type `Deploys` (+0xd1), the object has a
   locomotor, and the locomotor says **deploying** (vf+0x130) **or deployed** (vf+0x120), or locomotor pack +0x2b3 is set
   [M: name]. Only `WalkLocomotorBehaviorClass` answers: vf+0x130 0x61df60 = state 0x18 (Deploying), vf+0x120 0x61df40 =
   state 0x19 (Deployed); every other locomotor returns 0 (0x13b7d0). Extents 0x371d20: x = `Custom_Hard_XExtent_Deployed`
   × `Scale_Factor` when > 0, else the normal x (0x371be0); **y = `Custom_Hard_YExtent_Deployed` × `Scale_Factor`
   whenever XExtent_Deployed > 0** (also when YExtent_Deployed ≤ 0), else the normal y. FoC sets no
   `Custom_Hard_XExtent_Deployed` (one type sets only `Custom_Hard_YExtent_Deployed`), so no FoC footprint changes.
   Port: deploying / deployed = the DEPLOY ability active (`AbilitySystem::deployed`) [M: the +0x2b3 byte];
   `drop_deployed_extents`.
2. **Persist flag.** `Create_Cinematic_Transport`'s `persist` is TransportDataPack +0xc9 (TACTICAL_GAPS §4.2.2 item 3).
   Port: `ObjectStore::landing_persist`, set by the Lua call; a human drop ignores every landing object except persistent
   ones.
3. **Campaign Reinforce refusals** (TACTICAL_GAPS §4.2.2 item 5): campaign only, silent, after the validity and population
   tests: the type has `Stay_In_Transport_During_Ground_Battle`; the unit is a transport whose contents hold such a type
   [port: the port's pool holds companies, not loaded transports: not reachable]; the battle planet restricts the type
   (`GalacticSim::planet_restricts`, 0x373490). The "battle decided" gate (mode +0x28 → +0x20) stays [M].
4. **Space members (0x2bf2d0 → `Find_Free_Placement` 0x2a9670(GOM, P, out, type, 2500, start 0°, exclude none)).**
   - Campaign fleet / squadron: each content object (0x3965f0) of the pool object in order; skirmish company: each
     `Company_Units` member (+0x978 / +0x980 [M]); the team helper then groups them. A single unit: at P directly.
   - Per member: the type's model for the mode (0x3729f0); none, or `Space_Layer` (+0x16dc) = 0x20 in space → P.
   - Box b = the model's local box (vf+0x58: centre c, half extents e; the type's prototype model, so no `Scale_Factor`
     [M]); in space z is widened by ±1000. Step = 1.2 × |the larger of the x / y spans| (2·e).
   - Rings r = 0, step, 2·step, … while r < 2500; per ring the angles 0°, 22.5°, …, **360° inclusive** (17 tests; r = 0 is
     tested once): Q = P + r·(cos a, sin a) (small-angle shortcut), query box = Q + b in GOM +0xd8 (all owners).
   - A tree object blocks when: not dead (+0x3a0 & 2), not `Is_Decoration` (+0x3b), type byte +0x3e clear [M: name], not
     the excluded object, no TRANSPORT_LANDING, no LAND_OBSTACLE, no PROJECTILE (slot +0x33a = behaviour 8),
     `Space_Layer` ≠ 0x20, has a render model, and its model's world box (0x264ab0) overlaps the query box in **x and y**
     (the z test compares the query z with itself: always true).
   - First Q without a blocker → the result (land: z = terrain height). None → (0, 0, 0), which the caller replaces by P
     (z := 0).
   - Members are created one at a time, so each later member sees the earlier ones as blockers.
   - Port (`find_free_placement`, `TacticalBattle::space_member_spots`): the member box = the catalog's space model bounds
     (`model_info(type, true)`); blockers = live, not dying store objects passing the filters above with a
     `Space_Model_Name`, box = `TacticalMode::bounds_of` in x / y; touching boxes do not block [M]. [port] When a member's
     model is named but not loaded (no model data, e.g. headless runs without art), the whole drop keeps the rows of 6.

## 36. The land Reinforce block waits for the landings; parked infantry in a giant vehicle's way (a1j t01 / t02) [H unless tagged]

### 36.1 `ReinforceBlockStatus` (Lua class id 0x81a, ctor 0x69fbb0) [H]

The land `Reinforce` (`LandTaskForceClass` vf, skirmish path 0x6ee010) returns this status; `QuickReinforce` loops
`while block and not BlockOnCommand(block) do block = Reinforce(target) end` (land: no `max_duration`; the space
version waits 3 s).

| Offset | Field |
|---|---|
| +0x40 | in-flight count (ctor 0) |
| +0x44 | saved goal-system-removable flag (ctor 1) |
| +0x45 | result byte (TF owner) |
| +0x48 | result list (non-TF owner) |
| +0x50 | player |
| +0x58 | reserved drop point (ctor sentinel 0x5d5e0b6b) |

- **Init 0x6a1120(status, owner, inFlight, result, player, pos):** stores the four values. When the owner is a task force,
  +0x44 := the plan's removable flag (plan +0x98, the flag `Set_As_Goal_System_Removable` writes, 0x5f7650) and then
  plan +0x98 := 0: **the plan cannot be removed by the goal system while its reinforcements are in the air.**
- **Skirmish land reinforce 0x6ee010:**
  - in-flight := the number of drop positions 0x6eaf60 returned (12-byte entries); result := 0x6eaf60's return (every
    pending unit placed and none trimmed);
  - the spawn loop runs from the last pending unit; the cap is read once (0x2acb10) and not decremented; a unit whose
    cost is over the cap reuses the position index;
  - each spawned landing (0x2b5fe0, §35.1) gets a listener for signal 0x19 on landing +0x38, then 0x6c1dc0(plan, C) and
    0x5f9fd0(tf, type, −1), which add type entries to the TF's +0x60 / +0x90 lists (C itself, each member type, or
    ceil(count / team size) copies of C) so the plan's force counts include the units in the air;
  - a refused landing still counts as in flight (the original status would never finish then) [H, by the code path];
  - 0x6eaf60's third parameter (max wait) is unused.
- **IsFinished 0x6a11d0:** finished iff +0x40 == 0. **There is no timeout.** On finishing, 0x6a1930 removes the reserved
  drop point from the global list DAT_140b3a2d0 and restores plan +0x98 := +0x44.
- **Result 0x6a1a50:** the +0x48 list (non-TF owner) or the byte +0x45.
- **Signal handler 0x6a1440 (signal 0x19, `ReinforcementsUnloadedDataClass`):** stop listening to that landing; in-flight
  −= 1; for a TF owner, each unloaded unit (a team member stands for its container; one already handled is skipped) is
  added to the TF (vf+0xa0, land 0x6e9240 `Add_Force`) and to the plan (0x6bf3d0(plan, obj)).

### 36.2 A landing destroyed while it unloads [H]

`TransportLandingBehaviorClass` (vftable 0x8b31f8) slot 15 = 0x6442d0, the detach that runs when the object is deleted
(MOVEMENT §3.1.1). When the pack's units-out count (+0xb0) is not 0 it calls the unload finish 0x6438a0 (unlock, signal
0x19 with the units already out) and removes the dust effect. With **0 units out it sends nothing**, so the original
status above waits for ever; the units not yet out are lost with the landing (§35.3 [M] is now [H] for the signal; the
members inside die with it).

### 36.3 Rule for the port (a1j t01)

- `BattleConfig::landing_unload_timed` is on by default (§35.3's timed mode); the untimed mode stays a [port] option for
  tests.
- `tf_reinforce` (land) keeps a `Reinforce` status per order while any landing it issued is in flight
  (`TacticalAiHost::reinforce_in_flight`); each `ReinforcementsUnloaded` record (the battle's 0x19 records) removes its
  landing, adds the unloaded units to the TF (free store → TF, `reinforce_joined`), and the status finishes when none is
  left. While it is open the plan is not removable (saved / restored as +0x44); the in-flight types count as TF entries
  for `units_lost`. Result = all placed. Saved (tactical AI save v8).
- [port] The in-flight count is the landings actually issued, not the drop positions: a refused landing does not hang
  the status.
- [port] A landing destroyed with **0 units out** posts an empty 0x19 record, so the status finishes instead of waiting
  for ever (no timeout is added; the original has none).
- [port] Unloaded units another task force already holds are skipped (the original's Setup A excludes locked units,
  §28.2; the port's delivery runs after the units unlock).

### 36.4 `Clear_Infantry` holds its infantry until every member arrives; a giant vehicle pushes parked friends aside (a1j t02)

**How the original releases the units [H].** `ClearInfantry.lua` calls `Set_As_Goal_System_Removable(false)` after
`QuickReinforce` and then blocks on `Attack_Move(AITarget, Get_Self_Threat_Max())`; its while loop re-evaluates
`Should_Clear_Infantry` only after that block returns. A non-removable plan is never taken apart by the goal system and
the free store gets its units back only when the plan's thread ends (`ScriptExit`). Members that arrive early get
`MainForce_Unit_Move_Finished` → a unit-level `Attack_Move(kill_target or tf)`; they stay in the task force. So the
infantry is held until the slowest member reports movement finished (signal 10, §33.2) — the port does the same. There
is no timeout or release path in the original for a member that never arrives.

**Why a member never arrived (port, foc-land seed 7 without the retreat plan).** p0's first `Clear_Infantry` task force
(AT-AT, M1s, Stormtrooper squads) blocked from frame 215 to the 14400 cap. The AT-AT (`Max_Speed` 0.5 × land 1.56 =
0.78 / frame, about 4500 frames for its 3300-unit path) stopped 170 units short of its slot: two parked friendly
`Squad_Stormtrooper` members stood 102-108 units from its goal, on its path. The port's MovingVsStopped resolution (the
early arrival needs the blocker within the radii + `LandDestinationProximity` of the goal **and** the mover on its final
approach) fell through to the dynamic path-around every collision check, the new path met the next parked trooper, and
the AT-AT circled for ever (about 100 dynamic re-paths in 1500 frames).

**What the original does there: the MovingVsStopped decision 0x684f90 [H for the branch, M for the distances].**
Mover m (MovingVsStopped participant 0), blocker b (participant 1, stopped); classes from type +0x169c.
- Before the class rules: m moving (locomotor vf+0x1b0), `cvar8` = 0x5c8900(m, b) (m is hostile to b, has the SQUASH
  behaviour 0x26 and b is squashable: the crush test of MOVEMENT §5.5) and the push-ability test 0x5c8890(b).
- **Giant rule:** m is a Giant Vehicle (9) **and** (b is Infantry / Bike (5 / 6) **or** m is moving) **and** m cannot
  crush b → call the push 0x6859a0; success → state 5 (Resolved), **m keeps its path**. Failure → the remaining rules
  (re-route / path around / try again later); a later step skips the second push attempt (`bVar5`).
- Push 0x6859a0(collision):
  1. b has no locomotor (object +0x333 == −1, the locomotor behaviour slot [M: name]): a death-clone obstacle (type
     +0xcf) is destroyed (0x391310) and the push succeeds; anything else fails.
  2. b's type float +0x23b0 ≤ 0 [M: name] and b is deploying or deployed (0x39a400 / 0x39a390, the locomotor deploy
     states of §35.4 item 1) → fails.
  3. Direction: m's heading (locomotor vf+0x1f8 → x / y, normalised) turned by **45°** toward b's side: −45° when
     (b − m) · (−h.y, h.x) ≤ 0, else +45° (0x2bab90).
  4. A free-position search (0x440be0 / 0x440f90) for b from b's position along that direction, cone π/4, factor 0.33
     [M: the search distance]; none → fails.
  5. A path request for b to the spot (0x67b730; Infantry / Giant classes set request flag 1; `MovementBoxExpansionFactor`
     when > 0); no path → fails. Otherwise b's locomotor takes the path as a **pushed** move (mode 4, or 7 when m could
     crush b; locomotor data +0x6d := 1; deploy-ish states 0x16 / 0x25 are reset by 0x559250). It is a locomotor move,
     not a unit order (no new order signal). Success.
- What a pushed unit does after the pusher has passed (`MinimumPushReturnDistance`, `RepushDistance`) is not traced
  (MOVEMENT §5.4) [L].

**Port (a1j t02).** `MovementSystem::resolve_blocker`: after the early-arrival test and before the path-around, a Giant
Vehicle mover pushes the stopped blocker (any class it does not crush; the port has only moving or stopped land
participants, so "m is moving" always holds) and keeps its path; the collision is Resolved. [port] The push: direction
as item 3 (heading = m's path direction to its next point); b walks aside over `ceil(d / speed)` frames at its own max
speed through the existing nudge (passability-checked each frame), d = the radii + 2 (the distance the port's fallback push
already used); no free-position search, no return move; a deploying / deployed b is not pushed (item 2 with +0x23b0
taken as ≤ 0); a b still stepping aside counts as pushed (the original pushes a blocker that moves again, decision step 2).
A b that cannot step at all (impassable first step) fails the push
and the old path-around runs.

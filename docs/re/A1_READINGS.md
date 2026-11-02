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
| 0x6bf3d0 (PP, unit) | admits the unit if needed; +0x30 = PP; adds the unit to PP+0x108 (into a null slot of the same type, else appended) | ReinforceBlockStatus code 0x6a1440 (a unit delivered for a plan's `Reinforce`) [M caller name] | a running plan receives a new unit |
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

### 28.9 Implemented (a1g t01, 2026-11-01)

Tactical (`TacticalFreeStore` entry `owner_pp`) and galactic (`unit_reserved`, the derived free store) follow 28.4-28.6
through the shared `GoalSystem` (new hook `suspend_reservations` at Commit step 4; kept goals call `refresh_plan`).
On_Unit_Service skips only reserved entries; the order-lock skip is gone. Port decisions:
- [M] The accept re-solve (0x6c4a50) draws its own synced numbers (Plan Setup C start team, IgnoreTarget contrast),
  since it runs the same solver. Replaying the first solve's draws was tried: it changed the land outcomes little.
- [port] The goal keeps Init's cost / ETA after the re-solve.
- Not modelled: the queue-slot flag 0 / 1 of 0x53bee0, the tactical credits reservation, 0x6bf3d0 (a Reinforce
  delivery joins PP+0x108 reserved), and the Setup A lock test (0x39ad40).
- `galactic_build.cpp` still reads `unit_reserved` when a build task takes a store unit (0x4d2bd0 does not read +0x30
  per 28.5) [open].
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
- **0x3973b0(team)** = FLEET (4) → fleet +0x70; else TRANSPORT (5) → transport +0x48; else TEAM → +0x50 (0x404c20);
  else, or when that is null, the object's own type (+0x298). For the example: `Stormtrooper_Team`.
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
  +0x48 [M: the carried type]), not the landing object's own power. Test: one landing object in (a) skirmish,
  (b) campaign with a resolvable parent (the parent's type's power), (c) campaign with an unresolvable parent
  (skipped).
- Implemented (a1g t03): `PerceptionWorld::skirmish_or_mp` (host: the battle is a skirmish; no MP), `parent_object`
  (store parent-mode id) and the `parent_rep_type` callback (host: the galactic object's type, -1 when not found [port:
  no TRANSPORT carried type in the port's galaxy]). Tests (a)-(c) in alamo_ai_tactical_perception_tests.

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
- **Drop validity (a1g t04 follow-up, 2026-11-01):** `TacticalAiHost::land_drop_ok(player, key, p)`, asked before each
  spot's Reinforce order; the battle host (`BattleAi::land_drop_ok`) lays out the slots its deploy would use (rows of 6 at
  `land_spacing` around p) and refuses the spot when a member's slot is impassable for its MovementClass (effective
  grid, static stamps included), fogged for the player (force 1), inside a non-allied Reinforcement_Prevention_Radius
  (2-D strict), or its footprint is taken (a live battle unit within the sum of the movement radii, 2-D strict).
  [port]: movement radii stand in for the TreeCull AABBs; the POWERED exemption of the prevention test is not modelled;
  the slot layout is the port's rows of 6 (the original company formation 0x5abc00 is [M]). Open: with that layout a
  large company (Rebel_Infantry_Squad, 3 rows) finds no valid spot around the Utapau Rebel point on seeds 1-6, while
  the tank brigades land.
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
| 0x45f5c0 story reward `INVADE_PLANET` | −1. It goes through the outgoing queue (0x3b01f0 on 0x140b2ed18), one event per land unit of the orbit objects that are friendly (0x39a2c0) to the plot's player (+0x228 → +0x638) [M: plot-player reading] | local player (stamped again at flush) |
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

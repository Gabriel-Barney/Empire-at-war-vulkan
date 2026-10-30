# A2 gal readings: income tick, galactic population capacity, steal order, blockade (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe (Ghidra 12.2 export in `re_reference/`). Prose and tables only; no code.
It settles the [M] / [L] guesses listed under "A2 gal" in `docs/PHASE4.md` (code blocks marked "A2 galactic" in
`engine/galactic` and `engine/ai`). It does not repeat GALACTIC_ECONOMY (GE), AI_GALACTIC_2 / _3 (AG2 / AG3),
STRATEGIC_GAPS (SG) or P4_SIM_SYSTEMS; read it with them.

Conventions: addresses are RVAs (image base 0x140000000). Confidence: [H] read directly, [M] inferred from strong
evidence, [L] guess. **probe** = needs an in-game run (Gabriel launches it; probes are prepared under
`tools/probe_mod/`). PD = planet data pack (planet object +0xB8). GOM = GameObjectManager (store).
GC = GameConstants (base 0x140b15800).

## 0. Status

| § | Item | Confidence | What the port should change |
|---|---|---|---|
| 1 | AI income tick: value, timing, gate | [H] | Pass the **raw** cycle income (before the AI `Credit_Multiplier` and before both clamps); fire **per player inside the payment loop**, only for non-human AI players with I ≠ 0; no tick at all on a zero-income cycle |
| 2 | Galactic population capacity 0x2ac700 / 0x3f8b40 | [H] | Starbase counted through the **planet** (its current starbase type, PD +0x140), once per owned planet; Base_Population_Capacity default 0 is right; the non-human skip is **"planet not revealed"** (0x3fa420), not "surface not accessible" |
| 3 | Steal order (HIJACK_UNIT step 2) | [H] | Walk the owner's object list **newest first** (reverse insertion; an owner change re-inserts at the newest end); match on the **representative** type; units in transit are always stealable |
| 4 | Blockade test 0x3fa370 | [H] | Only the **first** orbit entry that is not being removed and not hidden decides; blockaded = that entry is **not friendly (self / ally) to the planet's owner**; no combat-power filter |

---

## 1. Income: clamp order and when the AI hears about it [H]

### 1.1 The payment loop (0x4b2ef0, the fiscal-cycle block)

GE §1.2 describes the cycle. The exact AI feed inside it:

1. For each player in player-list index order, with I = Player_Income(player, consume = true) (0x4b16f0):
   1. If I = 0, nothing else happens for this player: no Add_Credits, **no AI tick**.
   2. Add_Credits(player, I, notify = false) (0x27f370). The AI multiplier, the zero floor and the cap clamp all
      happen in there, on Add_Credits' own copy of the amount.
   3. Then, still inside the loop and before the next player is paid, the AI budget accrual 0x614340 runs with
      **the same I**, the value returned by Player_Income. The accrual is called only if all of these hold:
      - the player has an AI system (player +0x360 ≠ null, 0x2814d0);
      - the player is not human (player +0x62 = 0);
      - the AI system has a context for the current mode (0x4b06d0) and that context's active byte (+0x69) is set.
      The budget is context +0x40, +400.
2. Only after the whole loop do these happen: last-cycle frame := next-cycle frame; next-cycle frame += the period;
   cycle counter += 1; the black-market reroll; the event 0x3d to the manager at 0x140b27f60; and, for every
   player in index order, notification 10 to mode +0x28 (0x346460).

So the value the AI sees is **before** the `Credit_Multiplier`, **before** the zero floor and **before** the cap
clamp. Add_Credits takes the amount by value and its adjusted amount is not passed back. The adjusted amount reaches
the AI only through Add_Credits' notify path (0x4b0310), and the cycle calls Add_Credits with notify off.

### 1.2 Inside the accrual 0x614340

The accrual's own gate: it does nothing unless the budget owner's +0x34 is 0 [L: meaning not traced], a galactic
mode exists, and T = 0x4b2010 > 0. 0x4b2010 returns the global 0x140b15980 = GC +0x180 = `Fiscal_Cycle_Time_In_Secs`
(the same constant the cycle uses for its period). The drift step reads the player's credits (player +0x70) at call
time, i.e. after this player's Add_Credits, cap included. The rest is AG2 §3.1.

### 1.3 Port check

| Port (campaign.cpp `fiscal_cycle`, ai_hooks.cpp, ai galactic_ai.cpp `on_income`) | Original | Effect |
|---|---|---|
| gain = I × `Credit_Multiplier` for AI players | gain = I (raw) | **Wrong** on Easy (0.5) and Hard (1.2): the AI's offensive pools accrue at the wrong rate. Normal (1.0) is unaffected |
| hooks fire after the whole cycle, after the black-market reroll and the `GalacticDayPassed` event | fired per player inside the loop, before the reroll and before the day notifications | The budget arithmetic is the same: only player i's own credits are read, and later players' payments don't change them [M]. **Exception:** a synchronous listener of the day event that changes credits (for example a story reward) would make the port's drift read different credits |
| a tick for every player, including gain 0 and humans; the AI side filters humans but not gain 0 | no tick when I = 0 | **Wrong**: on a zero-income cycle the original runs no accrual at all (no drift redistribution, no reset-flag pass, h does not count down) |
| `tick.day` = counter after the increment | the counter is incremented after the accrual | Only matters if something reads it (the budget does not) |
| T = `fiscal_cycle_seconds` | T = GC +0x180 | Right |

**Add_Credits clamp condition** (refines GE §1.1 step 4) [H]: with cap ≥ 0 the clamp is applied unless the global
flag 0x140b155c5 is set **and** either there is no current mode or the mode's vf +0xF8 returns false. The meaning of
the flag is still [L]. The port always clamps when cap ≥ 0, which matches whenever the flag is clear.

---

## 2. Galactic population capacity (0x2ac700) [H]

### 2.1 0x3f8b40

0x3f8b40(planet behaviour, planet object) returns PD +0x140. That field is the planet's **current starbase type**: it
is written only by 0x4bda40, which stores the type at +0x140 and its level (type +0x8A0, clamped to 5) at +0x148, and
clears both to 0 (and +0x138 to −1). 0x4bda40 is called from Set_Starbase 0x3fe750: from production, sabotage
(0x5b3b60 sets the `+0x8B0` "previous level" type, or clears it at level 1), Change_Owner and battle results. In galactic
mode **the starbase is not a store object**: it is a type pointer on the planet.

### 2.2 The capacity sum

Capacity(player), with the pop manager's current mode:

1. **Gate.** The result is 0 if there is no mode, no such player, or the mode is galactic (vf +0xE0 = 0; 1 / 2 =
   land / space) while the player is not human and GC `AI_Uses_Galactic_Pop_Cap` (global 0x140b276bc = GC +0x11EBC)
   is clear.
2. **Base:** faction `Base_Population_Capacity` (faction +0x2EE8, int). The faction constructor (0x33a580) zeroes it
   with a qword write over +0x2EE4..+0x2EEB, and no shipped XML sets it. **Default 0** [H] (the [L] is lifted).
3. **The player's own objects.** Walk the GOM per-owner list (GOM +0xC8 + player × 0x48; newest first, §3.1).
   Skip objects with +0x3A0 & 0x42 set. For each remaining object:
   - **Reveal skip (non-human players only).** Find the nearest planet among the object and its parents (+0x2B0
     chain; "is a planet" = behaviour-3 slot byte +0x335 ≠ −1). If the player is not human and that planet fails
     `Is_Planet_Revealed` 0x3fa420, skip the object. An object with no planet ancestor, such as a fleet in transit, is
     never skipped.
   - Add the object's own type `Additional_Population_Capacity` (type +0x2120, int).
   - If the object **is** a planet and its starbase type (0x3f8b40) is non-null, also add that starbase type's
     `Additional_Population_Capacity`.
4. **Skirmish only** (GMM 0x28afb0): for every other player in index order that is allied (0x2823c0), add the
   +0x2120 of each of its objects (+0x3A0 & 0x42 clear). Allies get no planet / starbase term and no reveal skip.
   This does not apply in campaigns.

### 2.3 What this means

- **The starbase counts exactly once per planet in the owner's list.** It comes with the planet, whoever holds the
  starbase (PD +0x13C is not read), and whatever the starbase level (a type set with a negative level still counts).
  There is no separate starbase object to count.
- **Contributors in shipped FoC data:** planets (PLANETS.XML, mostly 5) and starbase types (STARBASES.XML /
  STARBASES_UNDERWORLD.XML, 10–23 across levels). The MARKERS / PROPS_STORY entries (reinforcement-point markers,
  100) are tactical objects and never sit in a galactic owner list. No unit or ground-structure type in FoC sets the
  tag, but mods may, and any owned object with the tag counts.
- **0x3fa420 is `Is_Planet_Revealed`** (GALACTIC_REVEAL_CORRUPTION §1.1). It returns true in multiplayer; false
  without a PD or with the hidden byte PD +0x2E0 set; true with the galactic auto-reveal option; otherwise the
  revealed byte PD +0x2C9. It takes **no player argument**, so the non-human skip uses the *local human's* reveal
  state. AG3 §3.5's "surface not usable" reading is wrong.
- The used side 0x2ac320 uses the same reveal skip. It sums pop(representative type, mode 0) (0x3973b0 / 0x373500)
  and skips an object whose parent's representative type has pop > 0. With the flag, it adds the queue pop (0x401cf0)
  of objects that have a production behaviour. It then compares pop(T) ≤ max(capacity − (int)(sum + 0.5), 0). It does
  **not** test +0x3A0 & 0x42. Because galactic starbases are not objects, a starbase's `Population_Value` is never in
  "used" [H from §2.1; the port counts it if non-zero].

### 2.4 Port check (ai_hooks.cpp `population_capacity` / `population_used`)

| Port | Original | Verdict |
|---|---|---|
| base = faction `Base_Population_Capacity`, default 0, parsed as float and truncated | int, default 0 | Right (the int parse differs only for non-integer XML) |
| planet type capacity for every owned planet | same (the planet object is in the owner's list) | Right |
| starbase capacity from the starbase **object** when `store.owner(starbase) == player` | starbase **type** of every owned planet, starbase owner ignored | Differs when the starbase owner ≠ the planet owner, or when the port has no starbase object but PD +0x140 would be set |
| non-human skip = planet type not `surface_accessible` | skip = planet not revealed (local reveal state; always revealed in MP) | **Wrong predicate**. It only matters with `AI_Uses_Galactic_Pop_Cap` (off in shipped data) |
| objects in transit: counted via `fl.planet` with the skip applied | no planet ancestor → never skipped | Differs for in-transit fleets under the flag |

---

## 3. Steal order (HIJACK_UNIT 0x648370 step 2) [H]

### 3.1 The list and its order

The steal walks `GOM(mode 0)` per-owner list 0x2acb70(store, TF owner id) = GOM +0xC8 + owner × 0x48. It starts at
the list's tail link (record +0x10) and follows the *previous* links (+0x08) back to the sentinel (record +0x08).

The insert 0x20a9b0 always links a new entry **before the sentinel**, i.e. at the tail. Its callers are:
- Create_Object 0x29f810;
- `GOM::Register_Object` 0x2b5ad0 (save load);
- the owner change 0x2b3ac0, which unlinks the object from the old owner's list and inserts it into the new one.

So the walk is **newest first**:
- objects most recently created for, or captured by, the player come first;
- an object that changed owner counts as new to its new owner;
- after a load, the order is the load registration order [M: assumed to be the saved order].

The capacity / used walks of §2 use the same direction, but their sums do not depend on it.

### 3.2 The match (only the first match is killed, Kill 0x391310 flag 1)

The step runs if the plan's `MagicPlanStealing` is set or T is a named hero (type +0x13). Walking the list, an object
O matches when **0x3973b0(O) == T**. 0x3973b0 returns the representative type:
- a fleet: its fleet data +0x70, set only for fighter-squadron "fleet of one" containers (0x400650) to the squadron
  type; ordinary fleets fall back to `Galactic_Fleet`;
- a transport: its company type (transport data +0x48);
- a team: the team's type;
- else: O's own type.

O must also be outside the destination D. With P = O's parent (+0x2B0):
- P null → match;
- else, if P has a fleet behaviour, P := P's parent; if that is null (fleet in hyperspace) → match;
- else if P has no planet behaviour → match;
- else match if P ≠ D, **or** T is a named hero (heroes are taken from D as well).

The walk has no alive / removing test and no stealth test.

### 3.3 Port check (ai_hooks.cpp `steal_unit`)

| Port | Original |
|---|---|
| iterates `store_->all_objects()` (oldest first), filtered by owner | owner list **newest first**; capture re-inserts at the newest end |
| matches the store type; skips fleet and planet objects | matches the representative type: a fighter-squadron container fleet *can* match (its squadron type); the port's company object (store type = company type) matches like the original transport |
| destination test via `location_of(h)` | a unit in a fleet in hyperspace has no planet → always stealable; the port uses the fleet's `planet` field |

With shipped data (all 8 magic plans `MagicPlanStealing = false`) only **named heroes** reach this step, and a player
normally owns one instance of each hero. So the order matters only for mods, or for duplicate hero instances
[H data, M impact].

---

## 4. Blockade test 0x3fa370(planet behaviour, planet object) [H]

### 4.1 Definition

Let X = the planet's owner, looked up from the planet object's owner +0x58. Walk the PD orbit list (+0xC8, count
+0xD0) in list order:

1. A null entry → **blockaded** (defensive; not expected).
2. Skip the entry if it is being removed (+0x3A0 bit 0x2) or hidden. The stealth predicate 0x3a9760 (SG §2.2) is
   true:
   - for a fleet: only if it has at least one content and every content is hidden. An **empty fleet is visible**;
   - for a transport: if all its cargo is hidden;
   - otherwise: the stealth byte.
3. **The first entry that is not skipped decides:** blockaded ⇔ not 0x39a2c0(entry, X, 1), i.e. the entry's owner is
   **not self-or-ally** of X (relation table X +0x370[owner] ≠ 0). An owner id with no player object also counts as
   not friendly.
4. No entry left → not blockaded.

Details that follow from this:
- **There is no combat-capable filter.** An unarmed transport fleet, a lone hero or an empty fleet blockades as
  long as it is visible and first.
- **Only the first visible entry counts.** A friendly fleet listed before a hostile one lifts the blockade.
- **Neutral counts as a blockader:** anything that is not self or ally blockades.
- It is relative to the **planet owner**, not to the player who asks. All callers are AI paths: HIJACK_UNIT launch
  (0x648370), the build task (0x648120, FSM 0x64aec0), FreeStore `Move_Object` (0x68f5b0) and TaskForce
  `Launch_Units` (0x6cf070). An AI with units on a planet it does not own is therefore "blockaded" by its own fleet
  when that fleet is the first visible orbit entry [H read, M gameplay reach].

### 4.2 Port check (campaign.hpp `blockaded` → `enemy_fleet_in_orbit`)

| Port | Original |
|---|---|
| any orbit fleet that is `Relation::Enemy` to the **asking** player | the **first** visible orbit entry, not self / ally to the **planet owner** |
| needs a member with `AI_Combat_Power` > 0 | no combat filter |
| no visibility test | hidden (0x3a9760) and removing entries are skipped |
| Neutral is not a blockader | Neutral is a blockader |

`enemy_fleet_in_orbit` is also the port's starbase-in-production cancel (GE §2.5 item 2). That test is a different
one: any visible, non-friendly fleet with a combat-capable member. It should stay as it is. The blockade needs its own
function.

---

## Port-affecting findings

1. **Income tick value** (campaign.cpp `fiscal_cycle`): `last_income_[i]` / `IncomeTick::gain` must be the raw
   Player_Income I, with no `Credit_Multiplier`. The "after multiplier" note in AG2 §3.1 and in the port comments is
   wrong [H].
2. **Income tick gate:** fire only when I ≠ 0, for a non-human AI player with an active galactic AI context. On a
   zero-income cycle the AI budget gets no accrual call (galactic_ai.cpp `on_income` must not run
   `budget.tick(0, …)`) [H].
3. **Income tick timing:** fire per player right after that player's add_credits, inside the payment loop, before the
   black-market reroll, the counter increment and `GalacticDayPassed`. Equivalent for the budget today [M]; required
   once anything in the day event can change credits [H for the order].
4. **Population capacity starbase term:** add the starbase **type's** `Additional_Population_Capacity` for each
   planet the player owns, from the planet's current starbase type, whoever owns the starbase. Do not add it through
   the starbase object. Drop the starbase object's `Population_Value` from `population_used` [H].
5. **Population non-human skip:** replace the `surface_accessible` test with "the object's nearest planet is not
   revealed". The test uses the local human's reveal state, is always true in multiplayer, and is never applied to
   objects without a planet (in transit). It applies to both capacity and used [H]. It only matters with
   `AI_Uses_Galactic_Pop_Cap`.
6. **Base_Population_Capacity:** default 0 is confirmed. Drop the [L] tag [H].
7. **steal_unit order:** iterate the player's objects **newest first** by "joined this owner" order. Owner changes
   must move the object to the newest end. Keep that order across the port's save / load [H].
8. **steal_unit match:** use the representative type (company, squadron container); treat units in hyperspace as
   never at the destination [H]. Shipped data reaches this only for named heroes.
9. **blockaded():** implement 0x3fa370 as written in §4.1:
   - first non-removing, non-hidden orbit entry;
   - "not self / ally of the planet owner";
   - no combat filter.
   
   Keep `enemy_fleet_in_orbit` for the starbase cancel [H].
10. **Add_Credits clamp:** the cap is skipped only when the flag 0x140b155c5 is set and the mode's vf +0xF8 is
    false (or there is no mode). The port's unconditional clamp matches while the flag is clear [H read, L flag
    meaning].

Probes (optional; the readings above are [H] without them):
- **(a) Blockade:** at a planet the AI owns, put an allied fleet into orbit before a hostile one and check that the
  AI still launches.
- **(b) Steal order:** with a mod giving two instances of one hero type, check that HIJACK takes the newest one.

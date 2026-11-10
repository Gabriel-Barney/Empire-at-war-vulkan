# Hero clashes, build-pad occupancy and the vehicle-thief flags (retail behaviour)

Roadmap s78n t06 (hero clashes) and t05 (build-pad occupancy, vehicle-thief takeover flag).
Behaviour recovered from the retail `StarWarsG.exe` (FoC, Steam build) and the shipped XML. Written for a
port agent who must not read the decompile: everything a port needs is stated in behaviour terms here, with
retail addresses only as references for later RE work.

Confidence tags: **[H]** read directly from code (or disassembly) and cross-checked; **[M]** read from code
but one link not fully followed, or derived (timings); **[L]** a guess from naming or usage.
`[port]` marks a choice the port makes where retail behaviour is not observable or not worth copying.

Related docs (extended here, not duplicated):
- `TACTICAL_COMBAT.md` §2.3 (target candidate filters), §3.5 (convert projectiles), §3.6 (Take_Damage),
  §5.4.1-§5.4.3 (Bribe / Hack accepts, +0x150 build-pad pack, +0x3a0 / +0x3a2 bits, pairing rule 0x39a540).
  One correction to §5.4.3 item 1 is given in §7.1 below.
- `A1_READINGS.md` §33.2.1 (valid-target tests), lock table (Lock / Unlock_Current_Orders), AI free store.
- `MOVEMENT.md` §4.7.x (coordinators, stop-and-hold), MoveActionClass of the action-driven clash.
- `ANIM_PARTICLES.md` A.3-A.5, A.8 (Play_Animation, slot modes, frame advance), §107 (HC_WIN / HC_LOSE / HC_DRAW
  anim types 53-55).
- `xml_field_tables.md` (Hero_Clash field offsets).

Object field names used below (retail offsets, for cross-reference with the other docs): +0x50 object id,
+0x58 owner, +0x5c current hull health (float), +0x78 position, +0x84 orientation (+0x8c yaw, degrees),
+0x298 type, +0x2b0 parent container, +0x348 TEAM slot (the object is a team container),
+0x3a0 / +0x3a1 / +0x3a2 flag bytes, +0x3b4 "in hero clash" byte, +0x150 build-pad data pack.
Behaviour slots: 1 locomotor, 7 IDLE, 9 TARGETING, 10 WEAPON, 11 TURRET; behaviour vf+0xc0 = suspend,
vf+0xb8 = resume. "Synced RNG" = the game-logic LCG at 0x140a13e24 (`s = s*0x41C64E6D + 0xBDF`); float draw
0x532480(lo, hi) = `((s >> 10) & 0x7fff) / 32767 * (hi - lo) + lo`; int draw 0x1ffb40(lo, hi) (no draw when
lo == hi).

---

## 1. HEROCLASH.XML

### 1.1 Schema

Root `<Hero_Clashes>`, one `<Hero_Clash Name="...">` per entry. Loaded into a type object of 0x130 bytes; field
offsets in `xml_field_tables.md`. Meaning:

| Tag | Meaning |
|---|---|
| `Clash_Type` | `Melee_Duel` (0) or `Action_Driven` (1). Default -1: an entry with no / unknown type never starts a clash [H]. |
| `Clash_Range` | Start distance: the two heroes start a clash when their 3D distance is <= this. |
| `Involved_Hero_Types` | List of types; used by the action-driven clash and the "is this type in a clash entry" lookups. Detection itself uses `First_Hero_Type` / `Second_Hero_Type` [H]. |
| `Play_Conversation_Events` | Yes/No: melee exchanges may include a "conversation" exchange (§3.6). |
| `Combat_Distance` | Melee: distance the two heroes stand apart while fighting. |
| `Damage_Amount`, `Damage_Percentage` | Melee damage per won exchange (§3.7). |
| `First_Hero_Win_Exchange_Chance`, `Second_Hero_Win_Exchange_Chance` | Per-exchange win weights; draw weight = 1 - first - second. If first + second > 1 the loader reports an error and sets both to 0.3 [H]. |
| `First_Hero_Damage_Multiplier`, `Second_Hero_Damage_Multiplier` | Multiplier applied when that hero is the exchange winner. |
| `First_Hero_Type`, `Second_Hero_Type` | The two hero object types (resolved to type pointers at load). |
| `First_Hero_Win_Anim` / `_Lose_Anim` / `_Draw_Anim` / `_Conversation_Anim`, same for `Second_` | Animation types played per exchange outcome. |
| `First_Hero_Win_Speech`, `Second_Hero_Win_Speech` | Speech event played by the clash winner at the end (audio only). |
| `Clash_Actions` | Action-driven only: list of `<Attack_Action>` / `<Move_Action>` / `<Special_Ability_Action>` blocks with relevance curves, ability names and cooldowns. |

The loader 0x4814d0 appends every entry to one global list in file order and does **not** check for duplicate
names [H] (the shipped file has two entries named `ObiWan_BobaFett`; both are loaded).

### 1.2 Shipped entries (FoC and EaW files are identical)

| # | Name | Type | Heroes (First / Second) | Clash_Range | Notes |
|---|---|---|---|---|---|
| 1 | `Vader_ObiWan` | Melee_Duel | Darth_Vader / Obi_Wan_Kenobi | 100.0 | Combat_Distance 17.65, Damage_Amount 0.0, Damage_Percentage 0.15, multipliers 2.5 / 1.0, win chances 0.40 / 0.30 (draw 0.30), anims HC_WIN / HC_LOSE / HC_DRAW, conversation anim IDLE, Play_Conversation_Events No, win speeches `Hero_Clash_Obiwan_Vader__Vader_Win` / `__Obiwan_Win`. |
| 2 | `ObiWan_BobaFett` | Action_Driven | Obi_Wan_Kenobi / Boba_Fett | 0.0 | Actions: lightsaber attack, Force run, move to Boba; Boba jetpack jump (Move_Past_Target, Move_Use_Jetpack), flamethrower (ability Boba_Fett_Flame_Thrower, cooldown 1.5), blaster, burst fire. |
| 3 | `ObiWan_BobaFett` (duplicate name) | Action_Driven | Boba_Fett / Han_Solo (Involved also Chewbacca) | 0.0 | XML comment: used to be the vehicle-thief team, "broken now", disabled. |

Consequence [H code, M practice]: entries 2 and 3 have `Clash_Range 0.0`, so they need two exactly coincident 3D
positions to start (distance² <= 0). That does not happen in normal play; **only `Vader_ObiWan` (melee) can
start in the shipped game.**

Unit data relevant to entry 1: Darth_Vader and Obi_Wan_Kenobi are `Is_Named_Hero Yes`, `Tactical_Health 500`,
no shields; both are single objects inside a hero company (`Darth_Team`, `Obi_Wan_Team`). Their HC_WIN /
HC_LOSE / HC_DRAW clips (`EI_VADER_HC_*_00`, `RI_OBIWAN_HC_*_00` in models.meg) each have one variant (`_00`)
and 101 frames at 30 fps [H].

---

## 2. HeroClashManager (mode vf+0x210)

### 2.1 Where it lives and when it runs

- Owned by the land mode (mode +0x3f8). Mode vf+0x210 returns it in land mode (0x3b7f30) and 0 in space and
  galactic (0x17d070) [H]. **No clashes in space or galactic.**
- Ticked from the land mode tick 0x3bb440, step 4: after weather (0x46a0c0), before the mode frame counter
  increments and before the common object tick 0x3639d0 (object services) [H]. Call: 0x56ce20(manager, global
  frame).
- Manager (vtable 0x89a3b0): doubly linked list of running clashes at +8 (append at end), count at +0x10.
  Slot 0 0x1fa90 "clash started" (no-op), slot 1 0x56cda0 remove + delete a clash, slot 2 dtor 0x56c3c0 [H].
- Update 0x56ce20: (1) detection 0x56c790; (2) for each running clash in list order call its update
  (vf2) with the frame; the next pointer is fetched before the call, so a clash may remove itself; null
  entries are removed [H].

### 2.2 Detection 0x56c790

Runs only when `frame % 15 == 0` (global frame passed to the land tick) [H]. No RNG draws [H].

For each clash entry in load order:
1. `A` = candidates of `First_Hero_Type`, `B` = candidates of `Second_Hero_Type` (0x56c540). If `A` is empty,
   next entry.
2. Candidate list: every game object whose type is exactly that type, in game-object-manager list order
   (0x2a87d0) [H], filtered (0x56c540):
   - object **not** a team container: passes if all of
     - not using an active jet-pack locomotor (0x39ae20: locomotor pack +0xa8 has +0x28d set) [M on meaning],
     - +0x3a0 bits 0x40 (dead/dying), 0x80 (death clone), 0x02 (removed) all clear,
     - +0x3b4 != 1 (not already in a clash),
     - current attack target (0x397a40(obj, 0): the container's formation attack target, else the targeting
       data's target) is null **or** is of the other hero type of this entry;
   - object that **is** a team container: every member must pass the same tests; then the container itself
     is pushed.
3. Pairing: for each `a` in `A`, for each `b` in `B`:
   - if 3D distance² (a, b) <= `Clash_Range`²:
     - build the involved list = `a` (or `a`'s team members) + `b` (or `b`'s members); if any is null or
       has +0x3b4 == 1 → skip this pair;
     - create the clash by `Clash_Type`: 0 → MeleeHeroClash (0xc0 bytes, ctor 0x6ab920), 1 → ActionDriven
       (0x50 bytes, ctor 0x6aacc0), other → nothing;
     - call its start (vf1) with the list. Success → append to the manager list, erase `b` from `B`, go to the
       next `a`. Failure → delete the clash object, continue with the next `b`.

Shipped data lists the hero objects (`Darth_Vader`, `Obi_Wan_Kenobi`) as First / Second types, not their
hero-company containers, so the candidates are the hero objects and the team-container branch is unused in
vanilla [H for the types, M that no container ever has these types].

### 2.3 Removal hook

When an object leaves the game-object manager (Remove_Object 0x2a6a00; flushed at the end of the GOM service),
the mode's vf+0x258 (land 0x3b7a70 → 0x56c4c0) runs: if the object's +0x3b4 is set, find the first running clash
whose `involves(obj)` is true and call its object-removed handler (vf5) [H].

---

## 3. Melee clash (MeleeHeroClashClass, vtable 0x8bd9f0)

Vtable: 0 dtor 0x6ab9b0, 1 start 0x6ac780, 2 update 0x6acc30, 3 involves 0x6ac920 (obj is hero1 or hero2),
4 winner 0x6ac770 (valid in state 4), 5 object removed 0x6ac940, 6 heroes list 0x6ac6f0 [H].

State kept per clash: hero1, hero2 (+0x18 / +0x20), their types, **health of each at clash start** (+0x38 /
+0x3c, copied from +0x5c), state (+0x40), winner (+0x48), damage-applied flag (+0x50), exchange winner / loser
(+0x58 / +0x60), spot1 / spot2 (+0x68 / +0x74), snap start positions and orientations, snap start / end frame
(+0xb0 / +0xb4), current outcome (+0xb8), previous outcome (+0xbc, starts 0).

States: 1 approach, 2 snap, 3 fighting, 4 ended.

### 3.1 Start 0x6ac780 (sim)

From the involved list: hero1 = the object of `First_Hero_Type`, hero2 = the object of `Second_Hero_Type`.
Fails (returns 0, clash deleted) unless [H]:
- both found, both types are heroes (0x39a600: type `Is_Named_Hero` or `Is_Generic_Hero`),
- the owners differ and are **enemies** (player(hero1.owner) relation table [hero2.owner] == 1, 0x2824d0).

On success, for each hero: suspend TARGETING (slot 9), set +0x3b4 = 1 (**on the hero objects only, not on
their team containers**) [H]. Store heroes, types, start healths; then Begin (§3.2); call manager slot 0
(no-op); return 1.

### 3.2 Begin 0x6abde0 (sim)

- state = 1.
- mid = average of the two positions; D = `Combat_Distance`.
- Spots on the world X axis through mid [H]: if `mid.x <= hero1.x`: spot1 = (mid.x + D/2, mid.y, mid.z),
  spot2 = (mid.x - D/2, mid.y, mid.z); else spot1 = mid.x - D/2, spot2 = mid.x + D/2.
- For each hero that the coordinator system accepts (0x425e30): leave its coordinators (0x426d50), create a new
  coordinator (0x426010; if that returns null, Begin returns early **without locking**), add the hero, and
  issue the coordinator Move_To (0x4f8200) to the hero's spot; the spot is stored only on this path.
- Lock movement orders on both heroes (lock counter 0x3a8e50(h, 1)) [H]. While locked, a coordinator Move_To
  that includes the hero drops the order, and removing the hero from its unit is refused (see
  `A1_READINGS.md` lock table) — this is the "orders locked" effect of a clash.

### 3.3 Update 0x6acc30, state 1 (approach)

Per frame (the manager tick, every frame):
- If either hero's locomotor is still walking (locomotor vf+0x1c0(0): the walk locomotor reports "moving" when
  its path stack is non-empty) → wait.
- Else if `|dist²(hero1, hero2) - D²| > 0.5` (3D) → start the snap (§3.4).
- Else → fight (§3.5).

[M] Whether the path stack is already non-empty on the start frame: the new coordinator builds its formation
and path at its next coordinator update (`MOVEMENT.md` §4.7.4). If the first approach check runs before that,
the heroes are "not walking" and the snap starts on the start frame itself, i.e. they **slide** to their spots
rather than walking. Not settled without a probe (§9).

### 3.4 Snap 0x6aba30 / update state 2 0x6acfb0 (sim: positions change)

- state = 2; record both heroes' current position and orientation as snap start.
- errPos = max over heroes of the 3D distance hero → its spot, clamped to [0, 3].
- desired yaw for each hero = yaw of the vector own spot → other spot (0x20acd0: yaw = atan2 in degrees,
  [0, 360); pitch = -atan2(dz, dxy)).
- errAng = max over heroes of `|yaw - desiredYaw|`, and if that is > 180 retail **subtracts 180** (not
  `360 - x`; retail quirk [H]); clamped to [0, 180].
- duration t = max(0.1 + 0.4·errPos/3, 0.1 + 0.9·errAng/180) seconds; end frame = start + (int)(30·t).
- Each frame f in [start, end]: s = (f - start)/(end - start) clamped to [0, 1]; set position = lerp(start pos,
  spot, s) (0x3a8f90) and orientation = per-component angle lerp (0x63ea50 / 0x63efd0, shortest path [M])
  towards the desired orientation (0x3a8710). At f == end → fight (§3.5).

The snap is a direct teleport-lerp, not locomotion: no pathing, no collision [H].

### 3.5 Fight 0x6abcd0

state = 3; each hero is turned (snap) to face the other's current position; IDLE (slot 7) and locomotor
(slot 1) are suspended on both [H]. TARGETING has been suspended since start. The heroes stand still.

### 3.6 Exchanges (update state 3, 0x6ad230)

Per frame:
1. If current outcome is 4 (conversation): wait until the speech manager is idle (0x3223f0), then start a new
   exchange.
2. Else if hero1's primary animation type is not one of hero1's Win / Lose / Draw anims (the first frame of
   the fight, or something replaced it) → start a new exchange now.
3. Else if hero1's anim slot mode (0x395660) is 2 (finished): if hero2's is not also 2 → return; else (both
   finished) → apply damage if not yet applied (4.), then start a new exchange.
4. Else progress = hero1's current anim frame / clip frame count, clamped [0, 1]; if < 0.8 → return.
   Otherwise apply the damage once per exchange (+0x50 flag):
   - if there is an exchange loser: `dmg = (int)round_half_away((loserStartHealth * Damage_Percentage +
     Damage_Amount) * mult)`, with `mult` = `First_Hero_Damage_Multiplier` if the winner's type is
     `First_Hero_Type`, `Second_...` if `Second_Hero_Type`, else 0 (0x480ed0) [H];
   - Take_Damage (0x3a9e30; arguments per `TACTICAL_COMBAT.md` §3.6): victim = loser, kind 1, amount =
     (float)dmg, source = winner, no hardpoint, no out pointers, no-team flag 0, raw flag 0, param 10 = -1
     (disassembly 0x6ad39c-0x6ad3e9) [H];
   - then if the loser is dead (+0x3a0 & 0x40) → end the clash with the winner (§3.8).

**Start a new exchange** (0x6ac1b0) [H]:
- clear the damage flag and exchange winner / loser.
- Outcome:
  - if the previous outcome was not 4, the speech manager allows it (0x3223c0) and `Play_Conversation_Events`
    is Yes → outcome 4 (conversation: both play their Conversation anim, no winner, and the conversation
    speech plays). No draw.
  - else: weights {1: First win chance, 2: Second win chance, 3: 1 - first - second}; **remove the previous
    outcome's key** (so the same outcome never happens twice in a row; on the first exchange the previous
    outcome is 0, nothing removed); total = sum of remaining weights; **one** synced float draw
    `r = Get_Float(0, total)`; walk keys ascending, `r -= w`, first key with `r <= 0` wins; fallback 3.
    Previous outcome := chosen.
- Outcome 1: hero1 plays First Win, hero2 plays Second Lose; winner hero1, loser hero2.
  Outcome 2: hero1 First Lose, hero2 Second Win; winner hero2, loser hero1.
  Outcome 3: both Draw anims; no winner.
- Variant: n1 = variants of hero1's anim on hero1's model, n2 likewise for hero2. If n1 == n2: v = synced
  `Get_Int(0, n-1)` if n >= 2, else 0 (no draw). If both clips exist, have equal frame counts and equal
  vf+0x38 value [L: clip fps or duration], both heroes play (Play_Animation 0x3a92f0) type, variant v, no loop,
  blend 0.1, speed 1.0, start frame 0, mode 0 (one-shot), no lock. If the variant counts or clips differ,
  **nothing is played** [H]; the next frame then finds hero1's anim is not a clash anim and draws again [M].
- current outcome := chosen.

Shipped numbers (entry 1) [M derived]: one variant, 101 frames @30 fps → an exchange lasts about 100 ticks
(one-shot finishes at frame >= count - 2, `ANIM_PARTICLES.md` A.5), damage lands at progress 0.8 ≈ tick 81-82
of the exchange. Vader wins an exchange → Obi-Wan takes `round(500 * 0.15 * 2.5) = 188` (at full start
health); Obi-Wan wins → Vader takes `round(500 * 0.15 * 1.0) = 75`. Damage is based on health **at clash
start**, not max health (the XML comment says max health; code uses +0x5c at start) [H].

### 3.7 Damage path details

Take_Damage runs its normal steps (`TACTICAL_COMBAT.md` §3.6) [H for the step selection, M for armour /
shield steps in detail]: the victim's TAKE_DAMAGE multipliers (kind 7) apply; the winner's CAUSE_DAMAGE does
**not** apply and there is no diminishing firepower, because the source is a non-projectile object without
+0x3a1 & 0x10. Base-shield factors etc. apply as for any damage.

### 3.8 End and release

Release one hero (0x6acb80(hero, state)) [H]:
- state 1 (approach): unlock movement (0x3a8e50(h, 0)), resume TARGETING, +0x3b4 = 0.
- state 3 or 4: first resume IDLE and locomotor, then as state 1.
- **state 2 (snap): does nothing** — retail quirk: a hero whose opponent is removed during the snap stays
  locked, targeting suspended, +0x3b4 = 1, for the rest of the battle [H].

End 0x6ac660(winner): release both heroes (per current state), winner := given, state = 4, the winner plays its
win speech (audio), the manager removes and deletes the clash.

What ends a clash [H]:
- the exchange loser is dead after the exchange damage (winner = exchange winner);
- a hero leaves the game-object manager (death by anything else, removal, despawn) → object-removed handler
  0x6ac940: hero1 removed → hero1 cleared, end with winner hero2; hero2 removed → hero2 cleared, hero1 released
  per state, end with winner hero1.

There is **no timeout and no separation check** [H]. Nothing else (Lua, orders) ends a clash. Lua
`Unlock_Current_Orders` on a clashing object reports "locked by a hero clash. Cannot unlock." and does nothing
(`A1_READINGS.md`).

### 3.9 What still reaches clashing heroes

- Area damage (0x4c7f70) has no +0x3b4 test [H]: splash damage hits duellists.
- Direct fire and abilities are filtered by the pairing rule (§5.2): only the clash partner may target them.
- Third-party ability users obey the same rule, so e.g. a Force-lightning user outside the clash cannot target
  a duellist [H].

### 3.10 Visual / audio only (port may skip for sim)

Camera / cinematic focus on clashes (0x2cc080, 0x2cca80, 0x2cda20, 0x2cbc40, read the manager list), win
speech, conversation speech, holster-weapon draw (0x635750), unit response sound lookups. The **animation
choice is sim**: the clip frame counts drive exchange timing and when damage lands.

---

## 4. Action-driven clash (ActionDrivenHeroClashClass, vtable 0x8bd978) — unreachable in shipped data

Summary only [H for structure, M for action servicing details]:
- Vtable: 0 dtor 0x6aad00, 1 start 0x6ab200, 2 update 0x6ab6f0, 3 involves 0x6ab490, 4 winner (0x264a30),
  5 object removed 0x6ab4d0, 6 list copy 0x6ab1e0.
- Start: involved objects must have at most two distinct owners (a third → fail), exactly two, and enemies.
  Per object: hold (0x63f060(obj, 1, 1): stop locomotor, suspend behaviours 9, 10, 11, 7, 0x20); +0x3b4 = 1 on
  the object **and** its team container; new coordinator + Move_To own position.
- Update: an object with no running action picks one by relevance-weighted random (one synced
  `Get_Float(0, total)`), actions are serviced at their interval (MoveActionClass with Move_Use_Jetpack etc.,
  `MOVEMENT.md`), finished actions end.
- Object removed: drop it; if the remaining objects share one owner → winner = synced `Get_Int(0, n-1)` among
  them; release all (hold off, +0x3b4 = 0 on object and container); manager removes the clash.

Port recommendation `[port]`: parse `Action_Driven` entries and their actions, never start them (behaviour
identical to retail for shipped data, since `Clash_Range 0.0`). If a mod sets a non-zero range, log once.

---

## 5. The +0x3b4 byte

### 5.1 Writers [H]

| Where | What |
|---|---|
| 0x6ac780 melee start | := 1 on hero1, hero2 |
| 0x6ab200 action start | := 1 on each object and its team container |
| 0x6acb80 melee release | := 0 (not in state 2) |
| 0x6acc30 / 0x6ac940 melee update / removal | via release |
| 0x6ab4d0 action removal | := 0 on all + containers |
| 0x472560 | object copy (copies the byte) |
| 0x3a5ad0 / 0x39ce40 | object save / load chunk (with +0x3a0, +0x3a1, +0x3a2) |

Clash objects and the manager have no save code [H]: after a load, a hero saved mid-clash has +0x3b4 = 1 and
no clash, i.e. it is stuck as in the state-2 quirk [M quirk; saves are out of scope for the port].

### 5.2 Readers

**Pairing rule 0x39a540(U, X)** [H]: true when X is non-null, both U and X have +0x3b4 != 0, the manager exists,
and the first clash involving U also involves X. Every caller below uses it as: *if U or X has +0x3b4 == 1,
then U may act on X only if 0x39a540(U, X)*. In words: **a clashing unit can only be acted on by, and can only
act on, its clash partner.**

| Reader | Effect | Sim? |
|---|---|---|
| Fire_At 0x3825b0 | refuse fire outside the pairing | sim |
| Hardpoint target pick 0x385190 | skip non-partner targets | sim |
| Ability accept (vf+0x28) of ArcSweep / ConcentrateFire / GenericAttack 0x6eef00, CableAttack 0x6f2b40, Earthquake 0x6f7510, Eat 0x6f7ec0, EnergyWeapon 0x6f9ca0, ForceLightning + Hack 0x6fd610, Telekinesis 0x6fe400, Grenade 0x700c10, IonCannonShot 0x704a80, PersonalFlameThrower + MaximumFirepower 0x705cd0, LuckyShot 0x706410, Repair 0x70ca40, TractorBeam 0x710540, Berserker 0x712ef0 (written as "both 0, or pairing"), SuperLaser 0x715250, LeechShields 0x717b50 | refuse target outside the pairing | sim |
| Target candidate scan 0x5662b0 | skip clashing candidates (`TACTICAL_COMBAT.md` §2.3 item 3) | sim |
| Validate_Current_Target 0x567c50 | drop target, reason 4 | sim |
| Valid-target test 0x5680b0 (test 5) | invalid | sim |
| Set_Target 0x569160 | refuse | sim |
| Targeting services 0x634810, 0x633ae0, team 0x62eeb0 | drop the current target while in a clash | sim |
| Lua Unlock_Current_Orders 0x581fd0 | error, no unlock | sim (Lua) |
| AI free store 0x7287a0 | skip clashing units | sim (AI) |
| Clash manager 0x56c540, 0x56c790, 0x56c4c0 | detection filter, pairing, removal hook | sim |
| HolsterWeapon 0x635750 | weapon holster draw | visual |
| 0x67d580 dynamic land avoidance | attack-anim exemption [L meaning] | [L] |

`Lock_Current_Orders` does not test +0x3b4 [H]. Galactic AI notes that mention +0x3b4 see it always 0 outside
land [M].

---

## 6. ***Port spec*** (smallest model with retail behaviour for shipped data)

State (per battle, land only): `std::vector<MeleeClash>` in start order; per unit `bool in_clash` (the
+0x3b4 byte; saved with the unit). Per clash: `hero1, hero2` (object ids), entry index, start healths,
state {Approach, Snap, Fight}, spot1 / spot2, snap start pos / yaw, snap start / end frame, current outcome,
previous outcome (0), exchange winner / loser, damage-applied flag.

Start rule (land mode tick, after weather, before the common object tick):
1. Only when `frame % 15 == 0`. For each entry with `Clash_Type == Melee_Duel` in file order (duplicates kept).
2. Candidates per hero type: units of exactly that type, alive (not dying / death clone / removed), not
   `in_clash`, not on an active jet-pack, whose current attack target is none or a unit of the other type.
   Team containers: all members must pass. **Order: ascending object id** `[port]` (retail uses the object
   manager list order, which is creation order in practice; ascending id gives the same result for normal
   battles and is deterministic).
3. For each a in A, for each b in B: 3D dist² <= range²; both heroes, owners differ and are enemies → start;
   remove b from B; next a.

Per tick (every frame, clashes in start order):
- Approach: if either is walking → wait; else if `|dist² - D²| > 0.5` → Snap; else Fight.
- Snap: lerp position and angles from the recorded start to spot / facing over `(int)(30*t)` frames (formula
  §3.4, keep the "minus 180" quirk); at end → Fight.
- Fight: face each other, suspend IDLE + locomotor; exchanges per §3.6 (anim-driven timing, damage at 80 %
  of the clip, once per exchange; Take_Damage kind 1 with the winner as non-projectile source).
- While `in_clash`: targeting suspended; movement orders locked (group Move_To drops the order); every
  target / fire / ability accept applies the pairing rule (§5.2); area damage still applies.

End rule: loser dead after exchange damage, or either hero removed → release (unlock, resume targeting /
IDLE / locomotor as per state, `in_clash = false`), except a hero still in Snap is not released (retail quirk;
keep it `[port]` for parity or fix it and record the choice in `docs/PHASE4.md`). No timeout, no separation.

Determinism:
- Detection draws nothing. Each exchange draws exactly one synced float `Get_Float(0, total)` (except
  conversation exchanges, which draw nothing), plus one synced int `Get_Int(0, n-1)` only when the anim has
  n >= 2 variants (shipped: never). Draws happen in the clash manager step (after weather, before the common
  object tick), clashes in start order. Exact draw positions relative to other systems are the port's choice
  under the parity tiers (`docs/DECISIONS.md`); the outcome distribution must match: P(first exchange) =
  0.4 / 0.3 / 0.3, later exchanges renormalised without the previous outcome.
- New coordinators created at Begin build their formation at the next coordinator update; any formation draw
  follows `MOVEMENT.md` §4.7.4.
- Iterate units by id, never by pointer; all distances in float as written (no FMA).

Readers to wire in the port (already stubbed: `tactical_ai.hpp` `hero_clash`, `battle_ai.cpp`,
`combat_system.cpp`, `bind_objects.cpp`, `ability_classes.cpp`, `tactical_battle_hangar.cpp` holster): one
helper `clash_allows(u, x)` implementing the pairing rule, used by fire, hardpoint pick, the listed ability
accepts, target scan / validate / set, the AI free store, and Lua `Unlock_Current_Orders`.

Action-driven entries: parse, never start (§4).

---

## 7. Build-pad occupancy (s78n t05)

### 7.1 The +0x150 pack and its slots [H]

Only objects that host tactical build pads (TACTICAL_BUILD_OBJECTS) get the +0x150 pack (created lazily by
0x390f70 from the attach path 0x42db20). Slots: +8 = under-construction (UC) object, +0x10 = finished
structure, +0x20 builder, +0x24 / +0x28 cooldown, +0x2c lock byte.

- **Set +8**: build start 0x42dbd0. Requires both +8 and +0x10 null, cooldown elapsed (pad +0x28 <= mode frame),
  a valid player, the build-allowed test 0x39a2c0, and affordability; creates the UC object; +0x20 = builder,
  +0x24 = 0.
- **UC completes** (normal path) 0x42e890: kill the UC object silently (0x391310(UC, 1)), +8 := 0,
  +0x10 := the finished structure.
- **Galactic-persistence restore** 0x42d8b0 (structure carried over from galactic) sets +0x10 directly.
  Correction to `TACTICAL_COMBAT.md` §5.4.3 item 1, which names 0x42d8b0 as the completion: the normal completion
  is 0x42e890; 0x42d8b0 is the restore [H].
- **Content dies** (any death, including sale 0x42ecd0 which kills the object): the Die notify reaches the pad
  synchronously (0x42e6d0(pad, obj)): if obj == +8 → +8 := 0; if obj == +0x10 → +0x10 := 0, +0x2c := 0
  (and the galactic persistence record is cleared, 0x42f160, when the type is persistent); then
  `Destroy_When_Child_Dies` kills the pad, capture state resets, cooldown +0x24 / +0x28 starts when the type's
  rebuild delay is > 0, and the pad plays its anim 0x75.

"Occupied" therefore means: +8 or +0x10 non-null, and both are cleared **in the same frame the content dies**.

### 7.2 Port mapping

The port's build-pad host is `PadState` in `engine/apps/sim/include/alamo/sim/tactical_battle.hpp`;
`TacticalBattle::pad_contents(pad)` (`tactical_battle_pads.cpp`) returns `built`, else `uc`, else
`kNoObjectId`. Retail "occupied" == `pad_contents(T) != kNoObjectId` **and that content is alive**. The port
clears the slots by polling dead objects in `pads_tick`, not at death, so a dead content can still be listed
for the rest of the frame: the occupancy test must also check the content is alive (or the port clears the
slots at the death event, which matches retail exactly) [H for retail, port note].

### 7.3 Convert-projectile / Bribe victim test 0x395c10 (order) [H]

`0x395c10(victim, mask)`; convert projectiles pass `Projectile_Convert_Enemy_Targets_Category_Mask`, Bribe
passes an all-categories mask on its target:
1. victim type `CategoryMask` & mask != 0;
2. victim +0x3a2 & 0x08 clear (not a stolen vehicle, §8);
3. no proximity mine attached (slot byte +0x387 == -1);
4. victim type `Friendly_Damage_Amount` <= 0;
5. victim type not `Is_Named_Hero`;
6. no +0x150 pack, or both its +8 and +0x10 null (not an occupied build pad).

Then the convert step (0x4ca2f0) skips a victim already owned by the new owner.

---

## 8. Vehicle-thief takeover flags (s78n t05)

### 8.1 Retail flow [H]

Vehicle_Thief ability apply 0x7112f0 (user = thief, target = vehicle): thief must have the VEHICLE_THIEF
behaviour; accept (vf+0x28) passes; XY distance minus the target's extent (0x397780) in
[`Activation_Min_Range`, `Activation_Max_Range`]; one synced float draw `< Activation_Chance` (shipped: Chewbacca,
range 0-50, chance 1.0). If the vehicle type has a `Vehicle_Thief_Inside_Clone` (20 shipped types have one):
spawn the clone at the vehicle's position / orientation **with the vehicle's owner**, kill the vehicle silently;
then board the thief into it (0x63fae0) and turn CAPTURE_VEHICLE on for the thief.

Boarding 0x63fae0(behaviour, thief, vehicle):
1. thief data pack: state, time (frame / FPS), vehicle id, **vehicle's previous owner** (saved);
2. transition particles (visual);
3. if the vehicle has unit ability 46 (EJECT_VEHICLE_THIEF) → turn it on;
4. **Change_Owner(vehicle, thief's owner)** (0x38e5a0);
5. vehicle +0x3a2 |= 0x08;
6. (store notify when the vehicle type is a store type);
7. vehicle +0x3a1 |= 0x10;
8. thief garrisoned into the vehicle (garrison pack add, pack +0x31 = 1), GOM notify, signal 0x29 on the thief.

### 8.2 Meaning of the bits [H unless noted]

- **+0x3a2 & 0x08 "stolen vehicle"**: only writer is boarding 0x63fae0; never cleared; saved; survives later
  owner changes (eject, conversion attempts). Readers: 0x395c10 (§7.3 step 2: stolen vehicles cannot be bribed
  or converted) and the unit response sound lookups 0x373700 / 0x3738b0 / 0x3739a0 (a stolen container uses the
  type's default response events; audio only).
- **+0x3a1 & 0x10 "incognito: not auto-acquired, does not auto-acquire, until it deals or takes damage"**:
  writers boarding 0x63fae0 and the Blast ability apply 0x71c0b0. Cleared by Take_Damage 0x3a9e30 (on the victim
  when a source is given and that source has no targeting pack or is targeting this victim; on a non-projectile
  source that has the flag, which then also gets CAUSE_DAMAGE; on a projectile's shooter, 0x2aca60) and by
  MaximumFirepower 0x707390 (team members). Not cleared by owner change; saved. Readers: target candidate scan
  0x5662b0 (`TACTICAL_COMBAT.md` §2.3 item 1: skipped as a candidate), hardpoint target pick 0x385190, hardpoint
  auto-scan 0x387400 (an owner with the flag does not scan), targeting services 0x634810 / 0x633ae0 (a unit with
  the flag does not auto-acquire), Take_Damage.

### 8.3 Port insertion point and gap

Port code (read for this mapping): `engine/tactical/abilities/src/ability_classes.cpp` (VehicleThief: range,
chance, `SpawnRequest` CaptureVariant with `q.owner` = victim owner, victim killed, CAPTURE_VEHICLE on) and
`engine/apps/sim/src/tactical_battle_oi_abilities.cpp` (CaptureVariant case: spawn
`Vehicle_Thief_Inside_Clone` with `q.owner`, `s4_board(mode, q.source, id)`, EJECT_VEHICLE_THIEF on);
`s4_board` in `engine/apps/sim/src/tactical_battle_garrison.cpp`.

- **Gap**: the port spawns the clone with the victim's owner and `s4_board` has no owner change, so the stolen
  vehicle stays with the enemy. Retail changes the vehicle's owner to the thief's owner at boarding (§8.1 step 4)
  and records the previous owner on the thief (for eject).
- Set `Unit::flags_3a2 |= 0x08` and an incognito flag (the +0x3a1 & 0x10 equivalent) on the clone in the
  CaptureVariant case right after `s4_board` (or inside a boarding helper used only by the thief path), together
  with the owner change.
- Readers to wire: `bribe_accepts` step 6 / the convert victim test refuse `flags_3a2 & 0x08`; target candidate
  scans skip incognito units; incognito units do not auto-acquire; Take_Damage clears incognito per §8.2.

---

## 9. Open points

- [M] Approach on the start frame: walk (path already built) or slide (snap starts at once) — depends on when the
  new coordinator builds its path relative to the clash manager step (§3.3).
- [M] Exchange timing (≈100 ticks, damage at ≈ tick 82) is derived from clip lengths and the slot-mode rules, not
  measured.
- [M] Variant / clip mismatch path (nothing plays, redraw next frame) and the meaning of clip vf+0x38 [L].
- [M] 0x39ae20 = "active jet-pack locomotor" (only matters for Boba Fett, who is not in a reachable clash).
- [M] Angle lerp 0x63efd0 takes the shortest path.
- [M] Armour / shield steps of Take_Damage for clash damage (Vader / Obi-Wan have no shields; armour per
  `TACTICAL_COMBAT.md` §3.6).
- [L] 0x67d580 (dynamic land avoidance) use of +0x3b4.
- [L] Player call 0x285f80(thief owner, thief) during boarding.

### 9.1 Retail probe (optional; settles the [M] timing points)

Land skirmish probe kit (`tools/probe_mod/`): spawn Darth_Vader (Empire) and Obi_Wan_Kenobi (Rebel) idle about 90
apart on open ground, no other units nearby; log every frame for both: position, facing, hull health, and the
frame the clash starts (both stop targeting). Measure: start frame mod 15; walk vs slide approach and snap length;
exchange period; damage values (188 / 75 at full start health) and that they use start health (damage one hero
first); no outcome repeats twice in a row over many exchanges; the clash ending only on death. A second run:
kill Obi-Wan by script during the snap phase to confirm the stuck-Vader quirk (§3.8).

---

## 10. Entry points

| Address | What |
|---|---|
| 0x3bb440 | land mode tick (step 4: weather, then clash manager) |
| 0x3b7f30 / 0x17d070 | mode vf+0x210: land returns manager / others 0 |
| 0x56c2b0 / 0x56c3c0 | manager ctor / dtor (vtable 0x89a3b0) |
| 0x56ce20 | manager update (detection, then clash updates) |
| 0x56c790 | detection (frame % 15) |
| 0x56c540 | candidate list + filter |
| 0x56c4c0 | object-removed hook (via land vf+0x258 0x3b7a70) |
| 0x56cda0 | remove + delete a clash |
| 0x4814d0 | HEROCLASH.XML entry load |
| 0x480ed0 | damage multiplier by winner type |
| 0x6ab920 | melee ctor (vtable 0x8bd9f0) |
| 0x6ac780 / 0x6abde0 | melee start / begin |
| 0x6acc30 | melee update |
| 0x6aba30 / 0x6acfb0 | snap begin / snap update |
| 0x6abcd0 | fight |
| 0x6ad230 / 0x6ac1b0 | exchange update / new exchange |
| 0x6acb80 / 0x6ac660 | release / end |
| 0x6ac940 | melee object removed |
| 0x6aacc0 / 0x6ab200 / 0x6ab6f0 / 0x6ab4d0 | action-driven ctor / start / update / removed (vtable 0x8bd978) |
| 0x6aae80 | action pick (relevance-weighted) |
| 0x39a540 | pairing rule |
| 0x39a600 | type is hero (named or generic) |
| 0x42dbd0 / 0x42e890 / 0x42d8b0 / 0x42e6d0 / 0x42ecd0 | build start / UC completion / persistence restore / content died / sale |
| 0x395c10 | convert / bribe victim test |
| 0x7112f0 / 0x63fae0 | Vehicle_Thief apply / boarding |
| 0x71c0b0 | Blast apply (sets +0x3a1 & 0x10) |

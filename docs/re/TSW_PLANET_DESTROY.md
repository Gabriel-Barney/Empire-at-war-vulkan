# Tactical super weapon planet destruction: timing and galactic write-back (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe (Ghidra 12.2 export in `re_reference/`). Prose and tables only.
Topic: the PHASE4 "L1 systems" open item "TSW planet-destroyed write-back to `GalacticSim::planet_destroy`
(timing unknown: battle end at state 9, victory monitor 0x341fe0)".

Conventions:
- Addresses are RVAs (image base 0x140000000).
- Confidence: [H] read directly, [M] inferred from strong evidence, [L] guess. **probe** = needs an in-game run, which Gabriel launches; in-game probes are paused.
- "Synced draw" = the lockstep LCG 0x140a13e24 through int(lo, hi) 0x1ffb40 (MAIN_LOOP §4.1). lo == hi returns lo **without** a draw [H].
- FPS = 30. D = planet data (planet GameObject +0xb8). TSW = the object with TACTICAL_SUPER_WEAPON (0x2c); its pack = obj +0x148.
- Current mode = 0x140b15418 (GMM +0x38); during a campaign battle that is the **tactical** mode.

Read with SCRIPT_GAPS §1.2 (TSW state list, Fire), BATTLE_TRANSITIONS §2.5 / §3 (victory types, battle end),
STRATEGIC_GAPS §1.4 (victory 7), P4_SIM_SYSTEMS §3.3 (hack path). Not repeated here.

## 0. Status

| § | Item | Status |
|---|---|---|
| 1 | Answer in one paragraph | [H] |
| 2 | Victory monitor hold (0x341fe0 / 0x3440a0): no battle can end during the TSW sequence | [H] |
| 3 | TSW states 5..13 after Fire, with the frame budget | [H] transitions; [M] exact frame offsets |
| 4 | State 9 in detail: the planet is destroyed **in the galaxy, in that tactical frame** | [H] |
| 5 | What 0x3f76c0 does (and does not do) | [H]; list naming [M] |
| 6 | Victory 7 / tactical type 0x10 from the TSW | [H]; victim / list semantics [M] |
| 7 | Story events | [H] call; [M] which story manager hears it |
| 8 | Battle end afterwards; ownership | [H] no TSW-specific step; ownership [M] |
| 9 | Corrections to other docs | [H] |
| 10 | Port-affecting findings | — |

---

## 1. Answer

The galactic planet is destroyed **immediately, inside the tactical battle**, on the frame the TSW behaviour
(vf6 0x42a7b0) services state 9 — not at battle end. That frame calls the planet behaviour's destroy routine
0x3f76c0(planet behaviour, galactic planet object, 1) on the **parent galactic** planet object, raises story
PLANET_DESTROYED on the current (tactical) mode's story manager, and may declare galactic victory 7.
The battle itself cannot end from the moment Fire succeeds until the TSW sequence finishes (state 13), because
Fire puts a hold on the tactical victory monitor (0x341fe0) and state 13 releases it (0x3440a0) and replays the
victory checks that were deferred. The normal battle-end write-back (BATTLE_TRANSITIONS §3.1–3.3) therefore always
runs **after** the planet is already destroyed, and it has no TSW-specific step [H].

## 2. Victory monitor hold [H]

The victory monitor (mode +0x28) has a hold counter at +0x5c.

- 0x341fe0 = hold: +0x5c := max(0, +0x5c + 1). Called by Fire 0x42a200 on the **current** mode's monitor
  (also by 0x60330 and the story reward switch 0x456970).
- 0x3440a0(monitor, replay) = release: +0x5c − 1; while still > 0 it only stores the count. When it reaches 0
  and `replay` is 1, every deferred request in the queue (+0x68 array of {type, player} pairs, count +0x70) is
  re-evaluated in queue order with the normal check 0x344710; the first one that declares a victory stops the
  replay. The queue is then freed. Callers: 0x42a7b0 state 13 (replay 1), 0x456970, 0x61fe0. 0x343f80 is a
  "release everything" variant with the same replay.
- While +0x5c > 0, every victory check (0x344710 and its wrapper 0x346460, used by the per-frame polls and by
  0x28f570) does not evaluate anything: it appends {type, player} to the queue (0x341f40, no duplicates) and
  answers "no victory".

Consequence: from the frame Fire succeeds until state 13, no tactical victory of any kind is declared — not
"all enemies destroyed" (0xb), not a completed retreat (0xc), not a station kill (0xf). The battle keeps
running (units fight, die, retreat countdowns run) but cannot finish. At state 13 the deferred checks are
evaluated against the state at that moment.

## 3. The sequence after Fire [H transitions, M frame offsets]

Countdowns in the pack (+0x30) are decreased by the behaviour's elapsed-frames virtual (vf +0x28) on each
service (0x42b450 and the state-8 inline copy); "≤ 0 → next state" happens in the same service. Assuming one
service per frame with step 1 [M], and F = the frame of the successful Fire:

| State | Entered | Work | Leaves after (vanilla Death Star II values) |
|---|---|---|---|
| 5 power-up | Fire (0x42a200) | camera firing sequence 0x2ccf30, `TSW_SFXEvent_Activate_Voice`, `TSW_SFXEvent_Weapon_Power_Up`, music `TSW_MusicEvent_Activation_Start` (resolved event at type +0x21a0, music priority 8), **victory hold** | int(30 × `TSW_Power_Up_Countdown_Seconds`) = 270 frames |
| 6 | | nothing | 1 service |
| 7 | | attack animation (anim 5), `SFXEvent_Fire` (type +0x1240); countdown := int(30 × `TSW_Attack_Anim_Duration_Seconds`); camera notify if a target is set | 1 service |
| 8 | | countdown | 75 frames |
| **9** | | **planet destruction, §4** — state := 10 first; countdown := `TSW_Explosion_Debris_Creation_Frame_Delay` (type +0x214c) | 1 service |
| 10 | | countdown; at 0: spawn **every** `Death_By_TSW_Replacements` entry (§4 step 9); countdown := `TSW_Post_Destruction_Wait_Frames` (+0x21ac) | 33 frames |
| 12 (0xc) | | countdown | 350 frames |
| 13 (0xd) | | state := 0, target (+0x10) and target type (+0x18) cleared; 0x2f8da0 (presentation manager 0x140b27f60: ends the TSW screen effect [L]); **release the hold with replay** (0x3440a0(current monitor, 1)) | — |

There is no state 11. After 13 the TSW sits in state 0 for good: the "first service" set-up (pack +0x08) has
already run and the dummy is dead, so it never re-arms [H as read].

With vanilla data the planet dies about F+348 and the hold is released about F+732 (≈ 24.4 s after Fire);
the battle can end on that release frame at the earliest. Exact ±1 offsets depend on whether the TSW is
serviced on frame F after the Lua/command Fire and on the vf +0x28 step [M] — **probe** if bit-exact timing
matters.

## 4. State 9 step by step [H]

All in one service of the TSW behaviour, in this order:

1. State := 10; countdown := debris delay (+0x214c). Save the target dummy's position (+0x78..+0x80) and
   orientation (+0x84..+0x8c) in the pack (+0x34 / +0x40).
2. 0x38f750(TSW object): clears three internal lists of the TSW's sub-object (+0xb8, +0xe8, +0x118) [L: attack /
   animation bookkeeping; no sim effect found].
3. No target dummy (pack +0x10) → stop here.
4. Dummy type's `Death_Explosions` list (type +0xce0, count +0xce8): if non-empty, pick one with a synced
   int(0, count − 1) (no draw when count is 1, which is vanilla) and create it at the dummy's position /
   orientation for the **neutral** player (0x294aa0 = first player whose faction has `Is_Neutral` +0x108) in the
   mode's secondary store (mode +0x20); camera shake 0x2cdbb0 at the dummy.
5. Dummy type's `Death_SFXEvent_Start_Die` (+0x12d0) plays.
6. **The dummy is killed** (0x391310(dummy, 1)).
7. Galactic part — only when all of these exist, else stop: the parent mode of the current one (0x28aa00 on
   the GMM mode stack; none in skirmish), the head conflict record of the parent (0x28a7a0), the galactic planet
   object with the record's planet id (0x2aca60 on the parent GOM, record +0), and that object is not dead
   (+0x335 ≠ −1).
8. Victory 7 test and declaration (§6).
9. **The galactic planet is destroyed**: planet object's behaviour 3 (planetary) → 0x3f76c0(behaviour, planet, 1)
   (§5). This is the parent galactic store, mutated while the tactical mode is current.

State 10, at the end of its countdown: every entry of the dummy type's `Death_By_TSW_Replacements` list (type
+0xd20, count +0xd28; vanilla `Destroyed_Planet_Remains_Part_1`, `_Part_2`) is created at the saved dummy
position / orientation for the neutral player in the mode's main store (mode +0x18). No draw. The field
identifications come from the offsets in xml_field_tables.md (`Death_Explosions` 0xcd8, `Death_By_TSW_Replacements`
0xd18, list data at +8) and the vanilla SpaceProps that carry DUMMY_TACTICAL_SUPER_WEAPON_TARGET [M names, H use].

Nothing else in the battle is touched: no tactical unit is killed or damaged by the TSW, the battle's planet
backdrop is the only casualty.

## 5. The destroy routine 0x3f76c0(behaviour, planet, full = 1) [H]

Shared by the TSW (0x42a7b0), the galactic Death Star ability (0x707670) and both IG-88 hack paths
(0x61b790 / 0x61b9f0). In order:

1. full: story PLANET_DESTROYED (0x17) via 0x32a7a0 on **the current mode's** story manager (mode +0xa0), log
   `STORY EVENT - Planet %s destroyed`; then every script in that manager's list gets the planet name (0x52fa40).
2. Siphon fields of the planet data cleared (+0x200 = −1, +0x204..+0x220 = 0).
3. 0x4b7670(D, any owner, respawn heroes): walk D **+0xe0** in reverse; for each object, every hero inside it
   (0x38fa20, recursive) gets a default-time respawn (0x48eb10) and is despawned (0x48cf20); then the object is
   killed (0x391310) and removed from the list.
4. 0x4b7350(D, planet): remove the special structures (D +0x158), then 0x4bdaa0.
5. full: D +0x230 := 0.
6. D +0xa0 list cleared; 0x4b6c50: D +0x2e2 (2 bytes) := 0, +0x2e8 := 0, +0x2f8 := −1 (corruption removal state).
7. If the planet was corrupted (+0x2f4 ≠ −1): +0x2f4 := −1, 0x3ff230, and per-player / local-player corruption
   notifications (0x2861f0 / 0x286150), 0x37cff0 and 0x3f98a0 for the old type.
8. If the current mode's type is 0 (vf +0xe0) and the planet has a +0x100 object: 0x37de80 [L: map-side refresh].
   In a tactical battle this is skipped.
9. full: **D +0x2c8 := 1 (destroyed)**.

Not in 0x3f76c0: the orbit list (D +0xc8), the starbase, the owner. Those are done **only by the hack callers**
after it (0x61b790: visual refresh 0x3a4820, orbit removal 0x4b7860 on D +0xc8/+0xd0 with hero respawn,
starbase clear 0x4bda40(D, 0), owner := the `NEUTRAL` faction's player via 0x3f8060). The galactic Death
Star (0x707670) adds only the visual refresh 0x3a4820 and its ability presentation (0x48e4d0). The TSW adds
nothing.

List naming: D +0xc8 is the orbit list (fleets; BATTLE_TRANSITIONS §1, GALACTIC_ECONOMY §4.1 item 5,
STRATEGIC_GAPS) and D +0xe0 the surface list (GALACTIC_ECONOMY §4.1 item 2, BATTLE_TRANSITIONS §3.3) [M]. So the
routine kills the **ground** presence and leaves fleets in orbit — which is why the Death Star and both battle
fleets survive a TSW shot in the galaxy.

## 6. Victory 7 and tactical type 0x10 from the TSW [H calls, M semantics]

Before the destroy call (§4 step 8), on the **parent galactic** victory monitor (parent mode +0x28):

1. Look up the condition record {type 7, player = TSW owner} (0x342d10). None → skip.
2. Victim = the player in D +0x6c [M: the planet's owner field also used for residual influence], its faction's
   leader type (faction +0x2ac8). None → skip.
3. If the condition record's name list (+0x18..+0x20, 0x20-byte strings) is non-empty, the leader type's name
   must match one entry (case-insensitive) or the test is skipped.
4. The leader must be among the **team members** (0x397bf0 = object +0xc8 list) of an object in D +0xe0.
   Only members are compared, not the listed object itself [H as read; M meaning].
5. Hit → (a) galactic 0x343b90({7, TSW owner, countdown 30}): galactic victory declared now ("VICTORY - ..."
   log), the countdown runs once the galaxy ticks again [M]; (b) a tactical condition {0x10
   `SUB_TACTICAL_STORY_MODE_TRIGGERED_VICTORY`, TSW owner, 30} is added to the **current** monitor (0x341f00);
   (c) 0x28f570 asks the current monitor to check type 0x10 for the TSW owner — held, so it is queued and fires
   at the state-13 release: the battle ends with the TSW owner as winner, victory type 0x10, so the
   retreat-player R = the loser (BATTLE_TRANSITIONS §3.1 step 3).

The galactic Death Star path (0x707670) does the same leader test (without the name list, guarded by
0x3464b0 and a galactic-mode check) and declares only the galactic 7.

## 7. Story events [H call, M routing]

- PLANET_DESTROYED (0x17) is sent by 0x3f76c0 to the **current** mode's story manager. During a campaign
  battle that is the tactical mode's manager, whose owner is the current mode, so it dispatches immediately
  to the tactical plots (0x32e5f0). The galactic story manager is not called, and no delayed event is queued
  for it [M]. The only vanilla consumer, `STORY_EMPIRE_ACTIV_M11_SPACE.XML` (`STORY_PLANET_DESTROYED`
  ALDERAAN → VICTORY Empire), is a tactical plot, which fits. **probe**: whether a galactic plot waiting for
  PLANET_DESTROYED ever fires after a tactical TSW kill (expected: no).
- No story event is sent at battle end for the destroyed planet; the normal battle-end events
  (BATTLE_TRANSITIONS §3.1 step 7) fire as for any space battle.

## 8. Battle end afterwards; ownership [H flow, M ownership]

- The battle ends at or after the state-13 release through the normal path (BATTLE_TRANSITIONS §3.1–§3.3:
  result, pop, 0x2bff60 planet write-back, 0x28bf10 fleet write-back, story, next battle). Nothing in that path
  tests the TSW or D +0x2c8; it runs on a planet that is already destroyed.
- Space write-back 0x2bff60 still sets the starbase level from the surviving station (the TSW does not clear
  the starbase) and stores the attacker id in planet +0x330 [H as read for §3.2; L for its effect on a destroyed
  planet].
- The TSW path never sets the owner. The owner is then the per-frame derived owner (GALACTIC_ECONOMY §4.1):
  no surface units are left, the residual-influence step is skipped for destroyed planets (0x3f3830 tests
  +0x2c8), so starbase / orbital control / leaning decide [M]. **probe**: owner and starbase of a planet after
  a won and after a lost TSW battle.
- Income: destroyed planets use `Planet_Destroyed_Credit_Value` (GALACTIC_ECONOMY §1.4) from the next income.
- `SpecialWeaponsAllowed` (SCRIPT_GAPS §1.1) and TSW Fire (0x42a720) read D +0x2c8 of the battle planet, so both
  turn false in the same battle from state 9 on [H].
- Skirmish: §4 steps 1–6 only (no parent mode); the battle still has the hold until state 13.

## 9. Corrections to other docs

1. **P4_SIM_SYSTEMS §3.3 step 4/5**: 0x3f76c0 removes the D **+0xe0** list (surface, [M]) and does not touch
   orbit, starbase or owner; the orbit removal (D +0xc8 via 0x4b7860, "includes W"), starbase clear and NEUTRAL
   owner are the hack caller's (0x61b790) extra steps, not part of the shared routine. The Death Star (galactic
   and tactical) does not do them [H].
2. **BATTLE_TRANSITIONS §3.3**, first bullet ("GMM+0x1f4 ≠ −1: a super-weapon result pending [L]"): GMM +0x1f4 /
   +0x1f8 / +0x1fc is the pending **corruption mission** result (0x3f6810 increase corruption, story 0x3a / 0x3b
   via 0x326f20 / 0x326f50), not a super-weapon result [H].
3. **SCRIPT_GAPS §1.2 / open item 2**: +0x21a0 = the resolved `TSW_MusicEvent_Activation_Start` (name at +0x2180),
   played with music priority 8; 0x341fe0 = victory monitor hold (§2). States after 9 are 10, 12, 13 (no 11),
   then back to 0 [H].

---

## 10. Port-affecting findings

Current port: `special_weapons.cpp` runs the TSW machine and emits `TswPlanetDestroyed` at state 9;
`TacticalBattle` only latches `l1_planet_destroyed_`; `CampaignBattles` never calls the galaxy;
`GalacticSim::planet_destroy` is the hack's full routine (orbit + surface + structures + starbase + NEUTRAL owner).

1. **When**: call the galactic destroy in the **same frame** as `TswPlanetDestroyed`, while the battle runs
   (not at battle end). Natural place: `CampaignBattles::tick` right after `battle_->tick(...)`, when the latch
   turns on (or a TacticalBattle hook fired from the L1 event loop). Galactic state changes mid-battle are
   invisible to the tactical sim except D +0x2c8, which the port already mirrors with `l1_planet_destroyed_`.
   It must be saved / CRC'd as part of the galaxy (the galactic store is not ticking, but it changed).
2. **What**: split `planet_destroy` into the shared core (= 0x3f76c0: PLANET_DESTROYED story, siphon clear,
   **surface** objects with hero respawn, special structures, corruption clear with notifications, destroyed
   flag) and the hack-only extras (orbit fleets incl. the weapon, starbase, owner := neutral). The TSW calls the
   core only. Do **not** kill orbiting fleets or the starbase or set the owner — the Death Star, both fleets and
   the station are fighting in this battle.
3. The galactic Death Star path (`super_weapon_destroy_planet`, strategic.cpp) has the same problem today: it
   calls the full routine, which kills the Death Star's own fleet; the original (0x707670) uses the core only
   (+ visual refresh) [H].
4. **Victory hold**: add a hold counter to the battle's victory checks: +1 on a successful `tsw_fire`, −1 at the
   TSW's state 13 (port: `TswDone`). While held, `check_victory` must not finish the battle (retreat
   completion, all-destroyed, station / base destroyed — all of them). On release, run the checks once that
   frame; if the TSW leader test hit (item 6), finish with (TSW owner, type 0x10) first. A dead or removed TSW
   mid-sequence would leave the original held forever [L]; the port may release on prune [port].
5. **Timers**: after state 9 the original runs the debris delay (`TSW_Explosion_Debris_Creation_Frame_Delay`)
   and **then** `TSW_Post_Destruction_Wait_Frames` in sequence (states 10 → 12 → 13); the port runs them in
   parallel from state 9, which releases the hold 33 frames early with vanilla data. State numbers: 10, 12, 13,
   then 0 — no 11 (Lua sees only `== 4`, so the value matters only for saves / CRC).
6. **Victory 7 from the TSW**: victim = the planet's D +0x6c player only (port: every hostile player); leader
   searched among team members of D +0xe0 objects only (port: orbit fleets and surface); optional leader-name
   list on the condition. On a hit: galactic `claim_victory(owner, 7, 30)` at state 9 **and** the battle ends
   with (owner, 0x10) at the hold release. Do it before the destroy call (it reads the surface list the destroy
   empties).
7. **Story**: PLANET_DESTROYED goes to the **tactical** StorySession (the battle's plots), not the galactic one
   [M]; the battle-end story / write-back stays unchanged.
8. **Debris lists** (`debris_choices` is a stub returning 0): state 9 = `Death_Explosions` of the target dummy's
   type (one, synced pick when ≥ 2) + `Death_SFXEvent_Start_Die` + kill the dummy; state-10 end = **all**
   `Death_By_TSW_Replacements` at the saved dummy transform; both owned by the neutral player.
9. **Battle end**: no TSW-specific write-back; `finish_battle` runs as usual on the destroyed planet. Ownership
   is whatever the normal write-back plus the derived-owner rules give; do not force neutral.
10. Skirmish (no galaxy): only the tactical part and the hold.

Probes (paused): story routing (§7), exact frame offsets (§3), owner / starbase after a TSW battle (§8).

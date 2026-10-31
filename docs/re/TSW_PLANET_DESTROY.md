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
| 11 | Presentation: camera shake 0x2cdbb0 and the firing camera 0x2ccf30 (no synced state) | [H] formulas, constants, keys; [M] field names, box source; [L] end-of-shot chaining |

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
| 5 power-up | Fire (0x42a200) | camera firing sequence 0x2ccf30 (§11.2), `TSW_SFXEvent_Activate_Voice`, `TSW_SFXEvent_Weapon_Power_Up`, music `TSW_MusicEvent_Activation_Start` (resolved event at type +0x21a0, music priority 8), **victory hold** | int(30 × `TSW_Power_Up_Countdown_Seconds`) = 270 frames |
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
   mode's secondary store (mode +0x20); camera shake 0x2cdbb0 at the dummy (§11.1).
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

---

## 11. Presentation: camera shake and firing camera

Both belong to the **in-game cinematic camera controller** (global object 0x140b27a10, MAIN_MENU_BACKGROUND §1;
`CinematicClass` base, its `End_Cinematic_Mode` is 0x49b5c0). Controller fields used here: +0x34a active,
+0x37c state (Fire uses state 3), +0x34d shake enable (static init 0x17400 writes the dword at +0x34c = 0x10101,
so +0x34d = 1; nothing else writes it [H]), +0x14c shot timer {start = mode frame, +0x150 duration in frames},
+0x154 **shot clock** = frames since the shot started, +0x278 list of shake trackers.

**Synced state: none.** Everything in this section writes only camera, HUD, cursor, letterbox and pacing state.
The one RNG draw is from the generator at 0x140a2ab18 (seeded from `QueryPerformanceCounter` at boot, MAIN_LOOP
§5.1 step 2; reset to 0xf0f0f0f0 only by 0x69040), **not** the lockstep LCG 0x140a13e24 [H]. The shake call
sits inside TSW state 9 (sim code), but whether it does anything depends on presentation state (+0x34a), so the
port must keep it on the presentation side (an event the host plays). The cinematic reads sim positions (TSW,
target dummy, station marker) and never writes them. Side effect on pacing only: starting the cinematic sets
FS +0x37 (logic rate locked to 30, MAIN_LOOP §2) until it ends [H].

### 11.1 Camera shake 0x2cdbb0 → 0x2cdc00 [H unless tagged]

0x2cdbb0(controller, object, explosion type, fixed) = 0x2cdc00(controller, &object position (+0x78..+0x80),
explosion type, fixed) when object and type are non-null. Callers: TSW state 9 (§4 step 4) with fixed = **1**
and type = the chosen `Death_Explosions` entry; DeathBehavior 0x58cf90 (death explosion, fixed 0) and the hardpoint
death paths 0x384a30 / 0x386660 (type +0x430 explosion, fixed 0, direct 0x2cdc00 calls).

**Gate** (all must hold, else nothing): +0x34d (always 1); a current mode exists and its type (vf +0xe0) is 1
(land) or 2 (space); **+0x34a (a cinematic is running)**. So outside a cinematic **no explosion ever shakes the
camera**; the TSW shake happens only because Fire started the firing cinematic (§11.2) and it is still running at
state 9.

**Parameters** (J = `Explosion_Jitter_Factor` of the explosion type, type +0x2130, float, **default 1.0** from the
type constructor 0x367250; FPS = 30):

| CameraShakeEffect parameter | value | TSW planet kill (J = 1, D = 100) |
|---|---|---|
| D (distance) | fixed = 1: **100.0**; fixed = 0: \|object position − camera position\| + 0.1 (camera position = translation of the mode camera's (mode +0x90) matrix, 0x2619f0) | 100 |
| `XAmplitude`, `YAmplitude` | J × 500 / D | 5.0 |
| `RollAmplitude` (degrees) | J × 30 / D | 0.3 |
| `Frequency` | FPS × J × 11 | 330 |
| `NoiseOffset` | int(0..100) from the **unsynced** generator 0x140a2ab18 (0x1ffb40) / 50 | 0.00 .. 2.00 in 0.02 steps |
| `EaseIn` | 0 | 0 |
| `EaseOut` | 0.25 | 0.25 |
| start, duration | start = shot clock +0x154 now; duration = FPS frames (**1 s**) | 30 frames |

The values are stored straight into the parameters (the ranged-parameter limits, amplitudes ≤ 1000, roll ≤ 90,
are not applied). The effect object is `CameraShakeEffectClass` (factory 0x7d93a0(0)); 0x499c40 wraps it in a
`CameraEffectTrackerClass` {effect, start, duration} (0x7d92c0) appended to +0x278. Trackers are never removed
early; they are all freed by `End_Cinematic_Mode` (0x49b5c0). Several shakes add up.

Vanilla J values (PARTICLES.XML): small explosions 1, medium 2, large 3, huge 4, `p_orbital_impact` 3. The TSW's
`Tactical_Planet_Explode_300x_Particle` (the only `Death_Explosions` entry of every `DUMMY_TACTICAL_SUPER_WEAPON_TARGET`
prop in SPACEPROPS.XML) has no `Explosion_Jitter_Factor`, so J = 1.0 [H].

**Evaluation, every camera update** (0x7da320, called from the cinematic camera builder 0x49cd00 with frame c =
shot clock): for each tracker t = (float)(c − start) / (float)duration. Outside [0, 1] it contributes 0. Else
(0x7d97d0):

```
s     = Frequency * t
env   = Ease(t, EaseIn, EaseOut)
dx    = Fbm(s, NoiseOffset, 0.0)            * XAmplitude    * env
dy    = Fbm(s, NoiseOffset, 100.0)          * YAmplitude    * env
droll = Fbm(s, NoiseOffset, 303.1416015625) * RollAmplitude * env      // degrees
shake_offset += dx * R + dy * U
shake_roll   += droll
```

R and U are the normalised first and second **columns** of this frame's unshaken camera matrix (built by
0x22d600: column 0 = camera right, column 1 = camera up, column 2 = −forward; world is Z-up).

`Ease(t, a, b)` (0x7da450): clamp a and b to [0, 1]; e = 1 − b; lo = min(a, e), hi = max(a, e).
t < lo: 0 if t ≤ 0, else u = t / lo, return 3u² − 2u³. lo ≤ t ≤ hi: 1. t > hi: 0 if t ≥ 1, else
u = (t − hi) / (1 − hi), return 1 − (3u² − 2u³). For the TSW (a = 0, b = 0.25): 1 up to t = 0.75, then a
smooth fall to 0 at t = 1.

`Fbm(x, y, z)` (0x7d8a50, 3 octaves): f = 1, sum = 0; three times: (x, y, z) *= f (cumulative), sum += N(x, y, z) / f,
f *= 1.9. So sum = N(p) + N(1.9 p) / 1.9 + N(6.859 p) / 3.61.

`N` (0x7d8590) is Ken Perlin's improved gradient noise: lattice X = floor(x) & 255 (same for Y, Z), fractions
x, y, z; hash A = P[X] + Y, B = P[X+1] + Y, AA = P[A] + Z, AB = P[A+1] + Z, BA = P[B] + Z, BB = P[B+1] + Z;
grad(h, x, y, z): h &= 15, u = h < 8 ? x : y, v = h < 4 ? y : (h == 12 || h == 14 ? x : z),
result = (h & 1 ? −u : u) + (h & 2 ? −v : v); trilinear blend of the 8 corner gradients with the **cubic** fade
w = 3f² − 2f³ (not the quintic of the 2002 paper). P = Perlin's reference permutation (the 256-entry table that
starts 151, 160, 137, 91, 90, 15, …), stored twice (512 bytes at 0x140a45d20) [H, bytes read from the exe].

**Application** (end of 0x49cd00): with pos = keyframed camera position, look = keyframed look point,
d = normalise(look − pos), roll = keyframed roll (degrees):

- eye = pos + **0.1** × shake_offset
- look point = pos + 1000 × d + shake_offset
- roll = roll + shake_roll (then to radians; 0x22d600 applies it about the view axis)

So the shake is mostly an angular jitter of at most about |offset| / 1000 rad. For the TSW: offset up to about
5 × 1.8 = 9 units at the look point (≈ 0.5°), 0.9 units of eye movement, roll up to ≈ 0.54°, for 1 s, full
strength for 0.75 s then fading. Because s advances by Frequency / FPS = 11 lattice cells per frame, consecutive
frames are effectively uncorrelated: a per-frame jitter, sampled at integer shot-clock frames [H math; M on the
visual description]. Nothing scales with eye distance, FOV or screen size.

### 11.2 The firing camera: Fire → 0x2ccf30 → 0x4a5ef0 [H unless tagged]

Fire (0x42a200) calls 0x2ccf30(controller, TSW object, target dummy (pack +0x10), 1) **before** setting state 5.
0x2ccf30:

1. Store the cursor position (GetCursorInfo) in +0x390/+0x394 and copy it to +0x398/+0x39c: the baseline for the
   mouse-move skip test (below).
2. If a shot is already running (state > 1): stop it (0x2cbb80, the end path below).
3. Build the shot: 0x4a5ef0(controller, current mode, TSW, dummy, variant 1). On success state := 3, +0x34a := 1.
   It fails (no camera move at all) when the mode has no object store (+0x18) or TSW / dummy is null.

**Inputs** read once at Fire: TSW id (object +0x50); **E** = the TSW's bounding-box half-extents (render model
+0x2a0 vf +0x60 output, second vector; without a model, the world AABB 0x3982b0 +0xc..+0x14) [M: half-extents];
TSW and dummy positions = translation of their render transforms (+0x254, +0x264, +0x274);
**Mid** = (TSW position + dummy position) / 2.

**Keyframe record** (0x54 bytes, shared by eye and look keys) [H layout; M names]: +0x00 world position
(used when no object), +0x18 offset, +0x24 frame, +0x28 segment mode, +0x2c attach object id (−1 = world),
+0x30 sub-attach id (−1 here), +0x38 tangent scale (0.5), +0x44 ease-out fraction (acceleration share of the
segment that starts at this key), +0x48 ease-in fraction (deceleration share of the segment that ends at this key),
+0x4c orbit period (300, mode 3 only), +0x51 offset is world-aligned (else rotated by the object), +0x52 use muzzle
bone. An attached key's position (0x49d640) = the object's render transform applied to the offset (rotation and
translation) + (0, 0, `Ranged_Target_Z_Adjust` of the object's type, type +0x4dc, default 6.0); it is re-read every
frame, so attached keys follow the object. If the object is gone the camera keeps its previous position.

**Eye keys** (offsets in the TSW's local frame):

| key | frame | where | segment mode | +0x44 / +0x48 |
|---|---|---|---|---|
| P1 | 0 | TSW + (2 Ex, −2 Ey, 2 Ez) | 0 | 0.5 / 0 |
| P2 | 300 (10 s) | TSW + (1.5 Ex, −1.5 Ey, 1.5 Ez) | 2 (hold) | 0 / 0.25 |
| P3 | 315 (10.5 s) | TSW + (15 Ex, −5 Ey, 5 Ez) | 0 | 0.5 / 0 |
| P4 (only if a station marker exists) | 765 (25.5 s) | world: M + u × (\|M − T\| + 2000), u = normalise(M − T) | 0 | 0.5 / 0 |

M = position (+0x78..+0x80) of the first object returned by the store query 0x2a9ff0(store, 0x11, …) whose type
name is `Space Station Position` (case-insensitive; MARKERS.XML marker, `Behavior MARKER`) [M: query 0x11 = the
marker list]; T = the dummy position. P4 therefore looks back at the battle from 2000 units beyond the station
marker, on the far side from the planet. Frames are FPS × 10, (int)(FPS × 10.5) and (int)(FPS × 25.5).

**Look keys** (camera target):

| key | frame | where | segment mode |
|---|---|---|---|
| L1 | 0 | TSW + (0, −Ey, 0.15 Ez) (attached) | 2 (hold) |
| L2 | 315 | Mid (world, fixed at Fire) | 2 (hold) |

**Interpolation** (0x49c540 eye, 0x49de50 look; frame c = shot clock): find the segment [k, k+1] with
k.frame ≤ c < (k+1).frame; σ = (c − k.frame) / ((k+1).frame − k.frame); w = Accel(σ, k.+0x44, (k+1).+0x48); then by
k's mode: 0 = Catmull-Rom Hermite (tangents = 0.5 × (next − previous) world positions, 0x499ef0) **only when both
keys are world keys**, otherwise linear lerp of the two evaluated positions by w (0x49aa40 → 0x49a4b0); 1 = linear;
2 = **hold** k (the camera jumps when c reaches k+1: a cut); 3 = orbit. Before the first key: key 0; after the last:
hold the last key (still re-evaluated if attached).

`Accel(σ, a, b)` (0x49bb40) is a trapezoid speed profile: e = 1 − b, v = 2 / (2 − a − b);
σ < a: v σ² / (2a); a ≤ σ < e: v a / 2 + (σ − a) v; σ ≥ e: v a / 2 + (e − a) v + v (σ − σ²/2 − e + e²/2) / b.

**Resulting shot** (c = frames since Fire; TSW states from §3, F + 348 = planet kill):

| c | camera |
|---|---|
| 0 | cinematic mode on (below); eye at P1 beside the TSW, looking at a point just off the TSW's −y side |
| 0–300 | eye glides P1 → P2 (linear in world space, accelerating over the first half, decelerating over the last quarter); look stays on L1; both follow the TSW |
| 300–315 | hold P2 |
| **315 (10.5 s)** | **cut**: eye to P3 (far out on the TSW's +x side), look to Mid (half way to the planet). About 1.1 s before the planet dies |
| 315–765 | with a station marker: eye slides P3 → P4 (accelerating over the first half, constant speed to the end); without one: eye holds P3 (attached) |
| ≈ 348 | state 9: explosion and the §11.1 shake (if the shot is still running) |
| 765 (25.5 s) | shot timer ends → end path below |

FOV: no FOV keys, so the camera keeps the mode camera's setting (mode vf +0x1e8, +0x48 degrees; 55° if none)
[M]. Roll: no roll keys, 0. No look smoothing (+0x1bc stays 0), no frame blend (+0x270 = 0), no terrain clamp
(space). The camera is evaluated at integer shot-clock frames and the clock is the **mode frame** (0x4ad3a0
records mode +0x10), so it stops while the game is paused and moves in 30 Hz steps [M for the visual consequence].

**Cinematic mode on** (0x49b1a0(controller, 0), called at the end of 0x4a5ef0, then the timer 0x4ad3a0(+0x14c,
765 / FPS s)):
1. Save a copy of the mode camera (+0x2c8, once) and set its projection near plane 1.0, far 10000
   (0x261900 / 0x261ab0); 0x4a5ef0 then sets the **near plane to 100.0** (far unchanged).
2. HUD hidden (0x140b2a5f9 := 1, 0x140b2a5fb := 2, HUD object 0x140b27f60 notified) [M: hide]; flag 0x140b28d63
   cleared; counter 0x140b2c198 += 1; two toggles 0x13ecc0(0) / 0x48a3a0(0) [L: roles unknown]; mouse cursor
   hidden (0x262490(…, 0)) unless the mode says otherwise (vf +0x48) or the attract demo runs; FS +0x37 := 1.
   Each change is remembered in +0x310..+0x316 and undone at the end.
3. Letterbox: on the first controller update (0x2cd870, needs GameConstants `In_Game_Cinematics`) the bar overlay
   0x140a28c10 slides in over **0.5 s** (0x44b2c0); bar height = `Camera_FX_Manager_Letterbox_Height`
   (GC +0x11b34, vanilla 0.085) [M: the overlay is the letterbox: two quads, same height constant as Lua
   `Letter_Box_In`].

**Input while the shot runs** (0x2cc3e0, only when no GUI dialog has focus): the controller is offered mouse
events before the HUD and the world and consumes them. In state 3 a mouse move (code 0xb) smaller than 1/20 of
the screen width and height from the baseline is swallowed; a larger move, or any other event routed to it
(codes 2, 4, 5, 7, 8, 10, 12 [M: button / wheel events]), **ends the shot at once** (0x2cbb80). The TSW sim
sequence is unaffected; a skipped shot means no shake at state 9 (gate +0x34a).

**End** (shot timer expired, 0x4aaf00): because state is 3, the controller first tries to chain another shot
(held while Tab is down 0x20e2f0 or +0x317 is set; else 0x2cda20: combat shot 0x2cc780, list shot 0x4a4ac0,
0x49f370, interesting object 0x2cd930 — the attract-demo shot chain) [L: in a normal battle these are expected to
find nothing; **probe**]. With no follow-up: `End_Cinematic_Mode` 0x49b5c0 (free the shake trackers, undo step 2,
restore the saved mode camera +0x2c8 including its projection) and 0x2cbb80: letterbox out over 0.5 s scaled by
how far it is in (0x44b6f0) when mode +0x98 is set, else removed at once (0x44cb60) [L: +0x98 meaning]; +0x34a := 0,
state := 0. The RTS camera then resumes from wherever it was before Fire.

### 11.3 XML that feeds it

| tag | owner | used for |
|---|---|---|
| `Explosion_Jitter_Factor` (+0x2130, float, default 1.0) | the explosion type picked from the dummy's `Death_Explosions` | shake amplitude, roll and frequency (§11.1) |
| `Death_Explosions` (+0xcd8) | the target dummy (`Alderaan_Backdrop_Large` etc.) | no entry → no explosion and no shake |
| `In_Game_Cinematics` (GameConstants, True) | global | the controller only advances (and letterboxes) when true; false would leave the shot frozen at frame 0 until input ends it [L] |
| `Camera_FX_Manager_Letterbox_Height` (GameConstants, 0.085) | global | letterbox bar height [M] |
| `Space_Model_Name`, `Scale_Factor` (Death_Star: `w_planet_deathstar02.alo`, 2.5) | the TSW | bounding box → E, i.e. every eye / look offset [M] |
| `Ranged_Target_Z_Adjust` (+0x4dc, default 6.0; Death_Star does not set it) | the TSW | added to the z of every TSW-attached key |
| marker `Space Station Position` (MARKERS.XML) placed on the map | map | P4 exists only when one is present |

No `TSW_*` tag drives the camera: the frame numbers (300, 315, 765), offsets (×2, ×1.5, ×15/×5, 0.15, +2000) and the
near plane (100) are constants in 0x4a5ef0 [H].

### 11.4 Port notes (presentation, apps/game)

1. Replace the [port] shake (3 s, 2 % of eye distance, 7 Hz) with §11.1: J × 500 / 100 position amplitude on the
   **look point** (0.1 × on the eye), J × 30 / 100 degrees of roll, 1 s, ease-out over the last 25 %, 3-octave
   Perlin fBm at Frequency 330 sampled per sim frame. The NoiseOffset draw must come from a non-sim generator.
2. Shakes apply **only while the cinematic camera is active**; the normal RTS camera never shakes (this also
   holds for ordinary death and hardpoint explosions, which use D = distance + 0.1).
3. Implement the firing shot of §11.2 driven by the tactical frame count since Fire (not wall time): letterbox,
   HUD and cursor hidden, near plane 100, the P/L keys, the cut at frame 315, input-skip rules, restore of the
   pre-Fire camera at frame 765.
4. Nothing here may feed the sim or the CRC; the sim only emits the Fire / state-9 events it already emits.

Open: the end-of-shot chaining (§11.2 End) and the meaning of mode +0x98 [L, probe]; exact roles of the HUD /
toggle globals in step 2 [L]; whether E is the model box or the AABB for the Death Star (both are boxes; the model
path is taken when a render model exists) [M].

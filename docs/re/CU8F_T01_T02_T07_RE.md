# CU8F t01 / t02 / t07: SAP disable byte, engine-loop suppression, energy-pool engine disable, Force-disable emitter, space retreat fallback query

Recovered 2026-11-07 from the FoC x64 binary (`StarWarsG.exe`, RVAs below) and the Ghidra output, for roadmap
group cu8f. Shipped data: FoC / EaW `Data\XML` (extracted text zips). Confidence: [H] read directly, [M] inferred
from several consistent reads, [L] hypothesis. Object fields as in ABILITIES_HARDPOINTS.md (obj +0xf0 = combat
data "CD", obj +0x100 = special-ability data pack "SAP", obj +0x298 = type, obj +0xa8 = locomotor data pack).
"Now" is the mode frame unless stated; FPS = the frame-rate global 0x140b0a340.

## 1. t01: SAP +0x28, the "special abilities disabled" byte

### 1.1 What the byte is [H]
- The SAP (`SpecialAbilityDataPackClass`, ctor 0x55d530, vftable 0x899980) embeds a
  `SpecialAbilityManagerClass` at SAP +8 (ctor 0x37b430, vftable 0x865c20). SAP +0x28 is the manager's
  byte +0x20. The manager also holds the owner object (+0x10), the owner's type (+0x18) and the per-ability
  due-frame list (+0x28 / +0x30, 0x28-byte entries keyed by ability name).
- Constructor 0x37b430 and both destructors (0x37b460 / 0x37b4e0) set it to 0.

### 1.2 Writers (complete) [H]
A decompile scan of every byte store at manager +0x20 / SAP +0x28 (manager methods 0x37a000..0x37f000, SAP
methods 0x55c000..0x561000, and every function that loads obj +0x100 and stores a byte at +0x28; also a
disassembly scan for `mov [reg+0x28], …` after `mov reg, [x+0x100]`) found exactly two writers:

| RVA | Name (port) | Sets | Body |
|---|---|---|---|
| 0x37b9e0 | `SAP_Disable_All` | 1 | Returns 0 at once if the owner or type is missing or the byte is already 1. Otherwise, for every special ability in the type's `Abilities` list (type +0x1a98, list order) that is enabled (0x37be10 == 1) and allowed in the current mode (0x37c270), and whose `Activation_Style` is 1..5 or 12 (Global / Combat / Galactic / Space / Ground / Skirmish_Automatic): ability vf +0x80 (Cancel, 0x48cdd0 in the base class: logs "%s is CANCELLING %s's %s effect", then vf +0x78 remove-all with reason 2) on the owner, then 0x37bb10 drops that ability's due-frame entry. Then byte := 1, returns 1. |
| 0x37d7f0 | `SAP_Enable_All` | 0 | Returns 0 if the owner or type is missing or the byte is already 0. Byte := 0 **first**, then for every ability that is allowed in the mode and enabled (any style): if its service interval (vf +0x18) > 0, a new due frame = **synced draw** `Get_Int(now, now + interval)` (0x1ffb40 on the synced LCG 0x140a13e24; now clamped to ≥ 0) stored by 0x37dc10, the same stagger as SAP init (TACTICAL_GAPS §4.3); then vf +0x88 (Restore, base 0x48e9e0: logs "%s is RESTORING %s's %s effect" and re-applies, slot 30, to every still-live target in the owner's effect register 0x55e330). Returns 1. |

Their only callers (direct-call and pointer scan of the whole image):

| Caller | Class / slot | When |
|---|---|---|
| 0x6f3fc0 → 0x37b9e0 | `CancelSpecialAbilityClass` (vftable 0x8c5b28) slot 30 **apply(user, target)** | First calls its own accepts-target (slot 5, 0x6f4060); refused → nothing. Then disables the **target's** manager, `Begin_Effect` 0x48cca0(user, target, 0, 0) (registers the effect in the user's SAP) and the activation presentation 0x48e4d0 (light, particle, `SFXEvent_Activate`). |
| 0x6f40c0 → 0x37d7f0 | same class, slot 31 **remove(user, target)** | Re-enables the target's manager, then `End_Effect` 0x48e920. |

- **Accepts target** (0x6f4060) [H]: the target has a SAP (obj +0x100 ≠ null) and is **not friendly** to the
  user's owner (0x39a2c0(target, owner player, 0) false: enemy or neutral; self and allies refused). No range or
  type filter in the class itself.
- `Cancel_Special_Ability` has service interval 0 (TACTICAL_GAPS §2 table), so it never updates. Its remove runs
  through the usual special-ability removal paths (remove-all vf +0x78 0x48f1e0 on the user: user death /
  removal, mode exit) [M: removal triggers not traced one by one].
- **No other source.** No Lua function, story event, behaviour, galactic code or loader writes the byte.
  The manager's load (0x37c5b0, SAP chunk 1) does not read it, so a loaded game starts with every manager
  enabled [M: consequence of "not read"; the port saves `sap_disabled`, harmless].
- **Shipped content never sets it.** The only `Cancel_Special_Ability` entries in FoC and EaW
  (`Akbar_Cancel_Special_Ability`, Combat_Automatic, in NAMEDHEROUNITS.XML and UNIQUEUNITS.XML) are inside XML
  comments. One workshop mod (1770851727) defines one. [H]

### 1.3 Readers [H]
Every manager entry point returns early while the byte is 1, so the object uses **no special ability at all**:
- `SAP_Update` 0x37da80 (interval abilities are not updated), `SAP_Trigger_By_Style` 0x37b580 (auras on unit
  add, Take_Damage style 10, …), activate-by-name 0x37b750 / 0x37b8a0, the targeting fallback 0x37c9a0, the
  style-10 helper 0x37cf20 / 0x37ce30, 0x37d0d0, 0x37d160 (`SAP_On_Take_Damage`), 0x37dc10 (due-frame store),
  0x37df00 (`Set_Ability_Enabled`), 0x37cab0 / 0x37cc10 / 0x37ccf0 / 0x37cd80 / 0x37cdc0 / 0x37cff0, the
  signal handler vf1 0x37d6d0 and SAP init 0x37c050 (the apply-on-create of automatic abilities).
- **Unit abilities:** gate 5 of `Can_Activate_Unit_Ability` 0x39b480 (ABILITIES_HARDPOINTS §2.4) and the
  activation / autofire paths 0x38a350, 0x3a1930, 0x3a1ce0, 0x3a3260, 0x3a3500, 0x3a38d0 read SAP +0x28
  directly.

### 1.4 What a port needs
- Wire `set_abilities_disabled(target, true)` from the `Cancel_Special_Ability` apply and `(target, false)` from
  its remove, with the class accepting any non-friendly target that has abilities. **The port's current
  CancelSpecial (ability_classes.cpp, "cancels another named special of the owner [M]") is not what the original
  does** and should be replaced.
- Disable also cancels the target's live automatic-style (1..5, 12) effects and forgets their due frames; enable
  re-staggers each interval ability with one synced RNG draw (list order) and re-applies the automatic effects
  to their registered live targets. The byte must gate every SAP entry point, not only gate 5.
- No vanilla data reaches this code, so wiring changes no vanilla golden.
- Probe (only if parity on mods matters): un-comment `Akbar_Cancel_Special_Ability` in a probe mod, fight a
  space battle with Home One against an enemy hero with an aura (e.g. Combat_Bonus) and observe the aura end and
  the hero's unit-ability buttons grey out; the log lines "is CANCELLING" / "is RESTORING" mark the two calls.
  Which events trigger a Combat_Automatic apply with which target object is [L] (0x37b580 style 2 contexts).
- **Probe run (cu8g t09b, 2026-11-08, run `tools/probe_mod/cu8g_probes/out/20261108-065026-abil`, Claude drove the
  game).** Kuat space skirmish: a human Accuser (TRACTOR_BEAM) and an AI Corellian corvette 700 units away. AI Home
  One, with `Akbar_Cancel_Special_Ability` un-commented in the probe mod, was spawned 1001 units from the Accuser at
  frame 810.
  - Base phase (from frame 510): the beam activated at once (ready 0, active 1), and the corvette was under it.
  - Cancel phase (frames 810..1560): the beam stayed active and the corvette stayed under it in all 244 polls. All 26
    further activation calls returned ok with ready 0. **No cancel was seen on the Accuser.** Either the
    Combat_Automatic apply did not fire on it, or a disable does not end an effect that is already active [L which].
  - **The game crashed** (APPCRASH, c0000005 at `StarWarsG.exe`+0x7107c8, 06:56:58) at about the end of the cancel
    phase. That is where the probe despawns Home One (frame 1560; the last record is frame 1542, and the scan runs
    every 4 s), so the restore timing was not measured [M that the despawn caused the crash].
  - The same script also armed in the main-menu background battle, which has an AI Rebel (instance EB7500 in the
    report). Ignore that instance: the beam was never ready there.
- **Second run (cu8g t10, 2026-11-08 07:32, run `20261108-073231-abil`).** Home One was ordered to attack the
  Accuser (an idle Home One may never be "in combat"). It was then killed with `Take_Damage` instead of despawned.
  - The beam again stayed active through the whole cancel phase.
  - The game crashed again at the kill (frame 1563): APPCRASH c0000005 at the **same address +0x7107c8**.
  - That address lies inside the tractor-beam **Update** 0x710710..0x710880 (ABILITIES_HARDPOINTS §3.2).
  - So the crash belongs to the removal of the Cancel user while the target's tractor beam is **active**, not to
    `Despawn`. It happened twice: once by despawn, once by death [H for the repro and the address].
  - Mechanism [M]: `SAP_Enable_All` runs Restore (vf +0x88), which re-applies effects to their live registered
    targets. Tractor beam is a user-input unit ability, so `SAP_Disable_All` did not end the running beam (styles
    1..5 / 12 only). The restore then re-applies an effect that is still running, and the beam's update reads a stale
    entry.
- **Third run (cu8g t10, 2026-11-08 07:41, run `20261108-073703-abil`): block and restore measured.** The kit now
  replaces the corvette by a fresh one W2 at the start of the cancel phase, so the beam is off when Home One arrives
  and dies. Two trials:
  - **Cancel phase**, 750 frames each (810..1560 and 2238..2988): TRACTOR_BEAM `Is_Ability_Ready` stayed **0** in
    every poll. That is well past the 20 s recharge, which ended at about 1410 / 2838. All 25 activation calls per
    trial returned without activating (active 0). So the Combat_Automatic apply fires when Home One is in combat with
    the Accuser, and the disabled byte refuses the unit ability [H].
  - **Restore:** trial 1: Home One was alive at the poll of frame 1563 (the `Take_Damage` call). At the next poll (1566)
    it was dead and TRACTOR_BEAM was ready again (ready 1). Trial 2: kill at 2991, dead and ready at 2994.
    Activation succeeded on the next try (beam active on W2 by 1572 / 2997).
  - So the manager is re-enabled on the removal frame, within the 3-frame poll [H at that resolution]. This matches
    §1.2: enable on removal, then the re-stagger. No crash: the beam was not running at the removal.
- **Port consequence:** keep the §1.4 rules (block on apply, enable on remove, measured above). The retail crash when
  the user is removed during a running user-input effect on the target is not reproduced [port].
  `AbilitySystem::set_abilities_disabled` (ability_system.cpp) restores only the automatic-style specials that its own
  disable ended (`SpecialState::restore`), and unit abilities such as TRACTOR_BEAM are not in that list. So no code
  change follows.

## 2. t02: engine-loop suppression (0x392410 / 0x393970)

### 2.1 The primitives [H]
- 0x392410(obj, fade): stops `SFXEvent_Engine_Moving_Loop` (type +0x1410), `…_Idle_Loop` (+0x1400) and
  `…_Cinematic_Focus_Loop` (+0x1420) on that object, each with `fade` when fade ≥ 0, else the event's own
  `Loop_Fade_Out_Seconds` (event +0xc0); clears +0x3b5 and +0x3b6; sets **+0x3b9 := 1**.
- 0x393970(obj): +0x3b9 := 0 and nothing else (no loop restarts).
- Other writers of +0x3b9: object init 0x3989a0 clears it **before** the behaviours attach (so a behaviour's
  initialise can set it); 0x472560 copies it (object state copy).

### 2.2 Callers (complete; direct-call and pointer scan) [H]
All are in **`HolsterWeaponBehaviorClass`** (behaviour id 0x4E HOLSTER_WEAPON, vftable 0x8b0f78), plus two
entry points of it called by `Saber_Throw_Ability`. The previous "garrison / crew" guess (STATION_HANGARS_SOUND
§3.4 [L]) is wrong.

| RVA | Role | Calls |
|---|---|---|
| 0x6355c0 | vf9 Initialise | suppress(obj, **0**) |
| 0x635180 | vf13 "placed" broadcast (map load / campaign restore, MOVEMENT §LAND_OBSTACLE) | suppress(obj, 0) |
| 0x635750 | vf6 Service (interval 1) | suppress(obj, **−1**) when a holster transition starts and `Holster_Disable_Engine_Loops` |
| 0x635950 | interpolation step, called from Service every frame | **unsuppress** when a draw transition completes and `Holster_Disable_Engine_Loops` |
| 0x635450 | "snap holstered", called by Saber_Throw update 0x7141f0 | suppress(obj, 0) |
| 0x635310 | "snap drawn", called by Saber_Throw update 0x7141f0 | suppress(obj, 0) |

Type fields (HeroUnit table, xml_field_tables): `Holster_Weapon_Bone_Name` +0x2298, `Holster_Drawn_Bone_Translation`
+0x22b8, `Holster_Holstered_Bone_Translation` +0x22c4, `Holster_Transition_Time_In_Secs` +0x22d0,
`Holster_Minimum_Drawn_Time_In_Secs` +0x22d4, `Holster_Disable_Engine_Loops` +0xe2 (bool), `SFXEvent_Draw_Weapon`
+0x14f0, `SFXEvent_Holster_Weapon` +0x1500.
Behaviour state: +0x40 bone index (−1 = none), +0x44 transition start frame, +0x48 transition end frame,
+0x4c last "engaged" frame, +0x50 drawn (1) / holstered (0).

### 2.3 The holster behaviour [H unless tagged]
- **Who has it (FoC):** `Darth_Vader`, `Darth_Vader_Expansion`, `Obi_Wan_Kenobi`, `Emperor_Palpatine`,
  `Luke_Skywalker_Jedi`, `Yoda`, `UM06_MissionScript_Temp`; all in `LandBehavior` only, all with
  `Holster_Disable_Engine_Loops` Yes, bone `b_saber`. Transition 0.17 s (Yoda 0.2), minimum drawn 15 s (Yoda 0.5).
  Their engine idle / moving loops are the **lightsaber hum** (`Unit_Vader_Lightsaber_Idle_Loop`, …). Land only;
  nothing in space calls suppress.
- **Initialise / placed:** look up the bone on the model (needs a model and the bone; else +0x40 = −1 and the
  behaviour does nothing, including no suppression). With the bone: snap it to the holstered translation, suppress
  with fade 0 (**not** gated by `Holster_Disable_Engine_Loops`), drawn := 0. Because init runs after 0x3989a0
  cleared +0x3b9, a holster hero **starts suppressed**: the mode-enter "idle on" (STATION_HANGARS_SOUND §3.3) is
  skipped and no hum plays until the saber is drawn.
- **Service (every frame):** first the interpolation step (below). Then only when `frame % 15 == 0`:
  - engaged := the object has a current attack target (0x397a40) **or** SAP +0x2cc ≥ 1 (Berserker frames left
    [M meaning; written by the Berserker block 0x712dc0 / 0x712fd0 / 0x713710]) **or** the current animation type
    is 5 (ATTACK) or 0x68 (SPINMOVE) **or** obj +0x3b4 (hero clash) is set.
  - engaged → +0x4c := frame, want drawn. Not engaged → want drawn only while drawn and
    frame < +0x4c + frames(`Holster_Minimum_Drawn_Time_In_Secs`).
  - If want ≠ drawn (and the bone exists): +0x44 := now, +0x48 := now + frames(`Holster_Transition_Time_In_Secs`).
    To drawn: play `SFXEvent_Draw_Weapon` attached, drawn := 1. To holstered: play `SFXEvent_Holster_Weapon`,
    drawn := 0, and if `Holster_Disable_Engine_Loops` suppress with fade −1 (each loop's own fade-out).
  - frames(s) = round-half-away(FPS × s) (0x27c9a0 → 0x22f9c0): 0.17 s → 5, 15 s → 450 at 30 FPS.
- **Interpolation (0x635950, every frame):** runs while a transition is set (+0x44 or +0x48 ≠ 0), the bone
  exists, the model exists and no thrown saber is in flight (SAP null or SAP +0x2e8 == 0x3fffff). Lerps the bone
  between the two translations by t = clamp((now − start) / (end − start), 0, 1). When now ≥ end: +0x44 / +0x48
  := 0, and if drawn and `Holster_Disable_Engine_Loops`: **unsuppress**.
- **After unsuppress** nothing restarts the hum at once: the land locomotor (Walk 0x61e930 etc.) turns idle or
  moving 0 → 1 at its next service (STATION_HANGARS_SOUND §3.3, its moving test [M]).
- **Saber_Throw** (`Luke_Saber_Throw`, update 0x7141f0, land only): at the throw (anim 0x6c past half its frames [M: 0x395550 / 0x395570 read as frame / count]) it spawns
  the saber projectile (SAP +0x2e8 := its id) and calls 0x635450: bone to holstered, suppress(0), drawn := 0. At
  the catch (anim 0x6d, saber back within range) it plays 0x6e and calls 0x635310: bone to drawn, suppress(0),
  drawn := 1. 0x635310 **suppresses although it sets drawn**, so after a catch the hum stays off until a later
  draw transition completes [M: consequence; the Service only starts a draw transition when drawn = 0, so in
  practice the hum returns after the next holster → draw cycle, or after a draw transition that the Service
  started while the saber was in flight completes].

### 2.4 Energy-pool engine disable (0x56c230) [H unless tagged]
- **0x56c230 is `EnergyPool::Absorb`** (POWERED behaviour 0x0F, `EnergyPoolBehaviorClass`). Called once, from
  `Take_Damage` (call at 0x3aab80), in the absorption step after the base-shield (behaviour 0x39) and
  `ShieldBehavior::Absorb` steps, only when damage kind ≠ 22, the victim has POWERED (obj +0x341 ≠ −1) and the
  amount still left is > 0 (TACTICAL_COMBAT §3.6 step 15).
- Absorb returns 0 unless the source object is a projectile (has PROJECTILE, obj +0x33a ≠ −1) whose type has
  `Projectile_Does_Energy_Damage` (+0x46) and the victim has a CD. Otherwise energy E = CD +0xf8; new =
  clamp(E − x, 0, E), stored through 0x56c1b0 (also clamps to the max, `Energy_Capacity` × bonus); returns
  E − new = min(E, x). x is in shield-armor space (before the division back to raw); the caller subtracts it.
- **Engine disable:** right after, if absorbed > 0 **and** the projectile type has
  `Projectile_Disables_Engines_When_Power_Drained` (+0x48):
  1. if the locomotor pack (obj +0xa8) exists: `Locomotor_Set_Engines` 0x5545a0(pack, destroyed 0,
     `Projectile_Disable_Engines_Duration` (+0x4c8) seconds), then, when pack +0x1f0 exists, 0x422040 (stops
     the current move unless its last waypoint has flag 0x10 [M meaning]);
  2. signal 0x16 on the object (0x220ed0) [M: "engines disabled"; the re-enable sends 0x17];
  3. `Set_Engine_Moving_Loop(obj, 0)` then `Set_Engine_Idle_Loop(obj, 0)`.
- "Drained" is **not** "energy reached 0": any positive absorption triggers it. With shields up and a projectile
  that also does shield damage, the shield absorbs first and nothing reaches the pool.
- **Duration** (0x5545a0, HARDPOINT_DISABLED §2): acts only when (engines on (+0xd0) or a timed disable is
  running (+0xd4 ≥ 0)) and not permanently destroyed (+0xd1). Sets +0xd0 := 0 and +0xd4 := max(+0xd4, now +
  int(FPS × t)), so repeated hits extend to the latest expiry, never add up. The locomotor update 0x557ba0
  re-enables at now ≥ +0xd4 (signal 0x17), with no audio call.
- **Shipped users (FoC):** `Proj_Ship_Ion_Cannon_Small` (20 s), `Proj_Ion_Cannon_Planetary` (10 s),
  `Proj_Ship_Krayt_Megaweapon_Ion` (3 s), `Proj_Krayt_Bombardment_Ion` (10 s); victims need POWERED with energy
  > 0 (POWERED is listed by many corvettes, frigates, capitals, fighters and stations).
- **Relation to the hardpoint path:** both end in 0x5545a0. The hardpoint / station-upgrade path (0x3924f0) uses
  destroyed = 1 (permanent) or t < 0 (indefinite) and is already wired as engines_offline. The energy pool is the
  **only timed** caller in vanilla; the port's `TacticalBattle::disable_engines(id, seconds)` (timed disable +
  `EngineLoops::timed_disable`) is its exact counterpart. Wiring: in the combat absorption step, when
  `energy_absorbed > 0` and the source projectile type has `Disables_Engines_When_Power_Drained`, call
  `disable_engines(victim, Projectile_Disable_Engines_Duration)` and stop the current move. The engine loops go
  off at once but the next SimpleSpace locomotor service turns idle back on by speed (§3.3 of
  STATION_HANGARS_SOUND) [M consequence].

### 2.5 What a port needs (t02)
- suppress / unsuppress belong to a land HOLSTER_WEAPON model (lightsaber heroes) with land engine loops; nothing
  to wire for space. Rule: start suppressed; holster start → suppress(own fades); draw complete → unsuppress;
  Saber_Throw throw / catch → suppress(0). The bone motion is presentation, but the 15-frame decision and its
  timers decide the audio requests, so they belong in the sim side of the hum.
- Probe (optional, needs Gabriel): Vader in a land battle, idle 20 s then attack. Expected: no hum while
  holstered; draw SFX at the first 15-frame boundary after a target appears; hum starts within ~5 frames + one
  locomotor service; holster SFX and hum fade-out 450 frames after the last engaged frame. For Saber_Throw:
  whether Luke's hum returns right after the catch (the [M] above). A memory reader would watch obj +0x3b9.
  Run in cu8h t02 (§2.6).

### 2.6 Probe results (cu8h t02, 2026-11-08) [H for what was read]
Claude drove the retail game. Kit: `tools/probe_mod/cu8h_probes/`, sessions `hum` and `luke`.
- **Setup.** A land skirmish on `_MP_LAND_DAGOBAH`, human Empire against the AI Rebel. A free-store script spawned the
  heroes for the idle human player in an open square, all units invulnerable.
- **What was read.** A read-only memory watcher polled each hero every 2 ms (about 16 reads per frame) and logged:
  - obj +0x3b4..+0x3b9;
  - the HOLSTER_WEAPON behaviour's bone / start / end / last-engaged / drawn fields;
  - SAP (obj +0x100) +0x2e8, the thrown-saber id (0x3fffff = none).
- **Frames.** Frames are mode frames. The Lua frames of the orders are the same clock.
- **Vader (`hum`):**
  - Mode frame 311: starts suppressed, +0x3b9 = 1, drawn 0, bone 10.
  - 900: a Rebel trooper spawned and `Attack_Target` ordered.
  - 915: draw transition 915–920 (5 frames), at the first 15-frame boundary after the order.
  - 920: +0x3b9 → 0 at the transition end.
  - 922: idle loop on (+0x3b5); 990: moving loop on (+0x3b6 instead of +0x3b5).
  - Last engaged stamp: every 15 frames until 1200, when the target was removed.
  - **1650 = 1200 + 450:** holster transition 1650–1655. +0x3b9 → 1 and +0x3b6 → 0 at the holster **start**.
  - All as §2.3.
- **Luke, saber thrown while holstered (`luke` run 1; the `hum` run shows the same at 2115 / 2143):**
  - 453: target spawned 150 units away.
  - 462: autofire threw the saber, +0x2e8 := the saber id. Luke was still holstered and +0x3b9 stayed 1.
  - 465: the Service (engaged) started a draw, 465–470. The interpolation then stood still while the saber was in
    flight (start / end stayed set past 470).
  - 497: catch, +0x2e8 back to 0x3fffff.
  - 498: the transition cleared and +0x3b9 → 0. 499: idle loop on.
  - Holster at 2100 = 1650 + 450.
- **Luke, saber thrown while drawn (`luke` run 2):** a second target 120 units away, `Activate_Ability("SABER_THROW")`
  at 1509.
  - 1512: drawn 0, +0x3b9 → 1, +0x3b6 → 0, saber in flight. This is 0x635450's snap + suppress.
  - 1515: the Service (engaged) started a draw, 1515–1520, blocked by the flight.
  - 1555: catch.
  - 1556: transition cleared, +0x3b9 → 0. 1557: idle loop on.
- **Answer to §2.5.**
  - The hum is off for the whole flight. It returns **one frame after the catch** whenever the Service saw the hero
    engaged at a 15-frame boundary during the flight, which is always the case while he has an attack target. The
    held draw transition completes on the first frame without a saber in flight.
  - The §2.3 [M] case, "hum stays off after a catch until a later draw transition", needs a flight with no engaged
    boundary. It was not seen [M].
  - Throw to catch took 35 frames (target at about 150 units), 43 frames (about 120 units, moving) and about 27
    frames (`hum` run).
  - The second autofire throw against the first target never came: after the catch Luke was in melee, inside the
    20-unit minimum range.
- **Port.**
  - `hangar::EngineLoops` matches all of this when it is given `saber_in_flight`.
  - The host passes `false` and never calls `saber_catch`, because there is no thrown-saber phase. The throw snap runs
    at the ability apply.
  - So the port's hum returns at the next boundary + 5 frames after a throw (about 8 frames), where the retail game
    waits for the catch (27–43 frames). This is audio only, outside the CRC (follow-up).
  - cu8i t01 (2026-11-08): the port now holds the draw for a stand-in flight (out to the target and back to `Catch_Dist`
    at the `Saber_Name` type's Max_Speed per frame) and calls the catch, so the hum returns the frame after the catch
    (docs/PHASE4.md cu8i).

## 3. t07: the Disable_Force_Abilities_Range emitter and the space retreat fallback query

### 3.1 Emitter behaviour [H]
- `DisableForceAbilitiesBehaviorClass` (behaviour id 0x52 DISABLE_FORCE_ABILITIES, vftable 0x8b17c0): vf4 serviced
  = 1, **vf5 interval = 1** (0x18ceb0), vf6 Service 0x636260. Service re-arms itself with now + 1 (0x4c2f70).
  So the update runs **every frame** while the object is serviced; no XML timer, no RNG.
- **Only objects that carry the behaviour emit.** Range is type +0x53c (`Disable_Force_Abilities_Range`, read as a
  float), read only by this Service. FoC data:

| Type | Behaviour list | Range | Effect |
|---|---|---|---|
| `Ysalamiri_Tree` | `LandBehavior … DISABLE_FORCE_ABILITIES` | 150 | land only |
| `Underworld_Ysalamiri_Cage` | `LandBehavior … DISABLE_FORCE_ABILITIES` | 250 | land only |
| `Rebel_Mobile_Shield_Generator` | no DISABLE_FORCE_ABILITIES in any list | 150 | **none** (the tag is dead data) |

- **Query:** GOM 0x2a9ff0 on the active mode's GOM with behaviour filter **0x02 SELECTABLE** (the SELECTABLE
  behaviour list), all players, **owner ≠ the emitter's owner** (allies and neutral included; no hostility
  test), centre = emitter position, radius r (query keeps 3-D distance² < r², no cap on the count). This corrects
  TACTICAL_GAPS §2.3.1a, which read the first argument as "mode 2 [M: other owners]".
- **Per hit:** skip when obj +0x3a0 has 0x40, 0x80 or 0x02, when it is a projectile (+0x33a ≠ −1), has no model
  (+0x2a0) or no type, or 3-D distance² > r². Then for t = 0..0x4c: the object has unit ability t (0x3986b0) and
  the info table's F column is set for t (0x43fa30) → CD +0x110 := the global frame counter (0x140b0a320).
  No CD → no stamp.
- **Clearing:** nothing ever resets CD +0x110 (it starts at −1). The lockout ends **passively**: the test 0x380b10
  is elapsed = (frame − stamp) / FPS, elapsed < 0.1 counted as 0, locked while elapsed ≤
  `Force_Ability_Disable_Time` (GameConstants +0x11824 / 0x140b27024; shipped 0.0). With 0.0 at 30 FPS the
  object is locked on the stamp frame and the 2 frames after it; on the **3rd frame after the last in-range
  service** its Force abilities work again. With a mod value T > 0 the lockout lasts T seconds after leaving.
  Entering the range locks from the first emitter service of that frame.
- Port differences to fix when wiring: the port (`TacticalBattle::l1_force_disable`) stamps for any type with a
  range > 0 in any mode, which wrongly makes `Rebel_Mobile_Shield_Generator` an emitter and would let emitters
  work in space; it should require the DISABLE_FORCE_ABILITIES behaviour in the object's mode list (land only in
  FoC) and SELECTABLE on the victim. The every-frame interval is correct.
- Probe: not needed for the interval. Optional confirmation of the dead tag: Vader next to an enemy Rebel mobile
  shield generator can still use Force powers; next to a Ysalamiri tree he cannot.

### 3.2 Space retreat fallback object query [H]
In the space retreat-permission test 0x282750 (called by `SpaceRetreatCoordinatorClass` vf9 0x5334e0 and by the
HUD 0x2ded90), after the interdict / `Retreat_Prevention` list check finds no blocking entry:
- Query 0x2a9ff0 on the active mode's GOM with behaviour filter **0x2C TACTICAL_SUPER_WEAPON**, the **per-player
  list of the requesting player** (third argument = player id), no owner exclusion, position (0,0,0) with
  **radius 0 = no distance test**, no hostility filter, no count cap.
- Any hit → reason **6** (retreat refused); none → **0** (allowed).
- So in space a player **who owns a TACTICAL_SUPER_WEAPON object in the battle cannot retreat**. FoC objects with
  that behaviour: `Death_Star` and `Death_Star_II` (SPACEUNITSSUPERS.XML, `SpaceBehavior`); the many
  `DUMMY_TACTICAL_SUPER_WEAPON_TARGET` props are a different behaviour id and do not count.
- Full order of 0x282750: skirmish / multiplayer game types (0x28b060 / 0x28af80), a non-space mode, or network
  session type 3 → 1; then if the player has tactical data and its interdict list is set (0x35f6d0, an AND over
  the entries), the first entry with value 1 returns its stored code (+0x38); then the query above. The land test
  0x2825e0 has **no** object query. The caller 0x5334e0 reaches 0x282750 only for factions with +0x20a8 set and
  when the story manager (0x140b30350, 0x446480) does not list the player [M meaning of that list].
- Not related to `Weatherproof_Ability`: that writes player +0x410 (TACTICAL_GAPS §7.4), never read by this test.
- Port: add to `retreat_prevented` in space: the player owns a live battle object with TACTICAL_SUPER_WEAPON →
  refused. Whether dead objects are already gone from the per-player list at that point is [M: lists are
  maintained on Detach]; counting only live objects matches.

## 4. RVA index

| RVA | Name |
|---|---|
| 0x37b430 / 0x55d530 | SpecialAbilityManager ctor / SpecialAbilityDataPack ctor |
| 0x37b9e0 / 0x37d7f0 | `SAP_Disable_All` / `SAP_Enable_All` |
| 0x6f3fc0 / 0x6f40c0 / 0x6f4060 | Cancel_Special apply / remove / accepts-target |
| 0x48cdd0 / 0x48e9e0 / 0x48f1e0 | SpecialAbility Cancel (vf16) / Restore (vf17) / Remove_All (vf15) |
| 0x392410 / 0x393970 | engine-loop suppress / unsuppress |
| 0x6355c0 / 0x635180 / 0x635750 / 0x635950 | HolsterWeapon initialise / placed / service / interpolate |
| 0x635310 / 0x635450 | HolsterWeapon snap drawn / snap holstered (Saber_Throw) |
| 0x7141f0 | Saber_Throw update |
| 0x56c230 / 0x56c1b0 | `EnergyPool::Absorb` / energy setter |
| 0x5545a0 / 0x422040 | `Locomotor_Set_Engines` / stop current move [M] |
| 0x636260 | DisableForceAbilities service |
| 0x380b10 | Force-disable lockout test |
| 0x2a9ff0 | GOM object query (behaviour id, player list, excluded owner, position, radius, cap, hostile-to) |
| 0x282750 / 0x2825e0 | space / land retreat-permission test |

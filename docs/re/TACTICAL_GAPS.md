# Tactical gaps: answers to the Phase 4 tactical open items (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe (Ghidra 12.2 export in `re_reference/`, llvm-objdump on
`corruption\StarWarsG.exe` where the decompiler lost arguments). Prose and tables only. Addresses are RVAs
(image base 0x140000000). Confidence: [H] read directly, [M] inferred from strong evidence, [L] guess,
**probe** = needs an in-game run (Gabriel launches; in-game probes are paused).

Data checked: vanilla FoC XML and **Thrawn's Revenge** (TR, workshop 1125571106, `Config.MEG` + loose XML;
the parity target). "FPS" is the global 0x140b0a340 (30). "Synced draw" = the lockstep RNG 0x140a13e24;
"free RNG" = the unsynced stream 0x140a13e20. Inline synced draws use the LCG
s := s × 0x41C64E6D + 0xBDF, value = ((s >> 10) & 0x7FFF) / 32767.0 (so [0, 1] inclusive).

Sources of the questions: `docs/PHASE4.md` Open items (S8, S9, S10, FOW, L1/S4 fog, A1, S1, L1 systems, S7,
R1) and the open-item lists of ABILITIES_HARDPOINTS §8, TACTICAL_COMBAT §8, BATTLE_TRANSITIONS §7,
FOG_OF_WAR §9, OBJECT_RUNTIME §8 and MOVEMENT §8. Those lists now point here.

## 0. Summary

**Port-affecting findings (change code or goldens):**
1. **New synced draws** the port does not make yet:
   - behaviour Initialise: one `Get_Int(0, interval − 1)` per behaviour with interval ≥ 2 (§6.4);
   - SAP init: one `Get_Int(now, now + interval)` per special ability with interval > 0 (§4.3);
   - autofire candidate pick when several abilities qualify (§1.3);
   - spin-away and `Destruction_Survivors` (§1.9);
   - melee / earthquake / telekinesis / vehicle-thief / whirlwind chance rolls on every apply attempt (§2.3);
   - mine scatter (§2.3.3), Confuse destinations (§2.2), HUNT decisions (§2.2);
   - retreat attrition, one per retreating unit even when the factor is 0 (§4.9).

> **Status 2026-10-30:** Every draw in this list is now made: behaviour Initialise (`engine/objects/src/object_store.cpp:70-80`), SAP init (`engine/tactical/abilities/src/ability_behaviors.cpp:125-133`), the autofire pick (`ability_behaviors.cpp:230-231`), spin-away (`engine/tactical/combat/src/combat_system.cpp:1595-1598`), the chance rolls (`engine/tactical/abilities/src/ability_classes.cpp:116-120`), proximity-mine angle and radius (`ability_classes.cpp:160-166`), Confuse (`ability_behaviors.cpp:847`), HUNT decisions (`ability_behaviors.cpp:779`, `hunt_decide`, bounds partly [M]) and retreat attrition (`engine/apps/sim/src/tactical_battle_bt.cpp:381-386`, applied at `engine/apps/sim/src/tactical_battle.cpp:111`). The one scatter still not modelled is the ClusterBomb bombs, which all spawn at the user (`ability_classes.cpp:446`).

2. **Not synced** in the original: TSW death-explosion pick (§7.1) and the random tactical map pick (§4.10).
3. **Targeting FOW test uses force = 1** (§5.8): fogged TEAM / HIDE_WHEN_FOGGED enemies are untargetable
   for AI players too. obj +0x343 = MARKER (bombing-run markers skip the FOW test).

> **Status 2026-10-30:** force = 1 is done: the targeting test calls `target_hidden(a.owner, t.id, true)` in `engine/tactical/combat/src/combat_system.cpp:690-692`. Only the MARKER exemption is missing (no marker objects exist yet; noted in the same comment).

4. **Land battle scale is applied the wrong way round** in the port (§5.3).

> **Status 2026-10-30:** Fixed. `battle_scale_sides` in `engine/tactical/fog/src/fog_of_war.cpp:22-30` sums the attacker's modifiers and scales the defender's radii for land, and the reverse for space.

5. **Attacker flash** comes from firing, not from damage (§5.1).
6. **`Requires_Base_Power` does not pass by default** (§7.3): TR towers and shields go dead without a
   power generator.
7. **HUNT/LURE active = slot byte** (M = 1), not the behaviour (§1.1).
8. **GameConstants values TR changes:** `Health_Low_Percent_Threshold` 0.50 and
   `Hardpoint_Recharge_Cutoff_For_Opportunity_Fire` 5.0 (§3.2). Read them from data.
9. **A1 land edge weight** = 3-D zone-centre distance / 10 plus a threat node cost (§6.1).

> **Status 2026-10-30:** Done: `land_zone_hop` and `land_zone_edge_weight` in `engine/ai/tactical/src/zone_reachability.cpp:26-38`.

10. **Missile-shield emitters register at spawn**, not on activation. Jammers protect **friendly** units
    from homing missiles (§1.5, §1.7).

**Corrections to other docs** (applied in place where the doc is one of the six listed above; the
others are listed here for their owners):

| Doc | Correction | Here |
|---|---|---|
| ABILITIES_HARDPOINTS §2.1 | HUNT and LURE have M = 1; AVOID_DANGER has B = −1 | §1.1 |
| ABILITIES_HARDPOINTS §2.5 | MISSILE_SHIELD users are emitters from spawn; activation does not register | §1.7 |
| ABILITIES_HARDPOINTS §3.8 | the jammer stamps objects **not hostile** to its owner | §1.5 |
| ABILITIES_HARDPOINTS §3.3 | 0x48eb10 is the galactic hero-respawn scheduler, not a recharge | §1.11 |
| TACTICAL_COMBAT §2.3 step 4 / FOG_OF_WAR §5.3 | targeting FOW test force = 1; +0x343 = MARKER | §5.8 |
| TACTICAL_COMBAT §3.6 step 18 | the 75 % / 50 % crossing attaches damage smoke; there is no space model swap | §6.3 |
| BATTLE_TRANSITIONS §2.3 rule 1 | bit 0x2c in the space bitset = TACTICAL_SUPER_WEAPON (super-weapon always starts) | §4.6 |
| OBJECT_RUNTIME §9.1 | "computed at attach" intervals; first due frame draws | §6.4 |
| ANIM_PARTICLES §A row 242 | "garrison default idle" is presence-induced idle; crouch = SPREAD_OUT | §6.2 |
| SCRIPT_GAPS §1.2 | TSW explosion pick uses the free RNG | §7.1 |

---

## 1. Unit-ability framework (ABILITIES_HARDPOINTS §8 items 1–12, 14)

### 1.1 Info-table defaults [H]
- A static initializer at 0x18750 fills all 0x4d entries of the table 0x140b2f2e0 before 0x43fab0 runs:
  M = 1, B = −1, +0x28 = 1, X = 1, U = 0, +0x2b = 1, F = 0, empty icon. The assumed default is correct.
- Rows 0x43fab0 sets only partly: HUNT (6) B = 0x21; LURE (10) B = 0x23; LUCKY_SHOT (34) U = 1;
  ION_CANNON_SHOT (41) U = 1. HARASS, AFTERBURNER, AVOID_DANGER, SPRINT, SPOILER_LOCK, MAXIMUM_FIREPOWER,
  EJECT_VEHICLE_THIEF and STIM_PACK keep M1 B−1 X1 U0 F0.
- **HUNT and LURE have M = 1** (the §2.1 table shows "d", which reads as 1). The behaviour branch of
  `Is_Unit_Ability_Active` (0x39b950) needs B ≠ −1 **and** M = 0, so it applies only to DEPLOY_TROOPERS.
  For HUNT/LURE "active" is the slot byte, written by the M = 1 tail of 0x38a350 after handler 0x3a4070
  toggled the behaviour. The S9 port choice "HUNT/LURE taken as M = 0" should become slot-byte semantics.
- AVOID_DANGER has B = −1, so 0x3a4070 has nothing to toggle for it (no shipped data uses it).
- **Order cancel 0x38f5d0** runs on every new move / attack / escort order (0x2a8240, 0x57da90, 0x5730f0,
  0x3b0040): it switches off BARRAGE, LUCKY_SHOT, ION_CANNON_SHOT (on the team container) and HUNT when
  active, then services the garrison (0x51) behaviour. A new order is the normal way HUNT ends.

### 1.2 `Active_By_Default` [H]
- 0x3a82a0 checks the type's **first two** `Unit_Abilities_Data` entries (0x374b50(type, 0/1)); for each
  record with +0x1c set it calls `Set_Unit_Ability_Active(obj, t, on, pos 0, owner, no target)`. Skipped when
  obj +0x3a0 bit 0x02 is set.
- Callers: creation 0x29f810 (object not dying and not a team container); team/company spawn 0x2a5a60;
  `AIFreeStoreClass` vf1 0x4d4aa0 (**corrected, A1_READINGS §28.8**: only the signal 10/11 branch - a unit a script
  locked with `Lock_Current_Orders` finished its order - after releasing that lock with 0x3a8e50(obj, 0); admission
  does not call it); `TaskForceClass` vf49 0x5ff610 (unit leaves a task force and its movement is not locked,
  0x39ad40 - not a HUNT test).
- So the AI layer re-applies default modes: the Interdictor's gravity well, REPLENISH_WINGMEN and TR's 17
  INTERDICT users come back on after task-force release.

### 1.3 The autofire driver [H]
Autofire acts **inside the sim**, in `Issue_Auto_Target` (0x38d730), which runs for targeting-acquired
targets; the S9 doc's "GUI only" reading is wrong.
- **Candidates** (0x37d280, when the call carries no explicit ability): the type's `Abilities` (+0x1a98) in
  list order, each needing (1) `Activation_Style` 9 Special_Attack, or 11 User_Input **and** autofire on
  (0x48e240 → 0x48d5e0); (2) its unit-ability countdown ready (0x48d5a0 → 0x42f820); (3) allowed in the
  mode (0x37c270), enabled (0x37be10), accepts the target (vfunc +0x28) and the black-market gate (0x39b0b0).
- **Pick:** one candidate → it; n ≥ 2 → **one synced `Get_Int(0, n − 1)`**. The pick becomes the pending
  ability (targeting vfunc +0x130) and fires through the pending mechanism (ABILITIES §2.9).
- **Fallback:** no candidate → 0x37c9a0 tries an immediate style-9 trigger (0x37b580(9, …)); success
  aborts the attack order. `TargetingBehaviorClass` vf31 0x568c80 also calls 0x37d280.
- **"Autofire on" (0x48d5e0):** if the object's SAP data pack (obj +0x100) has an entry for the ability
  name CRC in its map (+0xa0; bool records from the autofire setter), that value; otherwise **true when the
  owner is AI-controlled (0x2814d0) and not human (player +0x62 = 0)**. A global game-state gate
  (0x28af60 / 0x2cd0c0, debug flag 0x140b1574e) and the black-market gate apply first [M on the gate].
- **Setter 0x3a78b0** (command 58 and the Lua setters): writes the slot byte +9 **and** the +0x100 map
  entry; refuses abilities without `Supports_Autofire` unless the owner is AI and non-human.
- Slot byte +9 is read only by 0x39b310 (command-bar GUI 0x2e7fd0, 0x2f6410, 0x30e660, 0x30edf0, 0x3108b0,
  0x31a450, 0x31b590; Lua `Is_Ability_Autofire`).
- **Determinism:** human state arrives only through synced commands; AI autofire depends only on player
  flags. AI players autofire every User_Input special ability; humans only what they toggled.

### 1.4 Special-ability service intervals (vfunc +0x18) [H]
The SAP update 0x37da80 updates only classes with interval > 0.

| Interval (frames) | Classes |
|---|---|
| 0 (never updated) | Combat_Bonus, Battlefield_Modifier, Enhance_Defense, Redirect_Blaster, Absorb_Blaster, Enable_Ability, Cluster_Bomb, Vehicle_Thief, Permanent_Weapon_Swap, Base_Power, Base_Destruction, Cancel_Special, all galactic economy / corruption / political classes |
| 1 | Berserker, Stealth, Tactical_Bribe, Infection, Arc_Sweep, Cable, Concentrate_Fire, Eat_Attack, Energy_Weapon, Force_Healing, Force_Lightning, Force_Whirlwind, Generic_Attack, Grenade_Attack, Hack, Ion_Cannon_Shot, Personal_Flame_Thrower, Lucky_Shot, Maximum_Firepower, Repair, Tractor_Beam, Stun, Radioactive_Contaminate, Force_Sight, Saber_Throw, Force_Cloak, Super_Laser, Laser_Defense, Force_Confuse, Leech_Shields, Sensor_Jamming, Remote_Bomb, Proximity_Mines, Drain_Life, Blast, Buzz_Droids, Shield_Flare, Summon, Corrupt_Systems, Spawn |
| FPS / 10 = 3 (integer divide, 0x6f74e0) | Earthquake_Attack, Force_Telekinesis |
| 2 / 30 / computed | Starbase_Upgrade 2 (**skirmish tactical**, not galactic: STATION_HANGARS_SOUND §1.1); galactic only: Find_Weakness int(FPS + 0.5); Detect_Hero / Planet_Income_Gambling int(FPS × +0x178 + 0.5) (0 when ≤ 0); Income_Stream_Mod 0 when +0x199 |

Energy_Weapon therefore deals `Damage_Per_Frame` once per frame. The first due frame of each interval > 0
ability is staggered by a synced draw at SAP init (§4.3).

### 1.5 Readers of tractored, interdict, jammed and `Target_Damage_Increase_Percent` [H]
- **Tractored (CD +0x132):** besides the tractor update 0x710710, read only by Lua
  `Is_Under_Effects_Of_Ability` (0x57c600), which supports TRACTOR_BEAM (CD +0x132) and ION_CANNON_SHOT
  (ION_STUN behaviour 0x45 active) and logs "not yet supported" for any other name. **No sim reader**: the
  flag does not block hyperspace or retreat.
- **Interdict list** (player tactical data +0x180, via mode vfunc +0x220(player id)):
  - Insert (0x3a2240): space mode only, and only when the retreat coordinator (mode +0x30) vfunc +0x30
    allows. Into every player 0x294910 returns (player-list order, not the owner, relation test 0x2824d0
    [M: enemies]): bool 1, permanent (0x35cd20(−1.0)), name CRC = owner type name, source = owner + "INTERDICT".
  - Readers: the retreat-permission tests 0x282750 (space; `SpaceRetreatCoordinatorClass` vf9 0x5334e0 and
    GUI 0x2ded90) and 0x2825e0 (land; `LandRetreatCoordinatorClass` vf9). The first entry with value 1 makes
    the test return that entry's +0x38 instead of 1 ("allowed") [M: a blocking reason code]. The space test
    falls back to an object query (0x2a9ff0, category 0x2c): any hit → 6, else 0 [L meaning].
  - `Retreat_Prevention_Ability` (FoC 1, TR 1) uses the same list [M].
- **Jammed (CD +0x194):**
  - The jammer update 0x719620 stamps objects **not hostile** to the jammer's owner (0x39a450 → 0x2824f0
    false): its own side and allies. Other filters: not attached (+0x33a), has a locomotor, not dying, no
    0x80 / 0x02 flag, has a CD, 3-D distance ≤ radius.
  - The projectile dispatcher 0x4ce6a0 (MISSILE branch) tests `Is_Jammed` on the projectile's **target**
    (projectile data +0x18; disassembly 0x1404ce768): no target or a jammed target → straight flight. The
    ROCKET / MPTL path skips jammed objects in its passive-shield query (0x1404ce8e0).
  - **Net effect: a jammer protects friendly units in its radius from homing missiles.**
  - 0x35b4d0 (`GameModeClass` vf88) is the local radar/minimap blip filter (presentation): it hides the
    local player's jammed or stealthed enemies.
- **`Target_Damage_Increase_Percent` (+0x178):** read only by galactic Find_Weakness (0x6fb360 / 0x6fb740)
  and Detect_Hero / Gambling. Concentrate_Fire never reads it — unused tactically.

### 1.6 Converted-decay bounds [H, disassembly 0x1403a75f2–0x1403a7687]
d = max tactical health (0x3727a0) / `Converted_To_Enemy_Die_Time_Seconds` (type +0x234c) / FPS, in float.
If d > 0: **one synced `Get_Float(−d/10, +d/10)`** (divisor 10.0 at 0x140805898). If d + draw > 0:
`Apply_Hull_Damage(kind 0x11, d + draw)` (0x3ab890). Only with +0x3a2 bit 4 and not dying, once per object
update. Shipped: 60 s (15 entries) and 45 s (6), FoC and TR.
Aside (L1): the same function draws one synced `Get_Float(0, ServiceRate)` when an object's Lua script first
gets a LastService value (0x1403a7470, negated) — a synced draw at script start.

### 1.7 Missile-shield emitter registration [H]
- `Add_Emitter` 0x4cce30 appends to the list 0x140a2bb98 (no duplicate check). Callers: creation 0x29f810
  (once when either of the first two `Unit_Abilities_Data` entries is MISSILE_SHIELD 32, once more when
  `Passive_Missile_Shield_Radius` +0x23e8 > 0; a passive emitter also gets animation state 7); the post-load
  pass 0x3a0930 (same rules); `Sensor_Jamming` apply 0x719000 (every apply).
- **Activating MISSILE_SHIELD does not register**; users are listed from spawn and the per-projectile test
  checks "active". No other writer exists.
- Removal: `Die` → 0x4d07e0 removes **one** entry. A unit registered twice can leave a stale entry; keep a
  multiset and remove one per call.

### 1.8 BurningBehavior [H]
Serviced, **interval 30** (vtable +0x28 = 0x1e). Each service (0x6241b0) does one
`Take_Damage(kind 7, Burning_Damage_Per_Second)` (type +0x2044), i.e. the per-second value once a second.
No expiry in the service: burning lasts until death [M]. First due frame per §6.4 (a synced draw).

### 1.9 Spin-away and `Destruction_Survivors` [H]
- **Spin-away** is decided in `DeathBehaviorClass` Initialise (vf9 0x58cdb0), not in `Die`: with
  `Spin_Away_On_Death` (+0x9a) **one synced `Get_Float(0, 1)`**, spin away when draw ≤
  `Spin_Away_On_Death_Chance` (+0x2208); stored at death data +0x34. The death animation
  (`Specific_Death_Anim_Type/Index` +0xd98/+0xd9c) starts there; without `Remove_Upon_Death` (+0x9b) the
  animation result counts as success; animation failed and no spin-away → removed at once (0x391310(obj, 1)).
  0x3ac290 runs when type +0xed0 > 0. "DEFAULT_UNIT_DIE" is a local-visibility presentation event.
- **Survivors** are spawned in world removal 0x391310 (`Die` step 7) when +0x3a0 bit 0x80 is clear:
  1. `Tactical_Respawn_Time_In_Secs` (+0x21d0) > 0 → delayed creation of the same type at now + FPS × T
     (0x2bd230); owner = object owner, or for capture-point objects the player from 0x331cc0.
  2. `Destruction_Survivors` (+0x1d30, list of (type, cumulative weight)) non-empty → count = **synced
     `Get_Int(Minimum_Destruction_Survivor_Count +0x1d48, Maximum +0x1d4c)`**.
  3. Per survivor (0x378cb0) **two inline synced draws**: u ∈ [0, 1], spawn only when u ≥ 1 − total
     weight; v ∈ [0, total], first entry with cumulative weight ≥ v (binary search 0x378b70).
  4. Owner: the dying object's owner if its faction may own the type (0x3751a0), else the first player in
     list order whose faction can. Spawned through 0x29ef20 at the object position (**one id each**).
  Bit 0x80 guards a second run [M]. Shipped: FoC 41, TR 26 entries (crew/pilots, weight 1.0).

### 1.10 Detonation effects (0x4cb070) and the land stun path [H dispatch, M effects]
Per detonation, in order: surface FX (presentation); area damage when `Projectile_Blast_Area_Damage` and
`_Range` (+0x47c, +0x480) > 0 (0x4c7f70); stun (+0x6c) → 0x4d01c0; distract (+0x6e) → 0x4ca930; instant
heal (+0x6d) → 0x4cb6f0; convert (+0x6f) → 0x4ca020; invulnerability (+0x71) → 0x4cc010; ion stun (+0xab,
radius > 0) → 0x4cbbb0; combat mod (`Projectile_Combat_Mod` +0x2430) → 0x4c6950. Weaken (+0x70) runs from
the detonate paths 0x4c7050 / 0x4caea0 into 0x4d0800. Victim queries are per-player GOM queries (0x2a8fb0)
around the projectile, players in list order (port: ascending id within a player).

| Effect | Rule |
|---|---|
| **Stun** (land only) | `Projectile_Stun_Radius` (+0x1e90) > 0 and `_Duration_Frames` (+0x1e8c) ≠ 0. Each enemy player (0x294910), each object in radius with STUNNABLE (0x38) passing `_Victims_Category_Mask` / `_Unit_Types` (0x39f300) → stun (0x647290). Stealthed objects in radius lose stealth (0.3 s). `Destroy_When_Stunned_Over_Water` (+0xe6) objects more than 0.5 below the scene water height take `Take_Damage(kind 1, 999999)` |
| Stun apply 0x647290 | obj +0x3a0 \|= 0x10 (weapons disabled) and behaviours 1 (locomotor), 9 (targeting), 0xB (turret), 0x35 suspended (vfunc +0xc0, table 0x8b38f0); STUNNABLE +0x30 := now + frames (plain assignment, not extended), so its next service is the release; breaks stealth; attaches `Spawn_Effect` / `Stunned_Effect` |
| Instant heal | owner and allies (0x294750), radius +0x1ed0, `CategoryMask` & +0x1ee8; health := min(max, health + int `Health_Increase` +0x1ef0) |
| Convert | enemies in radius +0x1f10 matching +0x1f28 (0x395c10): victim and its team members +0x3a2 \|= 4 (drives §1.6 decay), owner := projectile owner (0x38e5a0), store / parent notifications (0x2c0340, 0x2b2cc0) |
| Invulnerability | projectile target first, then owner and allies within +0x1f34, up to `Max_Targets` +0x1f38, matching +0x1f50: attach INVULNERABLE (0x4b) if missing, duration +0x1f30 frames (0x630d60) |
| Combat mod | enemies within `Projectile_Blast_Area_Range` matching +0x2450: append-insert (0x536a70) modifier +0x2430 (code 0x52, §6.5), value +0x2438, duration +0x2434 |
| Weaken | enemies within +0x1f5c matching +0x1f78: two replace-by-source inserts (0x3fe810) of −`Take_Damage_Increase_Percent` and −`Cause_Damage_Reduction_Percent`, duration +0x1f58 s [L: which two lists] |
| Distract | enemies within +0x1f74 matching +0x1f08 whose targeting is distractable (0x397a40) get an order toward the detonation (0x2a7b80) [M] |

TR exercises these through the instant-effect unit abilities (§2.1): AREA_EFFECT_STUN 25, AREA_EFFECT_HEAL 11,
WEAKEN_ENEMY 17, HARMONIC_BOMB 46, FOW_REVEAL_PING 146, UNTARGETED_STICKY_BOMB 33.

### 1.11 Recharge of ION_CANNON_SHOT and the forwarded attacks [H]
- **0x48eb10 is not a recharge**: it is the galactic hero-respawn scheduler, called by 0x37b8a0 after a
  successful apply only for abilities with flag +0x41.
- **ION_CANNON_SHOT:** the one-volley off (0x3a06a0 → 0x38a350(41, off)) starts nothing (type 41 has no
  handler). Remove 0x705250 (next update finds 41 inactive) starts the recharge (0x3a9840 → 0x42fb70) iff the
  owner is a team container and some member fired (k < n). **A non-team owner never recharges** on this
  path. Every FoC user is a squadron/container; TR has no ION_CANNON_SHOT users, so the case does not occur.
- **LUCKY_SHOT:** remove 0x7069e0 always starts the recharge.
- **Recharge started at apply** (0x3a9840): Tactical_Bribe, Infection, Cable, Force_Telekinesis, Hack,
  Personal_Flame_Thrower, Repair, Radioactive_Contaminate, Berserker, Force_Sight (in update), Saber_Throw,
  Laser_Defense, Leech_Shields, Cluster_Bomb, Remote_Bomb, Proximity_Mines, Drain_Life, Blast, Buzz_Droids,
  Shield_Flare, Summon, Corrupt_Systems, plus Lightning, Whirlwind, Stun, Confuse, Grenade (§2.3).
- **Recharge at their end** via 0x42fb70 (0x6fa120 / 0x700a70 / 0x707390 / 0x715870): Energy_Weapon,
  Grenade, Maximum_Firepower, Super_Laser. Tractor and Concentrate_Fire use the early deactivate (§2.6).

### 1.12 Hardpoint repair (command 45) [H]
- **Execute** (`RepairHardpointEventClass` vf4 0x4308b0): object id → hardpoint index (bounded by obj
  +0x2d0, 0x396cb0) → 0x387d40(hp, on, event player). No ownership or credit check at command time; the
  command only **adds** a repairer (no cancel). 0x387d40 keeps a per-hardpoint repairer list (hp +0x78,
  player ids, no duplicates) and flag hp +0x6f = list non-empty.
- **Per frame** (0x387170, from hardpoint update 0x387010):
  1. Empty list → flag cleared.
  2. Hardpoint type `Is_Destroyable` (+0x4d) and health ≤ 0 → list cleared (a destroyed destroyable
     hardpoint cannot be repaired).
  3. Each repairer in list order: gone → dropped. Credits (float, player +0x70) ≥ `Repair_Cost_Per_Frame`
     (hp type +0x470) → deduct (0x27f370), health := clamp(health + `Repair_Amount_Per_Frame` (+0x474), 0, max);
     if the parent's hull fraction (0x396df0) is now below the hardpoint fraction (0x396070), parent health ×=
     (hp fraction − hull fraction + 1). Cannot pay → removed, pays nothing.
  4. At max: list cleared, +0x6e / +0x6f := 0, `Set_System_Online(hp, 1, 0)` (0x384d00).
- Several repairers each pay and each add per frame. No refund. Shipped: FoC cost 1.0–1.5 / amount 0.5–2.0
  per frame; TR mostly 1.0 / 2.0 (83 entries).

---

## 2. Special-ability classes and unit-ability handlers (ABILITIES_HARDPOINTS §8 item 11; S9 "unhandled types")

Usage counts are `<…_Ability>` / `<Unit_Ability>` elements (FoC / TR). Vftable slots: 3 +0x18 interval,
4 +0x20 update, 5 +0x28 accepts-target, 30 apply, 31 remove. Field offsets from the XML registration tables
in .data (docs/re/xml_field_tables.md).

**Shared rules [H]**
- **Accepts-target templates:** 0x6eef00 (Arc_Sweep, Concentrate_Fire, Generic_Attack): enemy (0x39a450),
  applicable filter 0x48e290, layer test when either side has +0x3b4, has a model. 0x712770 (Proximity_Mines,
  Buzz_Droids, Corrupt_Systems, Summon, Radioactive): a target carrying MARKER (+0x343, i.e. the dummy point
  object). 0x592e30 (stealth family, Whirlwind, Confuse, Sensor_Jamming, Laser_Defense, Cluster_Bomb,
  Shield_Flare, Blast, Force_Sight): self or no target. 0x6eff20: no target (passive / galactic).
- **Chance rolls** use the inline synced draw 0x5324d0 ([0, 1] inclusive). Direction differs: proceed when
  draw ≤ chance for Arc_Sweep, Generic_Attack, Eat_Attack; draw < chance for Earthquake, Telekinesis,
  Vehicle_Thief, Whirlwind (a chance of 1.0 fails on a draw of exactly 1.0). A chance of 0 refuses without
  drawing (melee classes). **The roll happens on every apply attempt that passed the range test, including
  each pending retry at the targeting cadence.**
- **Direct command skip:** Whirlwind, Stun and Confuse skip the auto-use pre-check (range band, victim scan,
  chance draw) when the pending context flag (+0x2e → +0x11) or a null target says the player ordered it [M].
- **Per-object state:** several classes keep state in the object's SAP data pack (obj +0x100), not per
  ability; Force_Healing's due frame (+0x25c) is shared by every healing ability on the object.

### 2.1 Unit-ability handler bodies [H unless tagged]

| Handler | Behaviour |
|---|---|
| **DEPLOY 0x3a1ce0** [H flow, M names] | Gates: Force block, garrison count, no other mode active, all team members pass `Can_Activate`, locomotor not busy. The unit stops (0x426d50 + 0x39bcb0). on → deploy 0x391260 (adds `DeployedBehavior`); off → undeploy 0x3ac100(obj, 0). Gates failing: on returns false; off still notifies targeting (vf +0xd8) and undeploys. No countdown; `Is_Active` = deployed state |
| **JET_PACK 0x3a38d0** | Same gates. 2-D distance to the point ≤ record `Effective_Radius` (+0x20; skipped when negative). Then, for a `Uses_Multiple_Locomotors` (type +0x74) unit, the locomotor swap to the secondary (0x3a9910; not a garrison release, MOVEMENT §3.1.1). Issues a **move** (0x2a7b80) of the unit / team to the point; the jump itself is the JETPACK locomotor (REMAINING_GAPS §4.3). Countdown: start recharge when `Expiration_Seconds` = 0, else start active. No RNG |
| **BARRAGE 0x3a1930** | Needs BARRAGE behaviour (0x1d) and data (obj +0x178). on, none running (data +0xc = 0): **refused when the point is fogged for the owner** (`FOW_Point_Test` force 1); otherwise start at the point (0x5920b0). on while running → true (re-target exception). off: early-deactivate if active and not recharging, stop (0x592420) |
| **SPREAD_OUT 0x3a3500** | Team gates. on → start active (0x42fde0), off → early-deactivate (0x42f320). Every member: +0x448 := on, reset to default idle (§6.2 → Idle Crouch); re-layout the formation (0x422040) and clear container locomotor +0x2a4. Spacing in team slot geometry not traced [M]. Effects: `Mod_Multiplier` SPEED 0.5, TAKE_DAMAGE 0.5 |
| **ROCKET_ATTACK 0x3a3260** | Gates; writes slot 11's active byte (+0x488) and starts / early-deactivates the countdown. The rocket-firing reader is the weapon path [M, not traced] |
| **FORCE_WHIRLWIND 0x3a1fb0** | Gates; forwards to the special ability whose name CRC = record +0x88 (0x37b8a0) |
| **REPLENISH_WINGMEN 0x3a2970** | If the squadron has fewer members than the type list (+0x978/+0x980), spawn the missing ones (**one id each**) at the formation slot offsets (0x3742c0) around the leader, re-creating the container if gone; re-layout; `Particle_Effect` (+0x60) on each member; register with the store lists (0x2a6430 / 0x2bd360); start recharge 44. No RNG |
| **Instant effect 0x3a3e20** (19, 23–26, 28, 35, 36, 42; also SELF_DESTRUCT 39) | `Can_Activate(t, 1)` (skipped for 39) and `Has_Ability`. Creates `Spawned_Object_Type` (record +0x98) at the point (unit position when the point is (0, 0, 0)), unit facing and owner (**one id**). Projectile: source := user, +0x68 := type +0x440, target id when given. BOMB (0x34): armed with `Bomb_Countdown_Seconds` (+0xa4). Start recharge (not for 39). **The gameplay effect is the spawned object's detonation** (§1.10) |
| STICKY_BOMB 22 / PLACE_REMOTE_BOMB 64 / PROXIMITY_MINES 67 | no handler; the forwarded classes do the work (Grenade_Attack, Remote_Bomb, Proximity_Mines) |
| EJECT_VEHICLE_THIEF 46 (inline) | not read [L] (FoC 18 / TR 107 users; pairs with Vehicle_Thief §2.3.10) |

### 2.2 Behaviours

**HUNT (0x21, `HuntBehaviorClass`; TR 743 users)** [H flow, M bounds, probe for the draw count]
- Interval (0x621840, shared with INDIGENOUS_UNIT / GUARD_SPAWN_HOUSE): **30, or 1 while burning** (obj +0x3a1
  bit 2). Starts disabled; HUNT on enables it (§6.6).
- Service 0x628b40: team members other than the leader do nothing (the container acts). Decides only when
  idle: not moving, idle animation (0x27b5e0) or locomotor state 0x1c / 0x16, formation ≤ 1 member, no attack
  target (0x397a40), 0x39b000 false.
- Destination 0x627d40 (map rectangle mode vf +0x1f8, or playable box 0x2cea10; zero rectangle = stay):
  1. u = synced `Get_Float(0, 1)`. If type flag +0x15 is clear and u > 0.5 (0x140a442e4): 99 random points
     (2 `Get_Float` each; 0x140a442e0 = 100, loop i < 99); keep those **fogged** for the owner
     (`FOW_Point_Test` force 0); any → `Get_Int(0, n − 1)`.
  2. Otherwise all objects (0x2a9ff0 mode 1): collect those **hidden** from the owner (`FOW_Object_Hidden`
     force 0); a +0x15 hunter also collects +0x15 types. `Get_Int` pick (preferring the +0x15 list, then
     hidden, then any object); its position is the destination.
  3. No objects: one random point (2 draws); used if its distance is in [500, 1000] (0x140a442dc /
     0x140a442d8), else moved along that direction by a further `Get_Float` (bounds lost [M]); clamped into
     the rectangle shrunk by half the reveal range (land `Land_FOW_Reveal_Range` × weather, space
     `Space_FOW_Reveal_Range`).
- Jitter: type +0x15 clear → **2 more `Get_Float(30, 400)`** added to the destination [M: x and y].
- Order: move for the unit or its whole team (0x628820 → 0x2a7b80).
- Determinism: up to 1 + 198 + 1 + 2 synced draws per decision; candidate order from GOM / spatial queries —
  use the canonical id order (S6). With `AIUsesFogOfWar` off, `FOW_*` force 0 returns "not fogged" for AI
  owners, so AI hunters fall through to branch 2 with an empty hidden list [M].

**BARRAGE (0x1d, `BarrageAreaBehaviorClass`)** [H]
- Interval 30 (0x436050), starts disabled. Start 0x5920b0: point += `Target_Position_Z_Offset` (record
  +0x1b8) **on x, y and z** (quirk); spawn `Dummy_Barrage_Target` (one id) owned by the shooter's first enemy
  player (0x294910); build and execute an **Attack command directly** on it; enable the behaviour.
- Service: errors without unit ability 5; switches 5 off when 0x591fe0 fails [L meaning].

**SELF_DESTRUCT (0x43)** [H]: interval 1, starts disabled, enabled by 0x3a4070 when 39 activates (which
starts the active countdown). Each service: once 39's countdown is no longer in a running active phase
(0x42f7a0; X = 0 so it never auto-clears), it suspends itself, runs 0x3a3e20(obj, 39) (spawns the blast) and
`Die(obj, kind 1, killer = self)`.

**DEPLOY_TROOPERS (0x1c)** [M]: interval 1, starts disabled; state machine 0x591a90 (0–10): wait until
stopped and not turning → deploy (0x391260) → wait for deployed → animation 0x61 → at its end spawn troopers
(0x590730) → animation 0x5e → release them (their LANDING 0x1e) → states 6–8 recall / undeploy
(0x590bb0 / 0x58fb80); 10 = done or failed. Spawn counts / draws in 0x590730 not read [L].

**CONFUSE (0x53)** [H] (used by Force_Confuse): skip when obj +0x3a2 bit 0x80 set. Next service = now +
int(FPS × `Confuse_Time_In_Secs`). **2 synced `Get_Float`** pick a point in the map rectangle (centre ± half
extents); move order there (whole team when a member; leader acts); lock (0x3a8e50(…, 1)) and +0x3a2 |= 0x80.
Expiry 0x636960: clear the flag, unlock (container too), suspend. STUNNABLE: §1.10.

### 2.3 Special-ability classes (tactical, shipped)

| Class (FoC / TR) | Interval | Synced RNG | Section |
|---|---|---|---|
| Force_Healing (65 / 261) | 1 | none | 2.3.1 |
| Berserker (1 / 39) | 1 | none | 2.3.2 |
| Proximity_Mines (4 / 37) | 1 (mine 1) | 2 per mine | 2.3.3 |
| Earthquake_Attack (6 / 33), Force_Telekinesis (2 / 5) | 3 | 1 per attempt | 2.3.4 |
| Grenade_Attack (3 / 18) | 1 | none | 2.3.5 |
| Force_Lightning (1 / 12) | 1 | none | 2.3.6 |
| Force_Whirlwind (2 / 12), Stun (2 / 11), Force_Confuse (2 / 5) | 1 | Whirlwind 1 per auto attempt; Confuse 2 per victim | 2.3.7 |
| Arc_Sweep_Attack (6 / 10), Generic_Attack (4 / 5), Eat_Attack (4 / 6) | 1 | 1 per attempt | 2.3.8 |
| Super_Laser (2 / 6) | 1 | none | 2.3.9 |
| Drain_Life (7 / 3), Leech_Shields, Blast, Force_Cloak (1 / 2), Force_Sight, Hack, Repair, Vehicle_Thief, Maximum_Firepower | 1 (Vehicle_Thief 0) | Vehicle_Thief 1 | 2.3.10 |
| Battlefield_Modifier (29 / 792) | 0 | none | 2.3.11 |
| Enable_Ability (12 / 4) | 0 | none | applies / removes another named special ability for the owner or, with `Affects_All_Allies`, all allies (0x6f8fb0) [M] |
| FoC-only classes | — | none found | 2.3.12 |

Already specified elsewhere: Combat_Bonus / Enhance_Defense (TACTICAL_COMBAT §4.3), Redirect / Absorb_Blaster
(§3.8, ABILITIES §4.6), Stealth, Sensor_Jamming, Laser_Defense, Tractor, Ion / Lucky shot, Concentrate_Fire,
Energy_Weapon (ABILITIES §3). Neutralize_Hero and Base_Destruction are galactic (strategic agent).

**2.3.1 Force_Healing** (Heal_Range +0x178, Heal_Percent +0x17c, Heal_Amount +0x180, Heal_Interval_In_Secs
+0x184, Single_Target_Heal +0x188) [H]
- Apply is a no-op; update 0x6fbb20 does the work. Skips without a SAP pack, when the combat data pack's
  **Force-disable timer** runs (0x380b10, §2.3.1a: a nearby enemy Ysalamiri-type emitter, not combat or damage), or
  when garrisoned (+0x3a7). Fires when frame ≥ due (pack +0x25c); due := now + **int(FPS × interval + 0.5)**.
  A skipped update does not move the due frame.
- Amount > 0 or percent > 0 → heal (vf32 0x6fbf80); amount < 0 and percent ≤ 0 → harm (vf33 0x6fbc20).
- Heal, range ≤ 0: self when health fraction (0x396470) < 1. Range > 0: query within range of the owner
  (0x2a8fb0); candidate: +0x348 = −1 [L: not a sub-object], applicable filter, **allied** (0x39a230), has a
  model, combined fraction (0x396e60: hull + targetable hardpoints when type +0xa8) < 1, not converted.
  Single target = lowest fraction (strict <, first wins); else all candidates. (Early return when the owner
  is the neutral player and 0x28a910 = −1 [L].)
- Amount (0x6fb940): with hardpoints, the damaged targetable hardpoint with the **lowest** fraction gets
  clamp(max × percent + amount + current, 0, max); else hull := max × percent + amount + current (0x3a89d0).
  Then the damage-state refresh 0x3ac290 (§6.3).
- Harm: enemies in range, health > 0, not garrisoned: `Take_Damage(kind 5, −amount)`; single = lowest hull
  fraction. Animation state 0x76 when present.

**2.3.1a The skip gate 0x380b10 (q6 land follow-through, 2026-11-04)** [H unless tagged]

Question from the port: does a structure under fire stop self-healing? No. The gate is the Force-suppression
timer, not a combat test.
- **Test.** 0x380b10(combat data CD = obj +0xf0) reads the frame stamp CD +0x110. A stamp < 0 → false. Else
  elapsed = (NOW − stamp) / FPS, with elapsed < 0.1 s counted as 0, and the result is elapsed ≤
  `Force_Ability_Disable_Time` (GameConstants +0x11824). It is the same timer as gate 3 of
  `Can_Activate_Unit_Ability` (ABILITIES_HARDPOINTS §2.4). Its neighbours are the shield-depleted lockout 0x380ad0
  (CD +0x108, `Depleted_Shield_Disable_Time`) and the nebula lockout (CD +0x104).
- **The only writer of CD +0x110** is the behaviour update 0x636260, which is the `Disable_Force_Abilities_Range`
  emitter (type +0x53c, read as a float here although xml_field_tables lists the tag as int [M]). Every update it queries objects within that 3-D range of the emitter that
  are not owned by the emitter's owner (GOM query 0x2a9ff0 mode 2 [M: "other owners"]). For each object that is
  alive (+0x3a0 bits 0x40, 0x80 and 0x02 clear), not a projectile (+0x33a = −1), has a model and a type, and owns at
  least one unit ability whose info-table entry has the **F (Force power)** column set (0x3986b0 × 0x43fa30 over
  types 0..0x4c), it stamps CD +0x110 := NOW.
- Users: FoC `Ysalamiri_Tree` (150), `Underworld_Ysalamiri_Cage` (250), `Rebel_Mobile_Shield_Generator` (150).
  Thrawn's Revenge sets the tag only once, to 0 (`Conquest_Bases.xml`).
- With the shipped `Force_Ability_Disable_Time` 0.0 (FoC and TR), the gate is true only for about 0.1 s after the
  last stamp, so it holds exactly while the emitter keeps re-stamping (the victim stays in range).
- **Effect on Force_Healing.** A healer is skipped only while it is Force-suppressed. Taking damage, being targeted,
  being in combat, being a structure and the owner do not matter. A structure, or any object without an F-flag unit
  ability, is never stamped, so it never skips. The TR / FoC `R_Command_Center_Self_Repair` (Heal_Range 0, 2.5 % every
  5 s, `Skirmish_Automatic`) therefore heals 70 hp (2800 × 0.025) every 150 frames while under fire, as long as
  hull < max.
- The same 0x380b10 test also ends Force_Sight early (ABILITIES_HARDPOINTS §4.14, Force_Sight).
- One more condition in the update: when `Heal_Range` (+0x178) ≥ 0, 0x6fc3d0 runs first, every update. It was not
  read [L: area visuals or the candidate refresh]. It does not gate the heal.

**Port notes**
- Implement 0x380b10 as "Force-disabled": CD +0x110 is written only by the `Disable_Force_Abilities_Range`
  behaviour, for victims that own an F-flag unit ability. Do not tie Force_Healing to damage, attack or "in combat"
  state. A self-repairing command centre heals at full rate while it is attacked.
- With current FoC and TR data the gate never fires for any structure. A land skirmish that stalls on a
  self-healing CC is an AI follow-through problem (AI_TACTICAL §7.7), not a healing bug.

**2.3.2 Berserker** (Activation_Max_Range +0x17c, Speed +0x180, Damage +0x184, Damage_Range +0x188,
Duration_In_Secs +0x18c) [H unless tagged]
- Accepts: enemy, not a projectile (+0x33a = −1), has a locomotor (+0x333), filter, model.
- Apply: in range and not "needs to close"; counter = int(FPS × duration); speed override (locomotor
  +0x2a0 := Speed, flag +0x29c := 1, 0x3a8c90); start recharge 51; animation 0x68.
- Update: counter −1, remove at < 1. Damage pass only when counter % 5 = 0 (or the animation is ending,
  0x39a180) **and** the animation frame is strictly between 20 % and 80 % of the clip [M: animation-driven]:
  every live enemy (not 0x40 / 0x80 / 0x02, not a projectile, locomotor + model) with 3-D distance − radius ≤
  Damage_Range takes `Take_Damage(kind 0x16, Damage)`. Port: ascending id.
- Target upkeep: drop the SAP target when not moving and it is beyond Damage_Range; else pick the nearest
  acceptable enemy with (squared distance − radius) < (1.3 × Activation_Max_Range)² (mixed units — keep the
  quirk) and move to it; none → remove.

**2.3.3 Proximity_Mines** (Activation_Max_Range +0x17c, Activation_Time_In_Seconds +0x180, Trigger_Radius
+0x184, Mine_Type +0x188/+0x190, Number_Of_Mines +0x198, Activate_SFX +0x1a0) [H]
- Apply 0x71a4d0: the target is the dummy point object, in range; the owner stops (0x3a9880) and faces it; the
  dummy is destroyed; start recharge 67 (owner or every team member).
- Spawn 0x71a650: R = unit ability 67's `Area_Effect_Decal_Distance` (+0xa8). Per mine **2 synced draws**:
  angle `Get_Float(0, 360)` degrees, radius `Get_Float(−R, R)`; position = point + (cos, sin)·r, z = terrain;
  create Mine_Type owned by the owner (**one id**; a failure stops the loop). Mine data (+0x220): trigger
  radius, arm frame = now + int(FPS × activation time) + 1, SFX.
- Mine behaviour (`ProximityMineBehaviorClass` 0x6373b0, interval 1): after the arm frame (SFX at exactly the
  arm frame, audio), on frames ≡ 0 (mod 5) test 0x637190: live mobile non-projectile enemies with a model; one
  within its own detection range (0x3979d0) sets "detected" (+0x20); the **first** within 3-D ≤ Trigger_Radius
  makes the mine attach a DEATH behaviour (0x4c3520("DEATH")), so it dies and detonates via its death clone /
  death projectile [M]. Port: ascending id.

**2.3.4 Earthquake_Attack / Force_Telekinesis** (Activation_Chance +0x180, Damage_Amount +0x184,
Damage_Percentage +0x188, Damage_Interval_In_Secs +0x18c) [H]
- Target needs `EARTHQUAKE_TARGET` (0x31, structures) / `TELEKINESIS_TARGET` (0x30), not already affected,
  enemy, filter, +0x363 (Earthquake), model.
- Apply: remove the previous effect; not in animation 0x25–0x27 (Earthquake) / 0x21–0x23 (Telekinesis);
  not moving; in range; not needs-to-close; **one synced draw < Activation_Chance**. Earthquake also removes
  the owner's own Force_Cloak effect and breaks stealth (0.3 s) when it was running. Face, hold (0x63f060 /
  0x6331e0), animation; hand to the target behaviour with D = (max tactical health + max shield 0x3725f0) ×
  Damage_Percentage + Damage_Amount, the interval, source = owner.
- Target behaviour (0x633330 Earthquake; Telekinesis 0x63fa30 …): state 1 ramps over the constant
  _DAT_140b15ad8; state 2 does `Take_Damage(kind 3, D)` every interval, timed in float seconds from
  DAT_140b0a320 / FPS [M: mode or global frame — **probe**: does it pause with the game]. Remove → animation
  0x27. Telekinesis starts recharge 13; Earthquake's recharge is not started here.

**2.3.5 Grenade_Attack** (Activation_Min/Max +0x1a0/+0x1a4, Requires_Direct_Player_Command +0x1a8,
Grenade_Type +0x1b0, Toss_Anim +0x1c0, Spawn_Frame +0x1c4, Spawn_Bone +0x1c8, Explode_Timer +0x1e8) [H]
- Accepts: enemy, filter, target type +0x41, alive, model. Apply: with Requires_Direct_Player_Command and no
  direct command, AI / non-human users refused (0x2814d0 / player +0x62); in range; Grenade_Type is a lobbed
  projectile (type +0x1fe8 = 3) with a solution (0x54a6f0, unit position → target position, no z adjust;
  LOB_SOLVER.md §3.4); unit ability 22's counter ≤ 0. Face, stop, lock
  (0x3a8e50 1), throw pending (+0x17c), store target, suspend targeting.
- Update: once not moving/turning, toss animation (−1 = at once); at frame ≥ Spawn_Frame throw once
  (0x700a70): spawn from the bone through the projectile spawner (**one id**), arm its BOMB with the timer,
  22 active + start recharge. At animation end resume targeting / locomotion, unlock. No RNG.

**2.3.6 Force_Lightning** (Activation_Max_Range +0x17c, Damage_Amount +0x180 unused, Max_Targets +0x1f0,
Targets_Radius +0x1f4) [H]
- Apply 0x6fd010: not in animation 0x28 (unless ending), not moving; primary = clicked object or its
  container; in range. Face, hold, animation 0x28.
- Targets (0x6fcd60): primary, then per enemy player (list order) that player's objects within the radius of
  the primary (query order; port: ascending id) passing accepts, up to Max_Targets.
- **Each target takes `Take_Damage(kind 4, its current health +0x5c, flag 1)` at once** — kill-all subject
  only to damage multipliers (Damage_Amount 9999 unused) [**probe**: does it kill through shields]. Corpses
  (or death clones +0x2c0) kept for the beam; start recharge 15. Update is visuals only.

**2.3.7 Wave abilities — Force_Whirlwind, Stun, Force_Confuse** [H]
- Fields: Whirlwind Activation_Chance curve +0x178 (CubicInterpolator, §3.3), Min/Max +0x1a0/+0x1a4,
  Damage_Amount +0x1a8, Damage_Range +0x1ac, Travel_Time_In_Secs +0x1b0. Stun / Confuse: Min/Max
  +0x178/+0x17c, Time +0x180, Range +0x184, Travel_Time +0x188, effect +0x190.
- Auto-use pre-check (skipped on direct command / no target): true at once when a wave runs; squared
  distance in [Min², Max²]; ≥ 1 eligible victim within Range; Whirlwind: curve value [M: argument = victim
  count] and **one synced draw < value** (shipped curve 0→0, 1→0, 5→1, 500→1).
- Wave: interval > 0 and Travel_Time > 0 → radius starts at (interval / FPS) / Travel_Time × Range and grows
  by that step per update up to Range; else full Range at once. Victims via 0x2a9ff0 (mode 2): enemy, filter,
  alive, not 0x80 / 0x02, not garrisoned, not already in the per-cast id list.
- Effects: Whirlwind — victim faces along owner → victim, `Take_Damage(kind 2, Damage_Amount)`, animation
  0x1f, recharge 14. Stun — STUNNABLE victims stunned **int(Stun_Time × 30.0)** frames (literal 30), recharge
  49. Confuse — CONFUSE added if missing for int(FPS × time), recharge 57, 2 draws per victim (§2.2).

**2.3.8 Melee — Arc_Sweep_Attack, Generic_Attack, Eat_Attack** [H]
- Common: not moving, not 0x39a350; animation not 5 unless ending; in range, not needs-to-close; facing
  |angle| ≤ **15°** (Arc) / **5°** (Generic); **one synced draw ≤ Activation_Chance**; hold; `Attack_Animation`
  (+0x190) at `Attack_Animation_Speedup` (+0x194).
- Generic: at frame ≥ `Damage_Frame_Number` (+0x198) one hit of (max health + max shield) ×
  Damage_Percentage (+0x188) + Damage_Amount (+0x184), kind `Damage_Type` (+0x18c).
- Arc_Sweep: sweep from −Arc/2 (or +Arc/2 with Apply_Damage_Right_To_Left); step per update = interval ×
  (Arc / Damage_Sweep_Time) / FPS × Speedup (sweep time ≤ 0: whole arc). From `Begin_Applying_Damage_Frame`
  (+0x1ac) each update damages accepted objects within 1.5 × Arc_Max whose (2-D distance − radius) ∈
  [Arc_Min, Arc_Max] and whose bearing is in this update's slice; damage as Generic. Slices do not overlap
  (each victim normally hit once); a victim the hit will kill turns to face the owner.
- Eat_Attack: prefers a team member of the target (first accepting member passing reach 0x6f8010; a lone last
  member is detached and given to neutral); optional turret rotation; at `Attachment_Frame` (+0x1c8) grab
  (held, targeting off, grabbed animation, attached to the owner bone, re-owned to neutral); at
  `Target_Destruction_Frame` (+0x1cc) `Take_Damage(Damage_Type, 1,000,000)` and the owner heals
  `Health_Per_Victim` (+0x1f8).

**2.3.9 Super_Laser** (Damage_Per_Frame +0x178, Fire_Frame +0x184, Attack_Anim_Sub_Index +0x188,
Face_Target +0x1c8, Turn_Rate +0x1cc, Firing_Cone +0x1d0, Fire_Delay +0x1d4, Fire_Continuation +0x1d8)
[H flow, M continuation]
- Apply: needs 55; in range; 55 active; fire frame = GMM frame (+0x10) + int(FPS × Fire_Delay). Face_Target
  with TACTICAL_SUPER_WEAPON (0x2c): turn to the target at Turn_Rate; else animation 5 / sub-index at once.
  "super_laser_fired" event.
- Update: after the delay and inside Firing_Cone start the animation; at frame ≥ Fire_Frame start 55's
  active countdown; while it runs `Take_Damage(kind 1, Damage_Per_Frame)` **every frame**. Ends when 55
  stops or the target leaves range (recharge at end, §1.11). Fire_Continuation = no-target branch (+0x308) [M].

**2.3.10 Smaller tactical classes** [H unless tagged]
- **Drain_Life** (DPS +0x178, Duration +0x17c, Charging_Time +0x180, Drain_Radius +0x1e8, Max_Victims
  +0x1ec, Should_Heal +0x1f0): charging > 0 needs the target within Drain_Radius; up to Max_Victims (0x71b1d0
  [M: like Lightning]); owner stops (held when charging = 0); recharge 69; counter = int(FPS × duration). Per
  frame each victim takes DPS / FPS (kind 4 when charging > 0, else 0x16); Should_Heal heals the owner by the
  same when its fraction < 1. A victim leaving the radius ends it (charging variant).
- **Leech_Shields** (Duration +0x180, Shield_DPS +0x184, Damage_Multiplier +0x188): needs 58 (live
  WEAPON_SPECIAL hardpoint), in range; 58 on, counter = int(FPS × duration), recharge 58. Per frame while
  `Can_Activate(58)` and in range: target shield drained by SDPS / FPS (0x549ac0); ends at shield 0.
  Damage_Multiplier consumed in 0x7182d0 / 0x717970 [L].
- **Blast** (Charge_Up_Seconds +0x178, Damage_Multiplier +0x17c): charge counter int(FPS × charge),
  charging particle and "buildup" mesh, recharge 70, obj +0x3a1 |= 0x10; at 0 charged effect, counter :=
  −(WEAPON_SPECIAL hardpoint count, 0x397c10). Multiplier application to the next special shots not traced [M].
- **Force_Cloak** (colour +0x178, transition +0x188): cancels the owner's own Earthquake; stealth fields as
  `Stealth_Ability` with duration int(FPS × unit ability 54 `Expiration_Seconds`); targeting drops; BASEMESH
  hidden, STEALTH shown; per frame counter −1, detection 0x592ae0, removal at 0.
- **Force_Sight:** apply stops the unit, animation 0x69 (refused while in 0x69–0x6b); update 0x713ba0 is the
  Reveal_All channel (FOG_OF_WAR §3.3).
- **Hack / Repair** (`Team_Member_With_Ability` picks the acting member): animation 0x5a facing the target;
  at its end **Hack changes the target's owner** (and its container's) to the hacker's owner (0x38e5a0);
  recharge 20. Repair (recharge 21) same shape with a repair effect [M].
- **Vehicle_Thief** (Min/Max +0x178/+0x17c, chance +0x180): in [min, max] (2-D − radius) and **one synced
  draw < chance**; if the victim type has a capture variant (+0xda8): killer byte, signal 0x25, spawn the
  variant at the victim position with the victim's owner (**one id**), remove the victim; VEHICLE_THIEF
  (0x32) boards the thief; CAPTURE_VEHICLE 18 on.
- **Maximum_Firepower:** 45-driven state machine (0 / 4 / 6), pulses until int(FPS × 45's
  `Expiration_Seconds`) frames or target lost; needs a firing solution (0x54aa30) [M].

**2.3.11 Battlefield_Modifier** (Stacking_Category +0x178, FOW_Reveal_Range_Multiplier +0x17c,
Reinforcement_Time_Multiplier +0x180, Stealth_Duration_Multiplier +0x184, Capture_Point_Time_Multiplier
+0x188, Reinforcement_Deploy_Time_Multiplier +0x18c, bools +0x190–0x196) [H]
- Apply vf30 0x6f10b0 → 0x6f02c0, tactical modes only (mode vf +0xf0). Players from 0x6f1130: owner, or all
  allies with Apply_To_All_Allies; Reverse_Application_Logic via 0x6f13b0 [M: enemies].
- Per player, append inserts (0x38c850; category +0x178; source = (ability, user); name = user type name;
  duration −1; removed when the ability ends, 0x6f13b0):
  - list **+0x1f8 ← FOW_Reveal_Range_Multiplier − 1** (skipped at 1.0) — FOG_OF_WAR open item 2, §5.2;
  - list +0x230 ← integer frame delta round(F × m) − F (F = reinforcement-time constant DAT_140b26fe8,
    rounding 0x22f9c0);
  - list +0x268 ← Stealth_Duration_Multiplier − 1; +0x188 → +0x288 and +0x18c → +0x2a8 the same rule.
- TR: 792 entries (reveal multipliers 1.06–1.18 → × (1 + Σ(m − 1))).

**2.3.12 FoC-only classes (apply bodies skimmed)** [M]; no synced draws found.
Buzz_Droids (spawns `Object_Type`, one id; damage radius, enemy / own DPS, chase radius, arm int(FPS ×
Activation_Time); recharge 0x47; BUZZ_DROIDS behaviour does the rest). Remote_Bomb (toss like Grenade;
DETONATE_REMOTE_BOMB via 0x280ec0). Cluster_Bomb (12 × Bomb_Type, BOMB timer `Detonation_Time_In_Secs`
+0x178; recharge 0x3d). Personal_Flame_Thrower (cone Damage_Arc, Damage_Amount over Fire_Time after
Damage_Delay; recharge 16). Shield_Flare (after Charge_Up_Seconds, shield-scaled `Take_Damage(kind 1)` capped
by Max_Damage_Per_Victim +0x178 within Damage_Radius +0x17c; recharge 0x48). Corrupt_Systems (attaches
CORRUPT_SYSTEMS 0x58 to the victim; recharge 0x4a). Summon / Spawn (create `Object_Type` after
Creation_Delay; Spawn: suicide bombers). Tactical_Bribe (**changes the target's owner**, 0x38e5a0; recharge
0x3b). Infection (throw → Infection_Projectile; INFECTION behaviour remaps death clones to key 0x14).
Radioactive_Contaminate (spawns Contamination_Object_Name after Spray_Time; recharge 0x32). Saber_Throw
(recharge 0x35). Permanent_Weapon_Swap (obj +0x324 := Weapon_Index). Weatherproof (§7.4). Cancel_Special
(cancels another ability). Find_Weakness, Hero_Protection, Hack_Super_Weapon: galactic.

---

## 3. Tactical combat (TACTICAL_COMBAT §8 items 9, 12; S8 port choices)

### 3.1 Targeting slots 44 / 45 [H]
`TargetingBehaviorClass` vtable 0x8b0d88: slot 44 = 0x567b40 ("in max range"), slot 45 = 0x5688c0 ("inside
min range"). `Scan_Player` 0x5662b0 accepts a candidate when slot 44 is true **and** slot 45 false (a
container contribution through 0x4160a0 can also accept).
- **Slot 44:** current weapon's projectile (slot 42, weapon set obj +0x324) of `Projectile_Category` 3
  (grenade) → lob solver 0x54a6f0; otherwise slot 43 (0x5673b0).
  **Spec of the solver, the grenade launch and flight: [LOB_SOLVER.md](LOB_SOLVER.md)** (S8 t03, 2026-11-01): in range ⇔ a
  closed-form high arc from the attacker position to the target + `Ranged_Target_Z_Adjust` exists at `Max_Speed` under
  G = gravity × lob mod; none of slot 43's R / E / redirect rules apply.
- **Slot 43:** in range iff XY distance(attacker, aim point) ≤ R + E; zero offset counts as in range.
  - R = slot 40 `Get_Max_Attack_Range`; **override**: combatant pack ordered target (+0x148 / +0x150 ≠ 0)
    equal to the candidate or its container and a pending special ability → R = the ability's vfunc +0xd8
    range (> 0).
  - Melee units (`Targeting_Max_Attack_Distance` ≤ `Melee_Cutoff_Range`, 0x372210) against a team container:
    redo against the member nearest the attacker (0x404fc0).
  - E without LAND_OBSTACLE (obj +0x346 = 0xFF): +10 for a moving team (locomotor vf +0x230); otherwise the
    target footprint half-extent along the line of sight: |dot(target facing, direction)| > 0.7071 → X extent
    (0x396a30), else Y extent (0x396b70); each = `Custom_Hard_X/YExtent` × type +0x658 (deployed variants when
    deployed), fallback |model bbox max x/y| × +0x658. With a hardpoint argument: 0x397780 (team radius, else
    0x373b30 = 0.5 × 20 × value).
  - E with LAND_OBSTACLE: the aim point steps back toward the attacker in 20-unit steps while the passability
    cell is occupied by the target (0x59efa0), snaps to the cell grid, E = √200 ≈ 14.14.
- **Slot 45 → slot 46 (0x5683a0):** same geometry with r = slot 41 (0x566eb0) = `Targeting_Min_Attack_Distance`
  (type +0x504) or the pending ability's vfunc +0xd0 (`Activation_Min_Range`); false when r ≤ 0; forwards to
  the nearest member of a squad-member target's container.
- **Port:** replace "XY ≤ R + target radius" by R + orientation-dependent footprint half-extent, and add the
  min-range rejection (only types with `Targeting_Min_Attack_Distance` > 0).
  **A1j t04 (2026-11-02):** the extent getters are 0x371e70 (X: `Custom_Hard_XExtent` +0x16bc) and 0x372040 (Y:
  +0x16c0), each × `Scale_Factor` when > 0, else |model bbox max.x| (resp. max.y, bbox +0x10 / +0x14) × `Scale_Factor`
  of the current mode's model (0x3729f0), 0 with no model; deployed X 0x371f30 reads +0x16cc first. The port used the
  unit radius (for a starbase the collision-class constant) whenever no Custom_Hard extent was set; it now takes the
  model box from the battle catalog (`UnitDesc::model_x_extent` / `model_y_extent`) and keeps the radius only when the
  host gives no model data. Effect: Rebel capitals 2000-2500 from a station's centre now accept and promote it.

### 3.2 The constants [H]
All are fields of the GameConstants object (.bss, base 0x140b15800), loaded from XML — read them from data.

| RVA (GC offset) | Field | FoC | TR 3.3 | Use |
|---|---|---|---|---|
| 0x140b27338 (+0x11b38) | `Health_Low_Percent_Threshold` | 0.33 | **0.50** | targeting replacement step 1; free-store eligibility; local "health low" speech (presentation) |
| 0x140b2733c (+0x11b3c) | `Health_Critical_Percent_Threshold` | 0.10 | 0.25 | speech only |
| 0x140b27340 (+0x11b40) | `Melee_Cutoff_Range` | 51.0 | 51.0 | short-range attacker = `Targeting_Max_Attack_Distance` ≤ it (0x372210): targeting filter 5, §3.1 redirect, MovingVsStopped 0x684f90, formations 0x43c1b0, 0x3825b0, 0x38d730, 0x54ade0 |
| 0x140b278ac (+0x120ac) | `Garrisoned_Max_Attack_Distance_Multiplier` | 1.5 | 1.5 | range of a garrisoned unit |
| 0x140b27048 (+0x11848) | `Hardpoint_Recharge_Cutoff_For_Opportunity_Fire` | 3.0 | **5.0** | opportunity fire (ABILITIES §4.2) |

### 3.3 Diminishing-firepower curve [H; M for the pair reading]
- `Diminishing_Firepower` (GC +0x117f0) is a **CubicInterpolator** (vtable 0x85c120). XML: a flat number list
  read as (x, y) pairs. FoC `0, 0.6, 0.3, 0.7, 0.9, 0.9, 1, 1, 2, 1` = (0, 0.6) (0.3, 0.7) (0.9, 0.9) (1, 1)
  (2, 1); TR `0, 1, 1, 1` = constant 1 (off).
- Setup (0x2c8a30, lazily at first evaluation): points sorted by x (0x2c3f50), **natural cubic spline**
  second derivatives (y″ = 0 at both ends), float.
- Evaluation (vf2 0x2c84b0): binary search for the first x_k > x; past the last → last y, before the first →
  first y; else cubic-spline interpolation between k−1 and k in float.
- Argument (Take_Damage 0x3a9e30, kind 1 only, gated by the attacker's ALLOW_TARGET_DIMINISHING_FIREPOWER
  flag): x = (float)(now − victim +0x304) / (float)FPS, +0x304 := now on every such hit; first hit (+0x304 = 0)
  uses **999999 frames** (x ≈ 33333 s → last y).
- The same CubicInterpolator class backs other curves (e.g. the Whirlwind chance curve, §2.3.7).

---

## 4. Battle transitions (BATTLE_TRANSITIONS §7 items 2, 3, 5–11)

### 4.2 Land force split 0x2bc6a0 and land reinforcement flow (item 2) [H unless tagged]
- **No population cap at land battle start.** 0x2bc6a0(GOM, humanOnly, forcedType) fills the start lists
  GOM+0x498 and reserve lists GOM+0x4d8 from the land head record (callers 0x2b76a0, 0x28f590):
  1. **Defender:** every object on the planet surface (0x397120 list) → start (with humanOnly = 1 only when
     the defender is human, player +0x62).
  2. **Attacker** (skipped likewise when humanOnly = 1 and the attacker is AI). Lead unit 0x29c0c0 from the
     record's unit ids (+0x10), first rule that matches: (a) first unit whose data +0x1b0 byte +0x32 is set
     (fleet lead) or whose unit type (0x3973b0) equals forcedType; (b) walking `Preferred_Pathfinder_Types`
     (GC +0x119f0 / +0x119f8 / count +0x11a00) in order, first type-name match (case-insensitive) among
     non-heroes, then heroes; (c) first non-hero, else first hero, else none.
     - **Raid:** the lead's fleet is at the planet and passes 0x432e20 (`Raid_Force_*`, GC
       +0x11738..+0x11750: faction = `Raid_Force_Required_Faction` when set; every member's CategoryMask in
       the free or limited mask; 1 ≤ limited ≤ `Raid_Force_Max_Limited_Objects`; heroes ≤
       `Raid_Force_Max_Heros`) → all record units start; record and head +0x50 := 1.
     - Otherwise only the lead starts (a transport lead is pulled from its fleet and landed, 0x4f3bd0,
       respecting `Max_Ground_Forces_On_Planet`; back to the fleet on failure). Every other attacker transport
       (TRANSPORT +0x337) in attacker fleets in orbit → reserve.
  3. Planet snapshot into GOM+0x518 / +0x520..+0x530 (as space).
  - FoC raid: Rebel only (Infantry | Vehicle | Air, ≤ 2 limited, ≤ 1 hero, LandHero free). **TR leaves the
    raid fields empty**: never a raid.
- Reinforcement cap: AI_GALACTIC_3 §3.1–3.2 (0x2aba70, rounded up past
  `Allow_Reinforcement_Percentage_Normalized`). TR comments that line out → ctor default **0.4**; FoC 0.0.
- **Land drop validity** (LandMode vf80 0x3b5f60 → 0x3b6170) [H radius rules, M formation layout]: formation
  slot points for the unit/team (`Max_Squad_Size` +0x1e44, 0x5abc00); each slot passable for the movement
  class (0x50e120), **not fogged for the player** (0x35f3b0), footprint free (TreeCull AABB). With
  `Use_Reinforcement_Points` (GC +0x118a0; True in FoC and TR) a slot is valid only within 2-D ≤
  `Reinforcement_Enabling_Radius` (+0x1e20) of an allied live object from GOM list +0x2f0; any slot within <
  `Reinforcement_Prevention_Radius` (+0x1df8) of a non-allied object without POWERED, or with POWERED and
  powered (0x527520), is invalid. Full layout and tests (corrected 2026-11-01): §4.2.1. "Powered" there is the
  object owner's **base power**, not the object's own energy.
- **Space drop validity** (SpaceMode vf80 0x4d6fd0): unless forced, rejected when fogged (force 1, so AI is
  tested too), inside a non-allied prevention radius (0x4d8d10), or outside the playable box; then the
  occupancy tracker (+0x388) must have no reservation on the type's `Space_Layer` (+0x16dc, default 4) over the
  box swept from pos − `Space_Reinforcement_Collision_Check_Distance` × (cos, sin)(player entry facing)
  (FoC 200, TR 150) to pos during frames [now, now + 115]; reservations of objects in state 4 (0x397900) are
  tolerated; layer 8 must also be clear; squadron footprint = union of member offsets + extents.
- Lua space `Reinforce` (0x6e8620) refuses AI players with no allied DUMMY_STAR_BASE left in campaign battles.
- Garrison spawn (`Garrison_Upgrade_Ability` 0x6fffa0, `Garrison_Reinforcement_Delay_Seconds` type +0x2ee4)
  is not part of the split [L].

#### 4.2.1 Land drop layout and validity: 0x3b6170, 0x5abc00, 0x40f970 (a1g follow-up, 2026-11-01) [H unless tagged]

**Entry points.**
- `LandModeClass` vf80 0x3b5f60(mode, player id, pos, type T, force, f6) calls 0x3b6170 with:
  - facing = mode +0x288[player +0x48], an angle triple (roll, pitch, **yaw at +8, degrees**) [M: the per-player
    entry facing];
  - check-radius = !force;
  - fog test skipped = force;
  - movers-ignored = force or the player is not human (+0x62 = 0);
  - landing-transport rule = f6.
- 0x3b6170(mode, pos, facing, T, player, checkRadius, out positions, out per-slot bits, n = 0 (= T's member
  count), skipFog, ignore list, moversIgnored, landingRule) returns true only when **every slot** passes every test.
  The per-slot bits are output for the UI.
- T is a **company** type. Slots are its `Company_Units` (+0x958 entries {name, type} 0x10 B, count +0x960). Count 0
  → false.
- "Member type M" = the entry's type. If M has no land model (0x3729f0(M, 1, 0) = 0) and its +0x1b60 is set, M's
  +0x1b60 type is used instead (layout, passability and footprint).

**Slot layout** (offsets in world units, relative to pos; then rotated by the facing yaw ψ and added to pos; z adds
too). It branches on the **first** member type F's `Create_Team` (+0x67):

1. **F without `Create_Team`** (vehicle companies): one grid over all n members, from 0x40f970(cell, n). Here
   cell = (a, b) = (2·hx/20 + FS, 2·hy/20 + FS) (0x41b170), where:
   - hx, hy = F's half extents (0x371be0: `Custom_Hard_XExtent` / `YExtent` × `Scale_Factor` when both are > 0;
     else |model box max x / y| × `Scale_Factor`);
   - FS = F's `FormationSpacing` (+0x16d4).
2. **F with `Create_Team`** (infantry): S = min(T's `Max_Squad_Size` (+0x1e44), n); teams = ceil(n / S); members
   are chunked in list order (team t = members [t·S, min((t+1)·S, n))).
   - **Inside a team:** if T's `Create_Team_Type` (+0x1e58) has `LAND_TEAM_CONTAINER_LOCOMOTOR` in its
     `LandBehavior` list (+0x718): **0x5abc00**((hx, hy) of F, g = 20 × FS of F, S). Otherwise 0x40f970(cell of F,
     S).
   - **Teams:** placed by 0x40f970((2.5·Bx, 2.5·By), teams), where Bx / By are the team layout's half extents /
     20 (the bounds output below).
   - Member i of team t lands at pos + R(ψ)·(O_t + R(φ_t)·m_j), where:
     - j = i − t·S;
     - m_j = the team layout's slot j;
     - O_t = team offset t;
     - φ_t = yaw of the vector 0 → O_t (0x20acd0: atan2f(y, x) × 57.295776, +360 if negative; 0 when O_t = 0).
   - So a team behind the drop point (O_t.x < 0, y = 0) is turned by 180°.

**0x5abc00(ext = (ex, ey), g, S) [H]** (rows across x, columns along y):
- Columns c = ceil(√S): (uint)sqrtf((float)S), plus 1 unless that squared equals S.
- Rows take k = min(c, remaining) members in order.
- In a row, slot y_j = −(k−1)(2ey + g)/2 + j·(2ey + g); y = 0 when k = 1.
- Row r is at x_r = ((R−1)/2 − r)·(2ex + g), R = number of rows. Row 0 is the most +x row (front); rows are
  centred in x.
- z = 0.
- Bounds output: {0, 0, 0, Bx, By, 0} with Bx = (max(x+ex, 0) − min(x−ex, 0))/2 × 0.05 and By likewise with ey.
- All float in this order: the row width is summed per slot (2ey + h at the row ends, g between; h = g/2).

**0x40f970(cell = (a, b), N) [H]** (constant 1.8 at 0x140869960):
- side = sqrtf(max(1.0, N·b·a·1.8)). b = 0 → fail.
- perRow = max(1, trunc(side / b)); rows = ceil(N / perRow).
- x starts at (rows·a − a)/2 and steps −a per row.
- In a row of k = min(perRow, remaining): gap = (side − k·b)/(k+1). y starts at gap − side/2; per slot y += b/2,
  emit, then y += b/2 + gap.
- Output (x·20, y·20, 0). Bounds {0, 0, 0, rows·a/2, side/2, 0} (cell units).

**Tests per slot** (all must pass; the result is the AND over slots):
1. **Passable** for M at the slot: 0x50e120(grid, M, slot).
2. **Not fogged** for the player: 0x35f3b0(mode, player, slot). Skipped when forced.
3. **Footprint** (an oriented-box test, not a single AABB):
   - Broad phase: the whole formation's box = centre pos, half extents (B'x·20, B'y·20) from the **last** 0x40f970
     bounds (the team grid, or the member grid for vehicles), rotated by ψ. Its AABB (0x20dd40), with z ±1000, is
     queried in the mode's object tree (0x2ab640 → 0x20e780).
   - Each found object X counts unless:
     - X is a PROXIMITY_MINE (slot +0x387 ≠ −1); or
     - moversIgnored and X has a locomotor (+0x333 ≠ −1) and no LAND_OBSTACLE (+0x346 = −1); or
     - landingRule and X has TRANSPORT_LANDING (+0x368 ≠ −1) and its data (X +0xc8) byte +0xc9 = 0 [M: "not yet
       landed"]; or
     - X is in the ignore list.
   - If X's footprint box (0x38de80) intersects the formation box (0x20d690), every slot is tested. Slot box = centre
     slot (x, y), half extents (hx, hy) of M (0x371be0), axes along the yaw of pos → slot (0x20acd0). A slot whose box
     intersects X's box fails.
4. **Reinforcement radii**, only when checkRadius. Over GOM list +0x2f0, skipping objects with +0x3a0 & 0x42:
   - **Enabling:** with `Use_Reinforcement_Points` (0x140b270a0) a slot needs **some** allied object (0x2823c0) with
     `Reinforcement_Enabling_Radius` R > 0 and 2-D d² ≤ R². Without the constant every slot passes.
   - **Prevention:** a non-allied object with `Reinforcement_Prevention_Radius` P > 0 (read as float) and 2-D d² < P²
     fails the slot if X has **no POWERED** behaviour (+0x341 = −1), or has POWERED and **X's owner has base power**.
     That is the per-player list mode vf+0x220(owner) +0x198 through 0x527520, AND of entries, empty = false
     (§7.3).
   - So a POWERED structure (e.g. a turret) blocks reinforcements only while its owner has a working power
     generator. TACTICAL_GAPS' earlier wording "with POWERED and powered" meant this owner-level test.

**Who uses it.**
- AI `TF.Reinforce` (0x6eaf60, A1_READINGS §30.7) and the Lua / AI position finder 0x69fc40 call vf80 with force 0
  and f6 0. The AI is not human, so movers are ignored and landing transports count.
- The human drop preview 0x345c0 calls 0x3b6170 directly with the local player, checkRadius 1, fog tested,
  moversIgnored 0 and landingRule 1. So the human's movers block, and landing transports are ignored until their flag
  is set.
- The `Reinforce` command checks validity again inside Execute (COMMANDS.md) [M: entry not re-read here].
- **One layout serves the human and the AI.** Only the mover / transport filters and the fog bypass differ.
- The transport unload (0x643130) places the cargo with the same per-team layout (0x5abc00 for team containers with
  `LAND_TEAM_CONTAINER_LOCOMOTOR`, else 0x40f970; same `FormationSpacing` × 20 and half extents) [M: the team grid
  around it was not re-read].

**Worked example, FoC `Rebel_Infantry_Squad`** (15 × `Squad_Rebel_Trooper`, `Max_Squad_Size` 5,
`Rebel_Trooper_Team` has `LAND_TEAM_CONTAINER_LOCOMOTOR`; the trooper inherits from `Rebel_Trooper`: hard extents
2.0 × 2.0, `Scale_Factor` 1.5 → hx = hy = 3; `FormationSpacing` 0.5 → g = 10):
- Team layout (S = 5, c = 3): (8, −16), (8, 0), (8, 16), (−8, −8), (−8, 8); Bx = 0.55, By = 0.95.
- Team grid 0x40f970((1.375, 2.375), 3): side = √17.634 = 4.1993, perRow 1, three rows. Team offsets (27.5, 0),
  (0, 0), (−27.5, 0); the third team is turned by 180°. Broad-phase half extents 41.25 × 42.0.
- Slots before R(ψ):
  - team 0: (35.5, −16), (35.5, 0), (35.5, 16), (19.5, −8), (19.5, 8);
  - team 1: (8, −16), (8, 0), (8, 16), (−8, −8), (−8, 8);
  - team 2: (−35.5, 16), (−35.5, 0), (−35.5, −16), (−19.5, 8), (−19.5, −8)
  - (± float rounding: sin of the 180° yaw is about −8.7e-8).
- The whole company covers about 77 × 38 world units. The port's rows of 6 at 40 spacing cover about 200.

**Port rule** (apps/sim battle host, `land_drop_ok` and the slot generator; the [M] "rows of 6 at 40" layout is
replaced):
- Build the slots as above: company members, the `Create_Team` branch on the first member, 0x5abc00 or 0x40f970 per
  team, the team grid, the per-team yaw, then the player's facing yaw. All in float, in this order.
- Test every slot:
  - passable for its member type;
  - fog (unless forced);
  - each slot's own oriented box against every blocking object's footprint box, with the mover / landing-transport /
    mine filters above;
  - enabling radius per slot (≤), prevention per slot (<), with the owner-base-power exemption for POWERED objects.
- AI drops: movers ignored. Human drops: movers block.

**Test:** `Rebel_Infantry_Squad`, facing yaw 0, pos P on open ground:
- the 15 slots equal P + the list above (±1e-3);
- with ψ = 90° each offset (x, y) becomes (−y, x);
- a synthetic enemy object with POWERED and `Reinforcement_Prevention_Radius` 100, 60 from team 0's front row: the
  drop fails while its owner has a `Base_Power_Ability` source, and passes once that source is gone (the owner's
  base-power list is empty); the same object without POWERED always blocks;
- an AI drop next to an enemy infantry squad succeeds; the same drop for a human fails when a member's box overlaps
  it.

**Implemented** (a1g t04 follow-up, 2026-11-01): `land_drop_layout` / `land_drop_shape` / `land_drop_radius_ok` in
`engine/apps/sim` (tactical_battle), used by `BattleAi::land_drop_ok` and the land `order_reinforce`; port choices in
A1_READINGS §30.7 "Drop validity". The FoC data gives the worked example's shape (15 members, 3 x 3, squad 5, team
container locomotor, FormationSpacing 0.5) and slots.

#### 4.2.2 Land drop [M] items settled (a1h t06) [H unless tagged]

1. **Per-player entry facing, mode +0x288.**
   - **Storage.** `GameModeClass` ctor 0x35a5e0 builds a `DynamicVectorClass<Vector3>` at mode +0x280 (data +0x288,
     count +0x290) with one 12-byte entry per player (count 0x140a16fb0) and **zeroes every entry**. Each mode object
     starts a battle with all facings (0, 0, 0). Entries are indexed by player +0x48. They are saved and loaded as chunk
     0x18 of the mode (vf12 0x362f60 / vf13 0x35f970).
   - **Writers.** Three copies, all `record := marker object's orientation triple (obj +0x84, +0x88, +0x8c)`, verbatim.
     The fields are (roll, pitch, **yaw at +8**), in degrees (obj +0x8c = yaw, MOVEMENT). No +180, no offset, no
     camera term. [M: no other writer; found by scanning every mode +0x288 access with a 12-byte stride]
     - **Campaign defender** (post-load placement 0x2a1eb0, REMAINING_GAPS §3.2 step 6): the **first** `Defending
       Forces Position` marker, in the GOM marker list order (MARKER objects with an owner), for the record's defender
       id. It is written whenever such a marker exists, even when the defender has no start units.
     - **Campaign attacker** (same function, step 7): the **chosen** `Attacker Entry Position` marker. That is the one
       farthest from the first attacker-filtered behaviour-0x5f object with a non-zero `Reinforcement_Prevention_Radius`,
       else the one nearest the first `Defending Forces Position`. It is written **only when the attacker's start list
       is non-empty** (record +0x28 ≠ 0) and at least one entry marker exists.
     - **Skirmish** (0x2a4fc0 `Creating_Starting_Forces_For_Skirmish`, once per player whose faction +0x20a8 is set):
       - no teams: the first map marker whose upper-cased type name equals `PLAYER_<k>_SPAWN_POINT_MARKER` (k = the
         player's index within his faction, 0x294c60) **and** whose owner's faction is the player's faction;
       - team game [M: flag = lobby +0xb5]: the k-th `TEAM_<tt>_SPAWN_POINT_MARKER` (tt = player +0x54 as `%02d`,
         k = index within the team, 0x294cc0).
       - The record is written only when a marker matched. FoC MARKERS.XML has `Player_0..7_Spawn_Point_Marker` and
         `Team_00..` markers.
   - **No marker → (0, 0, 0)**, so yaw 0 (+x world). This covers a campaign attacker with no start units, a skirmish
     player with no matching marker, and every third player (neutral, hostile, pirates).
   - Not used as the source: reinforcement points (their facing is never read) and the camera. Mode +0xc0 (the local
     camera focus) is set from the same marker, but only as a side effect.
   - **When:** after the map load, inside the first post-load step (campaign) or the skirmish start-force step. Nothing
     re-writes it during the battle.
   - **Readers:**
     - land drop vf80 0x3b5f60 and the human preview 0x345c0 (ψ = record +8 → sin / cos of ψ·0.017453292, small-angle
       shortcut sin = x, cos = 1 when |x| < 2^-68);
     - land reinforcement spawn 0x2b5fe0: the landing transport is created with the whole triple as its orientation;
     - space arrival 0x2bf2d0 (the same triple);
     - the space drop sweep and the AI ring start u = −(cos ψ, sin ψ) (§4.2, AI_TACTICAL).
   - Port rule: keep one (roll, pitch, yaw) record per player, zero at battle start, copied from the defender's first
     Defending Forces marker, the chosen attacker entry marker (only with attacker start units) or the skirmish spawn
     marker; use its yaw unchanged as ψ for land drops and as the landing transport's facing; replace the port's "side
     deploy facing".

2. **Type +0x1b60 = `Unique_Ground_Unit`.**
   - It is a name field (code 0x1d) at +0x1b58, with the resolved type pointer at +0x1b60. Its space twin is
     `Unique_Space_Unit` +0x1b48 / +0x1b50; `Landing_Transport_Variant` +0x1e60 / +0x1e68 follows the same pattern.
     BATTLE_TRANSITIONS uses the same pair for AI power.
   - The same substitute is used by start placement 0x2a00b0 (`plVar[0x36c]` = +0x1b60 → spawn 0x2b3be0) and by
     0x644330.
   - **FoC:** only `General_Veers` (NAMEDHEROUNITS.XML, a HeroUnit with no model at all) sets it, to
     `Veers_AT_AT_Walker` (UNIQUEUNITS.XML).
   - So company `General_Veers_Team` (one member, `General_Veers`) is laid out, passability-tested and box-tested as the
     AT-AT:
     - hard extents 28 × 10 × `Scale_Factor` 1.8 → hx = 50.4, hy = 18;
     - `Create_Team` is read from the **original** first member (General_Veers: no) → vehicle grid 0x40f970 with N = 1;
     - with N = 1 the single slot is pos itself (x = 0; gap = (side − b)/2 → y = 0).
   - Detail: in 0x3b6170 the `Create_Team` test (+0x67) uses the unsubstituted first member, while the cell
     (0x41b170 → 0x371be0, `FormationSpacing` 0x371b10) uses the substitute.
   - Port rule: when a member type has no land model and has `Unique_Ground_Unit`, use that type for the cell, the
     passability and the slot box, but take `Create_Team` from the original member.

3. **Landing-transport rule: TransportDataPack +0xc9 = `persist`.**
   - X +0xc8 is the TransportDataPack (ctor 0x4ed470 clears +0xc8 / +0xc9).
   - **+0xc9 has one writer:** 0x4ee200, called only from `Create_Cinematic_Transport` 0x2cb7a0 with Lua argument 8
     `persist` (SCRIPT_GAPS §3.1); 0x4ed840 only saves and loads it.
   - Readers:
     - the landing state machine 0x643c30: a persist transport never takes off;
     - 0x3b6170;
     - 0x642ef0, the transport's own landing-clearance test: another landing transport blocks unless it is in state
       3 / 4 or persist.
   - It is **not** "has landed" or "is unloading". The ordinary reinforcement transport (0x2b5fe0 attaches
     TRANSPORT_LANDING) keeps +0xc9 = 0.
   - So with landingRule = 1 (human preview 0x345c0 and the `Reinforce` command), **every** TRANSPORT_LANDING object is
     ignored except persistent cinematic ones, whichever state it is in; a null pack is also ignored.
   - With landingRule = 0 (AI vf80 callers) landing transports block like any other object. The shipped
     `*_Landing` variants have no locomotor (LandBehavior `HIDE_WHEN_FOGGED`), so the AI's mover exemption does not
     apply to them. They are in the tree via their parent's `Collidable_By_Projectile_Living` = Yes, e.g.
     `Imperial_Landing_Craft` [M: variant inheritance].
   - Port rule: human drops ignore landing transports unless created persistent by `Create_Cinematic_Transport`; AI
     drops are blocked by them.

4. **Broad-phase tree and footprint boxes.**
   - **Tree:** 0x2ab640(GOM) returns GOM +0xd8, the all-owners TreeCull (TACTICAL_COMBAT §1.5).
     - Objects are inserted by 0x29f810 (create) and 0x2b5ad0 (register), and removed by 0x2a6a00 / 0x2b68d0.
     - An object is inserted only when its type has `Collidable_By_Projectile_Living` (+0x41), or `_Dead` (+0x42) when
       obj +0x3a0 bit 0x40 (death clone) is set.
     - Both **default to false** (flag-block initialiser 0x367100 leaves type +0x41 / +0x42 = 0).
     - All owners count (own, allied, enemy, neutral); there is no owner filter in 0x3b6170.
     - The link's cached box is the object's world AABB (model collision bound, SPACE_AVOIDANCE §3.1). The query box is
       the formation AABB with z ±1000.
   - **FoC data** (tag set explicitly; variants inherit [M]):
     - Yes: 441 types. Units, heroes, vehicles, structures, special structures and star bases; 32 indigenous;
       41 build pads (34 say No); 12 `Props_Story`, 3 `Props_Generic`, 2 `SpaceProp`; 1 marker; 1 projectile.
     - Not in the tree: terrain props (`Prop_*`, `Props_Urban`, `Props_Temperate`, `Cin_GroundProp`), companies,
       team containers (`Container`), particles and most markers.
     - So infantry members are tested one by one; the TEAM object never is (its 0x404450 team box is unused here).
     - Trees, rocks and terrain decorations block a drop only through the passability test 1, not through the
       footprint.
   - **Filters in 0x3b6170** (§4.2.1 test 3):
     - proximity mines;
     - movers, for the AI only;
     - landing transports (item 3);
     - the ignore list: checked only when X's type equals the type of the list's first entry; the list is null for
       every drop caller.
   - **Footprint 0x38de80(X):** an oriented box with axes along X's yaw (+0x8c).
     - **Centre:** X pos + R(yaw)·(`Custom_Hard_XExtent_Offset` +0x16c4, `Custom_Hard_YExtent_Offset` +0x16c8) ×
       `Scale_Factor`.
     - **Half extents** (0x396850), first rule that applies:
       1. X has TEAM (0x16) and its locomotor vf+0x230 is true: the team member span box (0x404450). It never applies
          here, because team containers are not in the tree.
       2. X's type `Deploys` (+0xd1), X has a locomotor and is in its deployed state [M: the state predicate,
          locomotor vf+0x130 / vf+0x120 / obj +0xa8 +0x2b3]: `Custom_Hard_XExtent_Deployed` / `YExtent_Deployed`
          (+0x16cc / +0x16d0) × `Scale_Factor`. If XExtent_Deployed ≤ 0, the normal extents are used for **both**
          axes; the y test also reads +0x16cc, an original quirk.
       3. Otherwise 0x371be0:
          - when `Custom_Hard_XExtent` and `YExtent` are both > 0: each × `Scale_Factor`;
          - else **per axis**: the hard extent if > 0, else |model box half extent| of the current mode's model, then
            × `Scale_Factor`;
          - no model → 0.
          - §4.2.1's "when both are > 0; else model" holds only when both are ≤ 0.
   - Port rule: the blocking set is every live object (or flagged death clone) whose type has `Collidable_By_Projectile_*`,
     of any owner, minus the item-3 / mine / mover filters. Its box is centre + offsets, (deployed) hard extents or
     model half extents per axis, × scale, along its yaw. Replace the port's "battle units + LAND_OBSTACLE, else
     movement radius".

5. **Human `Reinforce` command** (`ReinforceEventClass` vf4 0x403cb0, Execute):
   - **Skipped entirely** when the current mode's +0x28 object has +0x20 set [M: the victory monitor's "battle
     decided" flag].
   - **Position:** re-quantised per axis (0x23bc10 with axes 1, 2, 3) before use. Land is mode type 1.
   - **Unit:**
     - campaign: the pool object 0x28aa50(+0x48); if it is missing → return. Its type is 0x3973b0.
     - skirmish (0x28afb0): type 0x407db0(+0x48).
   - **Population speech:** if the issuer is the local player and has no population room (0x2b03e0), the faction's
     "insufficient" speech (faction +0x6a0) plays. This is local only and plays before the validity test.
   - **Validity:** the **same** vf80 0x3b5f60(mode, issuer, quantised pos, type, force 0, f6 1). Its inputs:
     - the issuer's facing record (item 1);
     - checkRadius 1, fog tested;
     - landingRule 1;
     - movers ignored only when the issuer is not human (+0x62).
     - For a human, these are exactly the preview's arguments (0x345c0: checkRadius 1, fog, movers 0, landingRule 1).
   - The drop proceeds only if vf80 is true **and** the issuer has population room.
   - **Campaign extra refusals**, all silent:
     - the unit's type has `Stay_In_Transport_During_Ground_Battle` (+0x2d);
     - the unit is a transport (+0x337) whose contents (0x397bf0) hold such a type;
     - the battle planet (0x28aaa0 → snapshot +0x80) restricts the type (0x373490: `Planet_Restricted_Unit_Categories`
       +0x808 ∩ `CategoryMask` +0x1648, or listed in `Planet_Restricted_Unit_Types` +0x810).
   - **Spawn:** otherwise 0x2b5fe0(GOM, id, pos, issuer, 1) and the story notification 0x32a910. Skirmish: 0x2b5fe0 when
     the type resolves.
   - **Failure:** refused. The point is not moved or clamped, and there is no retry; the unit stays in the pool and no
     state changes.
   - **Spawn details** (0x2b5fe0):
     - It creates the landing transport (the pool object's type `Landing_Transport_Variant` +0x1e68, else the type
       itself) **exactly at the quantised pos**.
     - z = terrain height + `Hover_Offset` (+0x1b68). For a vehicle-company cargo, the larger of that and the first
       member's model height × scale [M].
     - Its orientation is the issuer's facing record. It attaches TRANSPORT_LANDING and loads the unit.
     - The cargo is unloaded later with the per-team layout (0x643130); the slots are not re-validated then.
   - COMMANDS.md "[M: entry not re-read]" is settled: Execute re-runs vf80 with the arguments above.
   - Port rule: the land `order_reinforce` must re-test vf80 (force 0, landing rule on, movers block for humans) on the
     quantised point plus the population, stay-in-transport and planet-restriction checks, and refuse the whole drop on
     failure. Drop the port's per-slot clamp and nearest-cell move.

6. **Space AI deploys (`TF.Reinforce` → 0x2bf2d0):**
   - The drop test is one point per task-force entry (§4.2 space drop, AI_TACTICAL ring search).
   - Fleet / squadron / company contents are **not** laid out by a formation table. Each member is spawned at
     GOM `Find_Free_Placement` 0x2a9670(P, member type, max radius 2500, start angle 0):
     - rings r = 0, then + 1.2 × the larger model span;
     - 22.5° steps, starting at angle 0;
     - first spot whose model box (z ±1000 in space) overlaps no blocker in GOM +0xd8. Blockers exclude dead objects,
       landing transports, LAND_OBSTACLEs, layer 0x20 and model-less objects.
     - falling back to P.
   - The members are then grouped by the team helper 0x2a5a60. Single units spawn at P. Facing = the player's record.
   - [M: the member-list fields +0x978 / +0x980 and +0x960]
   - Port rule: space AI drops place each member by the free-placement ring search around the validated point; there is
     no company formation in space.

**Still open:** the deployed-state predicate in 0x396850 (item 4, rule 2) [M]; the vehicle-company landing height term in
0x2b5fe0 [M]; the `+0x28 / +0x20` "battle decided" gate of the Reinforce Execute [M]; variant inheritance of
`Collidable_By_Projectile_Living` (assumed from `Variant_Of_Existing_Type`) [M].

> **Status 2026-11-02 (a1h t06):** implemented in the battle host (`TacticalBattle::land_drop_valid`, used by the AI's
> `land_drop_ok` and re-run by `order_reinforce`):
> - item 1: per-side entry-facing record `drop_facing_` (zero at start; the defender's first Defending Forces marker; the
>   chosen Attacker Entry marker only with attacker start units), saved with the battle; the drop yaw reads it. The
>   skirmish spawn-point markers are not used: the port's skirmish land battles go through the campaign placement.
> - item 2: `Unique_Ground_Unit` substitute for members without `Land_Model_Name` (cell, passability, slot box), `Create_Team`
>   from the original member.
> - item 3: human drops ignore `TRANSPORT_LANDING` objects, AI drops count them. [port] No `Create_Cinematic_Transport`
>   persist flag exists in the port, so no landing transport is exempt for humans.
> - item 4: blockers = live objects of any owner with `Collidable_By_Projectile_Living` (death clones: `_Dead`), minus
>   mines / AI movers; footprint centre offset `Custom_Hard_X/YExtent_Offset` x scale along the yaw; per-axis half extents,
>   0 without extents or model (the movement-radius fallback is gone). Rule 2 (deployed extents) is not modelled [M].
> - item 5: `order_reinforce` (land) re-runs the test (movers block only for a human issuer) and refuses the whole drop
>   (`InvalidPosition`); the per-slot clamp and nearest-cell move are gone. The campaign Stay_In_Transport /
>   planet-restriction refusals and the "battle decided" gate are not modelled [port].
> - item 6 (space members by the free-placement ring search 0x2a9670) is **not implemented**: space reserves still spawn in
>   rows of 6 around the validated point. Out of a1h's land scope; needs its own task.
>
> **Status 2026-11-02 (a1i t04 / t05, A1_READINGS §35):**
> - Landing object (§35.1-35.3): a land Reinforce of a company with a `Company_Transport_Unit` spawns its
>   `Landing_Transport_Variant` at the drop point (z + `Hover_Offset`, entry-facing record, parent id, `TRANSPORT_LANDING`,
>   carried type); the members appear at the 0x643130 unload offsets (team grid 3.0 x the team bounds) at the order [port],
>   the signal 0x19 record is posted (`landings_unloaded()`), and the landing object leaves after (steps + 1) x 2 s x
>   (1 + Reinforcement_Deploy_Time_Multiplier list). Battle AI perception reads the landing's carried type as its
>   representative type. `BattleConfig::landing_unload_timed` (off by default) runs the state machine instead: hidden
>   until clear of the other landings, one team / vehicle per step on a locked exit move, unlock + record after the last,
>   saved mid-unload (battle state v3). Open: ~~switching it on (the AI's in-flight wait)~~ (a1j t01: on by default, the
>   tactical AI's TF.Reinforce waits for the signal 0x19 records, A1_READINGS §36), the Land / Takeoff clip times,
>   the skirmish `Reinforcement_Time_Multiplier` wait, the +0xd8 hover term.
> - item 3: `Create_Cinematic_Transport`'s persist flag is stored (`ObjectStore::landing_persist`, save v3); a persistent
>   landing object blocks human drops.
> - item 4 rule 2: deployed extents (0x371d20) while a `Deploys` type with a locomotor has DEPLOY active
>   (`drop_deployed_extents`; FoC sets no `Custom_Hard_XExtent_Deployed`, so no FoC footprint changes).
> - item 5: campaign drops refuse (`NotAllowed`, after the validity) `Stay_In_Transport_During_Ground_Battle` types and
>   types the battle planet restricts. The "battle decided" gate stays [M].
> - item 6: space members by `find_free_placement` (§35.4 item 4) when every member's model box is loaded; rows of 6
>   otherwise [port].

### 4.3 Battle load consumes the sync RNG (item 3) [H draws, M path]
- Object construction draws synced numbers: SAP init 0x37c050 (from `Init_From_Type` 0x3989a0) does one
  `Get_Int(now, now + interval)` per special ability with interval > 0 that is allowed in the mode (first due
  frame stagger); the hardpoint ctor 0x381a90 one per hardpoint; behaviour Initialise one per interval ≥ 2
  behaviour (§6.4).
- Load_Map 0x56480 creates map objects through the mode's load vfunc +0x78 (hidden from the call graph), and
  every map-placed or persistent unit goes through the factory, so **the load consumes draws in creation
  order**. WeatherAudio (0x323110, via 0x364f00) uses the free RNG.
- Port: build the battle on the sim thread in a fixed order (map chunk order, then persistent units in list
  order) through SimRng.

### 4.5 Retreat destination finders (item 5) [H]
All four walk the galactic all-objects list (newest first) over planets (PLANET +0x335), skipping the
excluded planet, +0x3a0 bit 2 and owner −1, and return the smallest squared 3-D distance (0x397640, double,
strict <: first in list order wins ties).

| RVA | Filter (0x2c0510) |
|---|---|
| 0x2a9dd0 | orbit controller = R (0x3f9270: starbase owner when level > 0, else owner of the first non-transit combat fleet in orbit with state < 3, else the first such fleet in transit) |
| 0x2aa400 | ground-base owner (+0x124) = R or starbase owner (+0x13c) = R |
| 0x2a9ee0 | planet owner none or a neutral faction (faction +0x108) [M] |
| 0x2a8dd0 | any planet |

Fleets (0x2ba6c0): dd0 → 400 → ee0 → 8dd0, excluding the battle planet. Ground (0x2ba850): flag clear → dd0
then 400; flag set → 400 then dd0; then ee0, 8dd0 (excluding the source). The first two ground calls pass
**no exclusion**, so the source planet wins at distance 0 whenever R controls its orbit; the retreat fleet has
already entered orbit (0x4338d0, slot −2), so ground units usually just go up into orbit [H code, M effect].

### 4.6 Behaviour 0x5f and property bit 0x2c (item 6) [H]
- **0x5f = DUMMY_GROUND_STRUCTURE** (predicate 0x4c2920). In auto-resolve these galactic ground structures are
  picked first and always die on the losing side (FoC 72 types, TR 39).
- 0x370320(type, b, m) tests the bit for behaviour b in the type's per-mode behaviour bitset
  (type +(0x75 + m)·0x10; m 0 galactic, 1 land, 2 space, −1 → 4 = any). **Bit 0x2c, m = 2 = TACTICAL_SUPER_WEAPON
  in SpaceBehavior** (FoC Death_Star, Death_Star_II; TR Dummy_Planet_Killer): space split rule 1 means "a
  super-weapon always starts".

### 4.7 Second contrast factor and the "planet bonus" (item 7) [H]
- 0x5f22c0 is the AITerrainEffectiveness table (AI_GALACTIC_3 §6.2) keyed by AutoResolve+0x50 and the type's
  MovementClass. **+0x50 is only ever written as −1** (vf1 0x60d040, vf2 0x60cf30): the factor is never
  applied; the S10 choice "factor = 1" is exact.
- 0x374010 is a **per-unit-type bonus over `Abilities`** (+0x1a98), not a planet table. Every special-ability
  class uses base vf7 0x48d320 = (`Unit_Strength_Category` +0x48, `Unit_Strength_Factor` +0x50). In the
  exchange of fire (0x60d2a0), for each combatant whose type has ≥ 1 ability (0x374da0), each set category bit
  gets b = max(table, factor − 1) in a 32-float table (only increases count; initial value assumed 0 [M]).
  FoC: 13 generic-hero abilities use it. **TR: none** (bonus 1 throughout).

### 4.8 Lead object +0x28, victory records 3 / 6, GMM+0x1f4 (item 8) [H unless tagged]
- **+0x28 = tactical id of the spawned super-weapon.** The spawner 0x2a00b0 places it at the marker named
  `DEATH_STAR_POSITION` and writes the id into the parent's space (or land) head +0x28 [M: branch condition,
  probably the "Super" category].
- **Battle-end step 4** (0x3639d0 around 0x364330): when +0x28 resolves, its owner ≠ winner and not dying
  (+0x3a0 & 0x42 clear):
  - winner owns any `Is_Super_Weapon_Killer` (+0x1f) object → the super-weapon is killed (0x391310); if the
    galactic victory monitor has a type-3 condition (`Galactic_Super_Weapon_Destruction`) for the winner, post
    {3, winner, 5} (0x343b90), else galactic mode +0x374 := planet;
  - otherwise **R := winner** (the winner without a killer is the side that retreats);
  - then, unless just killed: monitor has a type-6 condition (`Galactic_Super_Weapon_Destroys_Last_Enemy_Planet`)
    for the super-weapon's owner, and the other side is playable and owns no planet with data +0x2c8 = 0 →
    post {6, owner, 0x1e}.
- **GMM+0x1f4/+0x1f8/+0x1fc is the corruption mission**, not a super-weapon: the corruption action 0x52bf50
  stores (defiler, corruption type, param) via 0x28bbf0 when the planet's Corruption_N block has a tactical
  mode; 0x28bd70 queues a battle record (GMM+0xf0) with `Corruption_N_Tactical_Map_Name` (0x371730), script
  (0x371800) and faction, and sets 0x140b30545. At battle end 0x28bf10: winner = defiler → apply corruption
  (0x3f6810 "increase_corruption"), story event 0x326f20, local movie; else failure event 0x326f50. Either way
  it clears the fields and **skips the normal fleet / ground write-back**. TR has no corruption maps (FoC 29).

### 4.9 Retreat attrition (item 9) [H; M for list +0x160]
- `RetreatCoordinator` vf12 0x3404c0 runs once when the retreat countdown (state 1) ends, before the jump-out
  (from Space vf5 0x5337c0 / Land vf5 0x535b40).
- p = vf13: space 0x533490 = `Space_Retreat_Attrition_Factor`; land 0x535240 = `Land_Retreat_Attrition_Factor`
  + `Blockade_Run_Attrition_Factor` (sum clamped ≤ 1) when any fleet in orbit of the parent planet owned by
  another player, not in transit, has a non-transport member not in transit; the blockade term is skipped when
  any retreating unit's type has `Prevents_Blockade_Run_Attrition` (+0x7a).
- r = the retreating player's tactical list +0x160 (max-then-sum, clamps +0x170/+0x174) [M: attrition
  reduction].
- Retreating units walked **last to first**; each that is not a team container (+0x348 = −1), not a named
  hero (0x38fa20) and has a galactic counterpart (+0x54) makes **one inline synced draw** u; u < p − r → removed
  from the list and killed (`Take_Damage` kind 1, source CRC 0x5d5e0b6b).
- FoC and TR ship 0.0 / 0.0 / 0.2: only land retreats past a blockade lose ~20 %. **The draw happens even when
  p = 0.**

### 4.10 Map-name sources and the random picker 0x47bf40 (item 10) [H unless tagged]
- `Destroyed_Space_Tactical_Map` (planet type +0x6b0): only ctor / dtor / copy (0x2c4370 / 0x2c5b30 /
  0x36c000); no gameplay reader [M]. Corruption maps come through §4.8.
- Picker table 0x140b311a8, built at boot (0x47cb40) from `Data\Art\Maps\*.TED`, `Data\CustomMaps\*.TED` and the
  mod paths; per map (0x47b030) a 0xf0-byte entry: +0x00 name, +0x80 path, +0xa0 kind (1 land, 2 space), +0xa4
  terrain, +0xa8 faction, +0xac player count, +0xb0 max base level [M], +0xb4 / +0xb5 flags.
- Query from 0x28fda0 (kind, terrain, faction, base level, 2, planet map, 0, 0, 0, 0): kind and terrain equal
  (terrain bit 31 = any), faction equal or −1, +0xb0 ≥ base level, +0xac ≥ 2, +0xb4 = 0. Game type 4 adds a
  `CONTROL` path filter and `Quickmatch_Map_Exclusion_List` (GC +0x119c0). Matches in table order; a matching
  preferred name stops collection after it. Result = one `int(0, n − 1)` from the **free RNG** (QPC-seeded);
  no match → empty entry.
- **The original pick is not deterministic.** Port: a seeded deterministic choice; parity only needs a valid
  map, which is then stored on the planet. TR gives 1386 planets fixed `Land/Space_Tactical_Map`, so the
  picker is rare there.

### 4.11 Pending-dialog command path (item 11) [H MP, M SP, L state 1]
- **MP:** vote id 2 (`PENDING_TACTICAL_BATTLE_VOTE`) checked in GMM::Update 0x28d400; when option-0 + option-1
  votes reach the player count (0x294b80), a **ResumeGame (51)** event is queued with bit = (option-0 votes <
  1); timeouts 0x8c270 / 0x8c310 send bit 0. Execute 0x4ae0d0 → 0x28c7e0 (galactic, paused, conflict state 0):
  MP game types (0x35f6b0: 1, 2, 4) bit 0 → state 3 (fight), bit 1 → 0x29c020 (state 6, force flag GOM+0x634
  = 1, auto-resolve); other game types → state 1 [L, no handler found]; then the pause is released. **A single
  option-0 vote means fight.**
- **SP:** the dialog button handler (0x347580, GUI) calls 0x28c7e0 directly, not through the command stream
  [M]. **Retreat:** Withdrawl (49) execute 0x4adf40 → AutoResolve vf5 (`Set_Retreating`) for the player at event
  +0x24.

---

## 5. Fog of war and station targeting (FOG_OF_WAR §9 items 1–4, 7; L1/S4 fog)

### 5.1 REVEAL slot 26 — the attacker flash (FOW item 1) [H]
Only the post-shot hook 0x3a06a0(shooter, target, …), from `HP_Fire_At` 0x3825b0 and WeaponBehavior vf6
0x54d090, calls the **shooter's** REVEAL vf26 0x537390: Flash of the shooter's footprint (0x365630) on the
**target owner's** grid only (not the target's allies). No damage-side caller. Every shot re-flashes, so a
firing attacker stays visible to its victim, regrowing ~6.4 s after the last shot. The hook then calls mode
vf76 (local attack alert, presentation). **Port: flash on fire events, not on damage events.**

### 5.2 Player FOW reveal-range list +0x1f8 (FOW item 2) [H; M which players]
Battlefield_Modifier stores **`FOW_Reveal_Range_Multiplier` − 1** (§2.3.11): shipped 1.5 → 0.5 → net ×1.5;
TR 1.06–1.18. The (1 + Σ) formula of FOG_OF_WAR §3.1 is therefore right as written.

### 5.3 Battle-record slots and the battle scale (FOW item 3) [H]
- In both land and space records **+4 = attacker, +8 = defender** (Land_Invade 0x2b0520 and
  Initiate_Space_Conflict 0x2af7c0 fill (planet, attacker, defender)). `Reveal_For_Attacker` → +4,
  `Reveal_For_Defender` → +8.
- Battle scale 0x35cd90 sums `Reveal_Range_Modifier` over **land: the attacker's objects, scaling the
  defender's radii; space: the defender's objects, scaling the attacker's radii**; clamped to [0.25, 1].
- Shipped users: Underworld_Sensor_Scrambler (−0.75, land turret), E/R_Orbital_Jamming_Station (−0.5),
  Destroyable_Asteroid_* (−0.5, normally neutral). TR: none.
- **The port is wrong for land** (it scales the attacker by the defender's objects).

> **Status 2026-10-30:** Fixed. `battle_scale_sides` in `engine/tactical/fog/src/fog_of_war.cpp:22-30` now sums the attacker's objects and scales the defender's radii for land (reverse for space); the clamp to [0.25, 1] is `battle_reveal_scale` at `fog_of_war.cpp:16-20`.

### 5.4 Post-setup re-reveal (FOW item 4) [H]
Setup start (0x3c0680) disables every behaviour through vtable +0xc0 (slot 24, disable counter +0x3c += 1),
except IDLE 7, SELECTABLE 2, HIDE_WHEN_FOGGED 0x19, TRANSPORT_LANDING 0x36, TACTICAL_BUILD_OBJECTS 0x29,
TACTICAL_UNDER_CONSTRUCTION 0x2a, TACTICAL_SELL 0x2b. Setup end 0x3bfba0 re-enables them through +0xb8
(slot 23, 0x4c30a0: counter − 1, floor 0); in space it also re-enables the container's TEAM. **Nothing marks
REVEAL dirty**: stationary units' cells regrow ~6.4 s after setup end until they move one cell (keep the
quirk). Moot for parity: `SetupPhaseEnabled` is False in FoC and TR.

### 5.5 Space reinforcement FOW sampler and palette 3 (FOW item 7) [H]
`SpaceReinforceFOWSampler` vf0 0x4d92d0(mode, player, pos) = 0 when a non-allied object's
`Reinforcement_Prevention_Radius` covers pos (2-D, strict <; GOM list +0x2f0, 0x4d8d10), else 0xFF: the
palette-3 overlay (`SpaceReinforceFOWColor`), render only. The sim rule is SpaceMode vf80 (§4.2), which also
requires the point unfogged.

### 5.6 Station "protection" test 0x540140 [H; M for an empty list]
It is the hardpoint **`Fire_Category_Restrictions` gate** (TACTICAL_COMBAT §2.6), not a protection flag:
passes for a null target; otherwise needs the target's unit-type CategoryMask (0x3973b0) ∩ the hardpoint mask
(+0x408, built from the list at +0x3f0) ≠ 0 — an allow-list ("Air" = fire only at Air). TR uses it on 566
hardpoints. The Lua station fire path should apply it.

### 5.7 Station readiness 0x385c00 [H]
0 unless the hardpoint type (+0x20 → +0x48) is 5..10. 1.0 when cooldown +0x58 = 0 and pulses +0x5c = 0.
Otherwise (F − t0) / (cooldown + F − t0), F = mode frame, t0 = +0x68 (cooldown start stamp), float32: elapsed /
(elapsed + remaining). Cooldown 0, pulses > 0, F = t0 gives 0/0 = NaN, which the "< 1 → not ready" test treats
as ready; keep it.

### 5.8 Targeting exemption obj +0x343 and the FOW force flag [H, disassembly]
- obj +0x343 = behaviour index slot for t = 0x11, the **MARKER** behaviour (+0x332 + t). In
  `Validate_Current_Target` 0x567c50 and `Scan` 0x5662b0 a MARKER candidate skips the FOW test and is accepted
  only by a shooter that is a member of an active bombing run (BombingRunManager mode +0x38, 0x4944e0); others
  get reject code 0xb.
- **The targeting FOW test calls 0x35f470 with force = 1** (`mov r9b, 1` at 0x140567d8d and 0x140566574).
  Fogged enemies with TEAM or HIDE_WHEN_FOGGED are untargetable for **AI players as well as humans**;
  FOG_OF_WAR §5.3 and TACTICAL_COMBAT §2.3 said force 0.

> **Status 2026-10-30:** force = 1 is implemented (`engine/tactical/combat/src/combat_system.cpp:690-692`, `target_hidden(..., true)`). The MARKER exemption (+0x343, bombing-run markers skip the FOW test) is not modelled yet, because the port has no marker objects.

---

## 6. Object runtime, movement, AI zones and models

### 6.1 A1 land zone graph: Edge_Weight / Node_Weight [H; direction now H, A1_READINGS §2]
`LandExecutionPathFinderClass` (vtable 0x890498) reuses the galactic all-pairs recompute 0x6913e0.
- Nodes (vf1 0x694140): the land mode's zones (mode vf +0x188 count, vf +0x190 get) in index order.
- **Edge_Weight(i → j)** (vf2 0x694010): −1 unless zone j's link table (keyed by zone i, 0x5a9af0, mask −1)
  has a link to i with non-zero flags [M on direction]. Else hop = (uint)(int64)(3-D distance between zone
  centres +0x30..+0x38 / **10.0**) & 0xFFFF; n = stored node cost of j; result = n × 65536 + hop, and
  0x10000 + hop when n = 0 (galactic gives just hop).
- **Node_Weight(i)** (vf3 0x6940e0): 0 when the AI player's +0x360 is null or the threat grid 0x140b31900 is
  absent; else (int64) max(0, Σ threat over zone i's cell list (zone +0x90)). The sum (0x653e90) reads the live
  threat grid (+0x28) for a non-human player when `AIUsesFogOfWar` for the mode is off (shipped off);
  otherwise per cell live (+0x28) or memory (+0x58) by the visibility test 0x656fe0. Node cost =
  (uint32)value / 100.
- Port: replace the `(uint32)trunc(XY distance)` default in zone_reachability.hpp.

### 6.2 "Garrison container → idle animation" is presence-induced idle [H]
Default idle type (0x395920): (1) SPREAD_OUT active → **71 Idle Crouch**; (2) else, when the object's IDLE data
(obj +0xd8 → +0x18 map presence-object id → remaining frames +0x14) is non-empty, pick a winner (strictly
larger remaining wins; on ≤ an allied presence object, 0x2823e0, replaces a non-allied best), and the idle =
the first `Presence_Induced_Animations` entry (type +0x1610: anim type + type list, 0x373530) whose list
contains the winner's type; (3) else 0.
Writer: **IMPOSING_PRESENCE** (0x624ab0, interval 30): AABB query of half-extent `Presence_Range`
(+0x1628); each object not flagged 0x42 whose type lists the presence type (0x370630) and has IDLE gets an entry
(0x62e450) with remaining = int(FPS × `Unit_Presence_Relaxation_Time` (GC +0x117a8)); skipped when the
**receiving** unit (the IDLE owner, not the presence unit; corrected s78l t03) has a target or moves without
`Allow_Idle_When_Moving` (+0x85). Lifecycle of the records: TACTICAL_COMBAT slot 47 "s78l t03". FoC 5 entries (Attention near
Palpatine / Vader), TR 26. No container-kind table; garrisoned units are hidden.

### 6.3 Mesh `_ALT` selection 0x3ac290 and the "space damage-model swap" [H]
- Callers: hull damage 0x3ab890, post-load 0x3a0930, Force_Healing 0x6fb940, Repair 0x70cb90, Drain_Life
  0x71b5d0, death 0x58cdb0, 0x295cf0. **No mode test** — it runs in space too (FoC 8 / TR 28 space types carry
  thresholds).
- Rule: f = health / max (team container: team fraction 0x405230). Walk **every** `Land_Damage_Thresholds`
  entry in list order; each with f ≤ t_k sets alt := `Land_Damage_Alternates[k]` (last match wins, default 0).
  Applied only if alt ≤ the model's highest ALT index (0x12dca0). On change obj +0x300 := alt and
  `Land_Damage_SFX[alt]` plays ("NULL" skipped); model instance +0xc4 := alt.
- Data quirk: thresholds `1` / alternates `3` (GROUNDBUILDABLESSKIRMISH) shows alt 3 at full health.
- **TACTICAL §3.6 step 18 correction:** the 75 % / 50 % crossing attaches `Damaged_Smoke_Asset_Name` (type
  +0xc58, "Data\Art\Models\" + name) when set (FoC 7 / TR 19 types, all land). There is no separate space model
  swap. The port's R1 rule ("last threshold at or below", land only) should drop the land-only restriction and
  apply the last-match walk.

### 6.4 Behaviour scheduling (OBJECT_RUNTIME open item 7) [H]
- Advance: base `BehaviorClass::Service` (vf6 0x4c2f70) sets +0x30 := frame + +0x34; derived services call it.
  Some behaviours set +0x30 directly (STUNNABLE, CONFUSE: to their end frame).
- +0x34 is cached at attach (0x4c3710); SHIELDED, POWERED (energy pool) and INDIGENOUS re-cache on every
  service, so multiplier changes apply from the next schedule.
- **First due frame** (base Initialise vf9 0x4c2f90; not on load): interval < 2 → +0x30 = 0 (due at once);
  else +0x30 = mode frame + **synced `Get_Int(0, interval − 1)`** (0x1ffb40 on 0x140a13e24). Every attach of an
  interval ≥ 2 behaviour consumes one synced draw, in attach order. Disable counter +0x3c := 0 (then the
  start-disabled classes of §6.6 disable themselves).
- "Computed at attach" intervals (vtable +0x28):

| Behaviour | Interval |
|---|---|
| LAND_OBSTACLE (0x4f8840) | 0 or 1 (every frame either way) |
| SHIELDED (0x5494b0) | int(FPS × SHIELD_REGEN_INTERVAL product × `ShieldRechargeIntervalInSecs` (GC +0x354) + 0.5) |
| POWERED = `EnergyPoolBehaviorClass` (0x56bfd0) | int(FPS × ENERGY_REGEN_INTERVAL product × GC +0x358 + 0.5) |
| BASE_SHIELD (0x56ac80) | FPS / 2 = 15 |
| INDIGENOUS_UNIT, HUNT, GUARD_SPAWN_HOUSE (0x621840) | 1 while burning (obj +0x3a1 & 2), else 30 |

### 6.5 Field codes 0x43 / 0x52 (type loader 0x51a280) [H]
- **0x43:** plain string assign, name resolved at use time: `Projectile_Blast_Area_Immune_Faction` (type +0x490),
  `Item.Faction`.
- **0x52:** combat-mod enum: token strtok'd, upper-cased, matched against table 0x140a434e0: DEFENSE 1,
  DAMAGE 2, HEALTH 3, SHIELD 4, ENERGY 5, SPEED 6, FIRE_RATE 7, FOW_REVEAL_RANGE 8, FIRE_RANGE 9,
  ABILITY_RECHARGE_TIME 10, TACTICAL_BUILD_TIME 11; stored int32 = modifier list index + 1; unknown token leaves
  the field unchanged. Field: `Projectile_Combat_Mod` (+0x2430, §1.10). FoC uses SPEED, DEFENSE, DAMAGE; TR
  SPEED ×8, DAMAGE, FIRE_RATE, DEFENSE.

### 6.6 Behaviour disable counter (+0x3c) [H]
- Serviced only when the counter is 0 (0x4c3700); "behaviour active" for the ability behaviours (HUNT, LURE,
  DEPLOY_TROOPERS in `Is_Unit_Ability_Active`) is the same test, counter = 0. 0x4c36f0 is the inverse
  ("disabled", counter > 0).
- Base Enable vf23 0x4c30a0: counter = max(0, counter − 1); base Disable vf24 0x4c30b0: counter + 1.
- Start disabled: HUNT, LURE, SELF_DESTRUCT (0x628800); DEPLOY_TROOPERS, BARRAGE, LANDING (0x58f870).
- Writers: `Toggle_Behavior_Ability` 0x3a4070 (on → Enable, starting the countdown when the counter reaches 0;
  off → Disable, then recharge); WeaponBehavior Disable (0x54ac30) also hides muzzle meshes; setup start / end
  (§5.4); stun (§1.10); `Do_End_Cinematic_Cleanup` (§7.2) disables every behaviour.

### 6.7 S7 collision priority sort [H]
- Driver 0x50a780 (per frame): collisions whose vf +0x50 says keep are collected in list order; the rest are
  cancelled (vf +0x20).
- Sort: MSVC `std::sort` (0x503f10), **ascending** by `CollisionClass::vf12` 0x5c7700 (insertion sort for ≤ 32
  entries, stable on ties; introsort above).
- Resolve each in order with vf +0x20 when the dynamic land pathfinder budget (0x679740) allows, else vf +0x18.
- **Priority = crowd − (float)(now − creation frame +0x64)**; crowd refreshed every 30 frames (now ≥ +0x6c) =
  5 × the number of land object-grid objects (0x5d7a00) in a 160 × 160 box centred on the XY midpoint of the
  first two participants (0 with fewer than 2). Older, less crowded collisions resolve first.
- Port: stable sort by (priority, insertion order) — exact up to 32 collisions per frame.

### 6.8 AI hint zones with duplicate zone names (A1 t03, 2026-10-31) [H unless tagged]
**Short answer.** The original never links zones by name. Zone-graph links resolve by the zone's 02 id.
Region targets are built per zone by pointer. Names matter in two places only:
- **the perception name id:** the second zone with a repeated name shares the first zone's id, and its
  location evaluator is refused, so `Variable_Target.*` for the second region reads the **first** zone;
- **the post-load relink** (0x6b9580), which resolves every region by name to the **first** zone in list order.

**Shipped data (FoC maps.meg + Patch2.meg, parsed 2026-10-31).** 11 repeated names in 5 maps, each one repeated twice:
`_LAND_PLANET_HONOGHR_01` ("New Zone 106"), `_LAND_PLANET_SALEUCAMI_01` ("New Zone 101"),
`_LAND_PLANET_TARIS_INTIMIDATION` ("New Zone 57"), and `_LAND_PLANET_UTAPAU_01` and `_MP_LAND_UTAPAU` ("New Zone 85"–"88" each).
The repeated zones are distinct: different 02 ids, centres, links and cells. Every name repeats with the same case. No map
repeats a 02 id, and every child E link key resolves to a zone id of the same map (8245 FoC zones, 44197 links, none
unresolved). EaW maps.meg has no repeated names.

**Zone layout read by the loader** (land mode vf46 0x3b8bc0 → one `AIHintZoneClass` (0x158 B) per 0x10B child,
appended to mode +0x3a0; reader 0x5a9100):

| .ted part | Zone field | Meaning |
|---|---|---|
| child 0, mini 01 | +0x10 string | name |
| mini 02 | — | **editor pointer id**: registered as "old pointer → this zone" in the global save/load pointer-remap table (0x1fe2d0) |
| minis 03, 04, 06, 0D | locals, +0x3c, +0x150 | not read by the AI |
| mini 05 | +0x30..+0x38 | centre |
| minis 08 + 09 | +0xd0 map name → u32 | per-zone **hint** values, keyed by hint name. Read by the land movement zone path cost 0x5a6940 through 0x5a8f50. Present in 36 EaW zones and no FoC zone. These are not zone links |
| child 7 (minis 01 u32 `x \| y<<16`, 02 u8) | +0x50 hash set | the zone's cells; the point-in-zone test is 0x5a9a30 |
| child E (per link: mini 01 key (u32 in this .ted format, u64 otherwise; flag 0x1fed00), mini 02 u32 flags) | +0x110 map: zone pointer → flags | **links by id**. Each key is queued for pointer remapping (0x1fe4a0). A post-remap callback (0x1ff040 → 0x3bc7c0 → 0x3bce10) inserts (remapped zone pointer, flags) into the map |
| child F | +0x90 list | empty in every shipped zone |

So `Edge_Weight` (§6.1, 0x5a9af0 on +0x110) and the movement zone path finder see each duplicate as its own
node with its own neighbours. **Zone names play no part in the graph.** The port's id-based links
(`HintZone::Link::zone` = target 02 id, `ZoneGraph::index_of_id`) match the original.

**Region targets at battle start** (land perception slot 6 → 0x6b8d80). For each zone in index order (mode vf
+0x188 count, vf +0x190 get):
1. The zone name is looked up in the global name-id table (0x140b310a8 vf +0x20). If it is absent, it is added with id = count + 1.
   **A repeated name returns the id of its first zone.** [H] (Case: the table's comparison is not traced [M]. Shipped
   repeats have identical case.)
2. A `LandLocationPerceptualEvaluatorClass` is built for **this** zone (0x6d55a0). It is registered under the name id by
   0x4db4f0, which **refuses an id that is already registered** (returns 0; the first evaluator stays). [H]
3. The region target is built with **this** zone's pointer (+0x28), centre (+0x1c..+0x24) and its own
   `ZoneReachabilityClass` (0x7253d0 → 0x73f1d0). It is stored in the zone → target map +0xf8 (key = zone pointer;
   lookup vf10 0x6b8fd0) and appended to the target list. [H]

Result: both duplicates are separate targets, with their own positions, their own reachability objects and their own zone
→ target entries, but **one name id**. Every perception evaluation binds `Variable_Target` = the name id (AI_TACTICAL
§3.5). For the second target it therefore evaluates the **first** zone's location evaluator: its force sums,
`IsContestable`, `IsFriendlyStartLocation` distance, threat cells, and so on. [H for the refusal, M for "evaluation looks
the evaluator up by the bound id"] `FindTarget` / `Reachable_Target` can still return the second target. Its
position (slot 13), range test and reachability are its own.

**Post-load relink** (land perception vf12 0x6b9580; land mode only, mode vf +0xe0 == 1) [H body, M "runs after load"]:
- Run the base vf12 (0x4df960: re-register the saved evaluators and the `PLAYER_%d` ids).
- Clear the zone → target map +0xf8.
- For each target in list order that has no game object (slot +0x30 == 0, i.e. a region): take its name (slot +0x50), then
  0x3b7b60 = **the first zone in +0x3a0 order whose name `_stricmp`-equals it**. If one is found, set map[zone] = target
  (the later target overwrites) and set target +0x28 = zone.
- With duplicates, both region targets point at the first zone after a load. map[first zone] = the later target. The
  second zone has no entry, so vf10 on it returns null. A region whose name matches no zone keeps its saved zone field.
  0x3b7b60 is called only from here: battle start never resolves zones by name.

**Port consequences.**
- Graph and links: no change (by id).
- The region evaluator of a duplicate-named zone must evaluate the **first** zone with that name. Today the port
  evaluates each region target with its own `zone` index. Map each region to the evaluator zone =
  `first index with an equal name`. Keep `position`, `zone` (reachability) and the target order per zone.
- The port saves in its own N1 format, and loading original saves is out of scope (CLAUDE.md). So the post-load
  collapse need not be reproduced. A port relink should restore `zone` by **index or id**, not by name. Today
  `TacticalTargetList::find_region` / `ZoneGraph::index_of_name` return the first match, which reproduces the original
  collapse. Use them for diagnostics only.

---

## 7. L1 systems

### 7.1 TSW debris [H; M store and owner]
TacticalSuperWeapon vf6 0x42a7b0:
- State 9: pick one `Death_Explosions` entry of the target dummy (data +0x10, type +0xce0) with `Get_Int` on
  the **free RNG (not synced)**; spawn it in the mode's +0x20 store (cosmetic [M]) owned by the first player
  whose faction +0x108 is set (neutral [M]) plus FX 0x2cdbb0; play `Death_SFXEvent_Start_Die`; remove the target
  (0x391310); store its position / facing; state 10 countdown := `TSW_Explosion_Debris_Creation_Frame_Delay`
  (type +0x214c; TR 33).
- State 10 expiry: spawn **every** `Death_By_TSW_Replacements` entry (list +0xd20) of the object at data +0x18
  (the planet prop) into the **primary** store (shipped `Destroyed_Planet_Remains_Part_1`, `_Part_2`): same
  position / facing / owner, ids in list order, no draws. State 12 countdown := `TSW_Post_Destruction_Wait_Frames`
  (+0x21ac) → state 13 resets the TSW to 0.

### 7.2 0x38ed10 = per-object `Do_End_Cinematic_Cleanup` [H; M +0x3c9]
Lua entry 0x541e20 queries all objects of the argument player (0x2a9ff0, unbounded). Per object, tactical modes,
unless obj +0x3c9 is set: (1) disable every behaviour (never re-enabled); (2) hide the model (0x264c80); (3)
remove from the land collision / object grid (0x508ec0 → 0x20abe0); (4) teleport to (1, 1, 0) (space: z +=
`Layer_Z_Adjust`) via 0x3abb80, team members offset alike, stop the locomotor (0x39bcb0). Then the collision
resolve pass 0x50a780 runs.

### 7.3 Base power (`Requires_Base_Power`) [H]
- Player state: a per-player tactical list (mode vf +0x220, list +0x198) of bool entries. **Has base power**
  (0x527520 → 0x2800d0) = AND of the entries with optional clamps; **empty list = false**.
- Writer: `Base_Power_Ability` (apply 0x6efaf0, remove 0x6eff70; tactical only): for every player allied to the
  source (owner included, 0x39a2c0), a permanent true entry keyed by the source; remove erases them. Holders: FoC
  `Power_Generator_E/R`, `Hutt_Power_Generator`, `U_Ground_Palace`; TR `Power_Generator`.
- Readers: energy pool 0x56c030 (energy 0); turrets 0x632270 (do not act); gravity control field 0x61c700;
  lobbing super-weapon 0x4ea3f0 and the FIRE_LOBBING gate in 0x39b480; shield 0x56bb00
  (`Shield_Requires_Base_Power` +0x8c); GUI warning 0x2e0570; reinforcement prevention (0x3b6170, via POWERED).
- Users: FoC `Ground_Magnepulse_Cannon`, `Template_Ground_Turbolaser_Tower`; TR `Template_Base_Shield`,
  `Template_Planetary_Shield`, `Template_Turbolaser_Tower` and variants.
- **Port: the "Requires_Base_Power passes" stand-in is wrong for TR** — towers and shields are dead without a
  generator.

### 7.4 Weather immunity list [H; M faction +0x20a8]
- Player +0x410: a timed bool list (value +0x10, expiry +0x14; purged on read by 0x282290). **Weather-affected
  = empty list, or the AND of the values** (clamps +0x420..+0x423).
- Writer: `Weatherproof_Ability` (apply 0x711720, remove 0x711a00) inserts **false**, permanent, keyed by the
  source: with `Apply_To_All_Allies` (+0x178) into every player allied to the owner whose faction +0x20a8 flag
  is set [M]; else the owner only.
- Users: FoC `RL_/EL_Weatherproof_Upgrade` (Ground_Automatic, all allies), `Felucia_Plantary_Advantage`. TR: none.

---

## 8. Status and what each answer unblocks

| Item | Status | Unblocks |
|---|---|---|
| §1.1 info-table defaults (HUNT/LURE M = 1) | H | engine/tactical abilities |
| §1.2 Active_By_Default | H | abilities spawn; A1/A2 free-store / task-force hooks |
| §1.3 autofire driver (in sim, synced pick) | H | abilities, combat targeting, AI |
| §1.4 special-ability intervals | H | abilities SAP update |
| §1.5 tractored / interdict / jammed readers | H (interdict return code M) | abilities, combat projectiles, S10 retreat, L1 Lua |
| §1.6 converted decay bounds | H | combat / abilities RNG count |
| §1.7 missile-shield emitters | H | combat missile shields |
| §1.8 burning | H (no expiry M) | combat |
| §1.9 spin-away / survivors | H | combat death, object store |
| §1.10 detonation effects / land stun | H dispatch, M effects, L weaken lists | combat projectiles, abilities |
| §1.11 ion / attack recharge | H | abilities |
| §1.12 hardpoint repair (command 45) | H | combat hardpoints, command host |
| §2.1 handler bodies | H (SPREAD_OUT spacing M, EJECT_VEHICLE_THIEF L) | abilities, movement (deploy, jet pack locomotor still open) |
| §2.2 HUNT / BARRAGE / SELF_DESTRUCT / DEPLOY_TROOPERS / CONFUSE | H / H / H / M / H; **probe**: HUNT RNG volume | abilities, AI, fog queries |
| §2.3 tactical special classes | H (animation-gated parts M, need sim animation); **probe**: Lightning through shields, Earthquake timer pause | abilities, anim |
| §2.3.11 Battlefield_Modifier values | H | abilities, fog, battle host reinforcement timer |
| §3.1 slots 44 / 45 | H | combat targeting |
| §3.2 constants | H | combat, AI |
| §3.3 diminishing firepower | H (pair reading M) | combat |
| §4.2 land split / drop validity | H (formation layout M) | apps battle host, reinforcement |
| §4.3 RNG during load | H draws, M path | battle host setup order, Q1 CRC |
| §4.5 retreat finders | H | engine/galactic conflict |
| §4.6 0x5f / bit 0x2c | H | galactic auto-resolve, force split |
| §4.7 contrast factor / ability bonus | H | galactic auto-resolve |
| §4.8 super-weapon / corruption branch | H (spawn condition M) | battle host, galactic write-back |
| §4.9 retreat attrition | H (+0x160 M) | tactical retreat coordinator |
| §4.10 map sources / picker | H | galactic conflict (host map choice) |
| §4.11 dialog path | H MP, M SP, L state 1 | galactic, COMMANDS (ResumeGame 51, Withdrawl 49) |
| §5.1 attacker flash | H | tactical fog |
| §5.2 reveal list value | H | tactical fog |
| §5.3 record slots / battle scale | H — port wrong for land | tactical fog |
| §5.4 post-setup re-reveal | H (moot: setup off in FoC/TR) | tactical fog |
| §5.5 space sampler / palette 3 | H | render overlay, reinforcement |
| §5.6 station category gate 0x540140 | H | combat, L1 station path |
| §5.7 station readiness 0x385c00 | H | L1 station path |
| §5.8 +0x343 MARKER / FOW force 1 | H | combat targeting |
| §6.1 zone Edge / Node_Weight | H (link direction M) | AI tactical (A1) |
| §6.2 presence idle | H | anim (sim side), objects |
| §6.3 `_ALT` rule / damage smoke | H | render, objects |
| §6.4 behaviour scheduling | H — attach draw changes goldens | objects |
| §6.5 codes 0x43 / 0x52 | H | objects type loader |
| §6.6 disable counter | H | objects, abilities |
| §6.7 collision sort | H | tactical movement |
| §7.1 TSW debris | H (store / owner M) | tactical L1 systems |
| §7.2 0x38ed10 | H (+0x3c9 M) | Lua bindings, objects |
| §7.3 base power | H | abilities, combat |
| §7.4 weatherproof | H (+0x20a8 M) | tactical weather |

> **Status 2026-10-30:** Row "§5.3 record slots / battle scale: port wrong for land" is out of date; the land scale is fixed (`engine/tactical/fog/src/fog_of_war.cpp:22-30`). Row §5.8 is half done: force = 1 is in `combat_system.cpp:690-692`, only the MARKER exemption is missing. Row §6.1 (land zone edge weight) is implemented in `engine/ai/tactical/src/zone_reachability.cpp:26-38`.

**Still open after this pass:** ABILITIES §8 item 13 (missile shields turning away lasers — probe); the
EJECT_VEHICLE_THIEF inline handler; DEPLOY_TROOPERS spawn counts; JETPACK locomotor flight (MOVEMENT §8
item 10); stun release in the STUNNABLE service; SPREAD_OUT spacing; Blast / Leech multiplier application;
BT items 1, 4, 12 (probes). Since answered: EJECT, DEPLOY_TROOPERS, jet pack, stun release, Blast / Leech
(REMAINING_GAPS §5); JETPACK and the other locomotors (REMAINING_GAPS §4.3–§4.7).

**Runtime probes worth preparing** (`tools/probe_mod/`, Gabriel launches): (a) a HUNT unit with an RNG-state
log per decision; (b) Force_Lightning on a full-health shielded target; (c) Earthquake damage timer across a
pause; (d) the Interdictor missile shield vs a laser frigate (ABILITIES §8 item 13).

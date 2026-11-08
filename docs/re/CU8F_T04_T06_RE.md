# CU8F t04 / t06 (RE): stickiness and prefer-ground Lua setters, unread ability fields (2026-11-07)

Roadmap group cu8f. StarWarsG.exe FoC x64 (Steam), RVAs. Tags: [H] read from code, [M] one inferred link, [L] unverified.
No decompiled code is reproduced. "TD" = the targeting data pack (`TacticalCombatantDataPackClass`, obj +0xf0, ctor 0x380540);
NOW = global logic frame 0x140b0a320; FPS = 0x140b0a340 (30). Behaviour slot bytes: obj +0x332 + behaviour id (0x333
locomotor, 0x335 planet (3), 0x337 TRANSPORT (5), 0x33a PROJECTILE (8), 0x348 TEAM (0x16), 0x35e TACTICAL_SUPER_WEAPON (0x2c),
0x370 DAMAGE_TRACKING (0x3e)); −1 = absent.

There is no Lua method called `Set_Stickiness`. The port hook `set_stickiness` corresponds to
`GameObjectWrapper.Set_Targeting_Stickiness_Time_Threshold` (0x580ac0) and `TaskForceClass.Set_Targeting_Stickiness_Time_Threshold`
(0x6003a0); `set_prefer_ground` to `GameObjectWrapper.Set_Prefer_Ground_Over_Space` (0x57d460). No shipped FoC or EaW Lua
script calls either of them (all 584 extracted scripts searched) [H].

---

## 1. t04a: targeting stickiness override (TD +0x158)

### 1.1 The field [H]
- TD +0x158, a 32-bit float. The TD ctor 0x380540 writes **0.0** there (store of a zeroed register at 0x3806cd). Nothing else
  initialises it: the TARGETING attach (vf9 0x5670b0) and the pack-creation helpers (0x566160, 0x390fc0) do not touch it.
- Writers (all through 0x568df0 = "TD of this TARGETING behaviour's owner, +0x158 := value"; its only three callers):
  1. Lua `GameObjectWrapper.Set_Targeting_Stickiness_Time_Threshold(seconds)` 0x580ac0: dead object → warning, nothing;
     not exactly one argument, or not a number → script error. The Lua number is narrowed double → float and written to
     the object's own TARGETING (behaviour 9) and, when the object has TEAM, to every member's TARGETING in team-list order.
     Any value is accepted (negative, zero, huge).
  2. Lua `TaskForceClass.Set_Targeting_Stickiness_Time_Threshold(seconds [, filter])` 0x6003a0: same write to each matching
     member and its team members (filter rules in `L1_S4_REMAINING.md` §TaskForce).
  3. Task-force release 0x5ff610 (TaskForceClass vf49, the unit leaves a task force; `A1_READINGS.md` §39.2): writes
     **the type's `Targeting_Stickiness_Time_Threshold` (type +0x528)** into the unit's TD, then into each TEAM member's TD
     (each member's own type). For the unit itself the type used is the type of the first member of its team data
     (obj +0x110) when it has one, else its own type. The same call writes `Targeting_Priority_Set` (+0x520) into TD +0x160.
- Saved: TD micro-chunk **0x21** (save 0x381170 writes 4 bytes; load 0x380cc0 reads it back) [H].
- Type side: the type ctor 0x367250 zeroes +0x528, so a type without the tag has 0.0 [M: 0x367250 is the GameObjectType ctor];
  type copy 0x36c000 copies it. The only other reader of type +0x528 is the stickiness test below.

### 1.2 The reader: `Is_Target_Sticky` 0x569000 (TARGETING behaviour, owner = behaviour +0x28) [H]
1. **Time to kill of the target** T (null target → not sticky):
   - T is a TEAM container: the sum over its members (team list order) of each member's time-to-dead, for members that
     have DAMAGE_TRACKING (0x3e); members without it add 0. An empty team gives 0.
   - otherwise: T needs DAMAGE_TRACKING (else not sticky); its time-to-dead.
   - Time-to-dead 0x58ba40: health = the sum of max(0, health) of T's **destroyable** hardpoints (hardpoint type byte +0x4d)
     when T has at least one, else T's hull health (obj +0x5c); plus 0x396420 (shield); divided by the damage rate at
     DAMAGE_TRACKING data (obj +0x1a0) +0xc. No data pack or rate 0 → 1e18.
2. **Threshold** = the owner's TD +0x158. Only when it is **negative** is it replaced by type +0x528, read at every test,
   from the owner's type, or, when the owner has TEAM, from the type of the first member of its team data (no team data or
   no first member → not sticky).
3. Sticky ⇔ time-to-kill ≤ threshold (float compare).
- Callers: the targeting services 0x62eeb0, 0x634810 and `Service_Space` 0x633ae0. In each, a valid current target is
  kept without a rescan when it is sticky or its priority score is exactly 1.0; otherwise the unit rescans and may switch
  (`TACTICAL_COMBAT.md` §2.5 / §2.8).

### 1.3 Consequences for vanilla [H for the reads, M for the gameplay effect]
- At spawn every unit has threshold **0.0**, not its XML value. 0.0 is not negative, so the type value is not used, and a
  target is sticky only when its time-to-kill is ≤ 0 (health + shield = 0 at a positive damage rate, or an empty team).
  So in practice **no unit is sticky** until something writes TD +0x158.
- Vanilla Lua never writes it, so the only writer in a vanilla game is the task-force release: an **AI** unit that has
  been in a task force and was released gets its type's value (5 s for 115 FoC types, 30 s for 9, ...). Human units, and
  AI units never released from a task force, stay at 0.
- The value is **not** cleared afterwards: it stays until the next release or Lua write, and it is saved.
- A negative Lua value means "use the type's value" (re-read each test). A negative XML value is therefore "never sticky".

### 1.4 Port gaps (for the implementing agent)
- The port (`CombatSystem::sticky`, `combat_system.cpp`) always uses the type value (`UnitDesc::stickiness_threshold`) and
  treats a threshold ≤ 0 as "not sticky". To match: add a per-unit float override initialised to **0.0** (saved), use it
  when ≥ 0, else the type value (team containers: first member's type); write it from the Lua setters (unit + team
  members) and from the task-force release (`TacticalAiHost::unit_left_task_force`, which A1 §39.4 says needs no
  stickiness reset; that note is wrong: it must write the type value).
- Two time-to-kill differences in the same function: retail sums **destroyable** hardpoint health whenever the target
  has any destroyable hardpoint (the port only does it for `Destroy_When_All_Hardpoints_Destroyed` types), and a team
  target is the **sum of the members' own time-to-dead** (the port uses the container's health).
- This changes targeting for every unit that is not an AI unit released from a task force, so goldens will move.

### 1.5 Probe (to confirm 1.3 in game; not needed for the reads)
Skirmish space map, one human frigate with `Targeting_Stickiness_Time_Threshold` 5.0 (e.g. a Nebulon-B) attacking an
enemy corvette that will die within ~3 s, while a second enemy of a higher-priority class enters its range. Run A: no
Lua. Run B: a probe script calls `Set_Targeting_Stickiness_Time_Threshold(-1)` on the frigate at start (= use the XML
5 s). Observe whether the frigate switches away from the dying corvette. Expected: A switches (threshold 0), B keeps the
corvette. If A and B behave the same, the spawn default is the XML value after all.

---

## 2. t04b: `Set_Prefer_Ground_Over_Space` (UNIT_AI pack +0x39)

### 2.1 The setter and the field [H]
- 0x57d460: no argument → script error "Missing required boolean parameter"; object dead or without UNIT_AI (0x12) →
  script error "Object invalid or missing Unit_AI behavior". Otherwise 0x4f6460 writes the bool to **AIDataPackClass
  (obj +0x108) byte +0x39**. A non-boolean argument is read as a null object (the original dereferences it).
- Default **true**: the AIDataPack ctor 0x550ed0 stores 0x0100 as a 16-bit value at +0x38, so +0x38 = 0, +0x39 = 1.
- Saved: AIDataPack micro-chunk **0xf** (load 0x551260, save 0x551900).

### 2.2 The only reader: galactic free-store unit hook 0x68fde0 (GalacticFreeStoreClass vf24) [H]
Getter 0x4f5920 has exactly one caller, 0x68fde0; the AIDataPack methods touch +0x39 only to save/load it. The hook is
the "auto-land" step already described in `AI_GALACTIC.md` §2.8 and `STRATEGIC_GAPS.md` (0x375220 paragraph). Full gate,
in order, for a free-store unit U:
1. the free store's player is not human (player byte +0x62 = 0);
2. `Is_Unit_Safe` 0x6902b0 is true and `Is_Unit_In_Transit` 0x68efb0 is false;
3. U has TRANSPORT (slot +0x337) and a parent F (obj +0x2b0) that is not a planet (F +0x335 = −1), i.e. U sits in a
   fleet;
4. **prefer-ground**: U's UNIT_AI +0x39, taken as **true when U has no UNIT_AI**;
5. F's parent P (the planet it orbits, F +0x2b0) exists, P's ground force count (0x3f9530 on P's planet behaviour) is
   below `Max_Ground_Forces_On_Planet` (0x140b168c8, 10), and P's surface is accessible (0x375220).
Then two FleetManagement events are built and executed for U's owner: op **2** (detach U from fleet F at P), then op **5**
(land U on P).
- So the flag only matters in **galactic** mode, only for **AI-owned transports in an orbiting fleet**: false keeps the
  unit in orbit (no auto-land); true (default) lets the free store land it. Tactical code never reads it. Human-owned
  units are unaffected either way.
- Port note: the port's galactic AI free-store hook should take this flag (default true, saved) into step 4.

---

## 3. t06: ability fields the port parses but does not read

### 3.1 LURE pulsing: `Is_Pulsing`, `Pulse_Frequency_Secs`, `Max_Number_Of_Pulses`, `Target_Types` (unit ability 10) [H]
`LureBehaviorClass` (behaviour 0x23, vtable 0x8af6b0). Interval 1 (vf5 0x18ceb0), attach vf9 0x628800 suspends it (starts
off), so the base attach makes no RNG draw. Data pack obj +0x1d8 (`LureDataPackClass`): +8 pulse count, +0xc next-pulse
frame, +0x18 the spawned effect object. Record fields read from the LURE `Unit_Ability` (via 0x397d90(obj, 10)): +0x20
`Effective_Radius`, +0x30 `Pulse_Frequency_Secs`, +0x34 `Max_Number_Of_Pulses`, +0x38 `Target_Types` (vector: data +0x40,
count +0x48, 16-byte entries, type pointer at +8), +0x60 the resolved `Particle_Effect` type.
- **Activate** (vf23 0x62b190): errors (log) without the LURE record; resumes the behaviour; count := 0, next := NOW (first
  pulse at the next service). If the owner is not in a team, or it is the **first member** of its parent's team, create
  the `Particle_Effect` object at the owner's position and facing, owned by the owner's player (0x62af80 → 0x29f810, a
  normal object creation, so it takes an object id [M]) and store it in +0x18; then the colour fade 0x62b6f0 (visual).
- **Service** (vf6 0x62b4c0, every frame): skip when suspended (counter +0x3c > 0) or without the record; visual 0x62b6f0;
  wait until frame ≥ next. Then:
  - if count < `Max_Number_Of_Pulses`: count += 1; next := frame + int(FPS × `Pulse_Frequency_Secs`) (float multiply,
    truncation); **pulse**; then the base service 0x4c2f70.
  - else: suspend the behaviour (vfunc +0xc0). The ability's own countdown is not touched here [M: the slot "active"
    byte stays until `Expiration_Seconds` runs out].
- **Pulse**: centre = owner position (obj +0x78/+0x7c/+0x80), radius r = `Effective_Radius`. For each `Target_Types` entry
  in XML order, take every object whose type pointer **equals** that type (GOM main list order, 0x2a87d0; no variant or
  base-type matching). Keep it when the **3D** squared distance ≤ r² (double compare). Replace it by its parent when the
  parent has TEAM. Keep it when it is an enemy of the owner's player (0x39a450 → 0x2824f0). Then lure it (0x62b270).
- **Lure one victim V** (0x62b270(behaviour, V, owner)): needs V not inside a TEAM parent, V not following a flagged path
  (0x39b000: any path record on V's locomotor path stack has byte +0x5c set, `A1_READINGS.md` "locomotor term"), and V's
  current attack target (0x397a40(V, 0)) ≠ the lure owner. Then `Order_Attack(V, owner, hp 0, ordered 1)` (0x38d730). If
  that returns 1 and the mode has a movement-coordinator system: V (and each TEAM member that the system accepts and that
  is not movement-locked) leaves its coordinator and joins one new coordinator, which gets a coordinator move of type 3
  towards the owner (or the owner's parent); an empty coordinator is destroyed and the call returns 0.
- **Deactivate** (vf24 0x62b100): kill the effect object, base deactivate.
- **`Is_Pulsing` (+0x2c) has no reader** [M: no reader among all functions that fetch a unit-ability record; the tag is
  even registered as type code 8 (float)]. LURE always pulses. With vanilla data (40 pulses × 0.5 s = 20 s against
  `Expiration_Seconds` 6 / 16 s) the expiry ends the lure before the pulse cap.
- `Target_Types` has no other reader found [M].
- Probe (optional, for the `Is_Pulsing` claim): a mod copy of a Skipray with `Is_Pulsing` No; enemies of the listed types
  inside the radius must still be pulled in repeatedly every 0.5 s.

### 3.2 `Max_Num_Spawned_Objects` (+0xa0): DEPLOY_TROOPERS (unit ability 38) only [H]
Readers: DEPLOY_TROOPERS behaviour activate vf23 0x58f6a0 and the cap test 0x58f840 (used by `Can_Activate` 0x39b480 case
0x26). Both compare it with the deploy pack (obj +0x1f0) **+0x58 = the count of the squads list at +0x48** (data +0x50).
- The list gets one entry per squad formed from released troopers (state 7 of the state machine 0x591a90 → 0x58fb80:
  troopers grouped by owner player, a team built for each group (0x2a5790), appended, then group-moved to a free spot).
- An entry is removed when that squad sends signal 0x28 (object removed / gone [M]) to the behaviour's handler 0x591900.
- `Can_Activate(38)` is false while count ≥ `Max_Num_Spawned_Objects`, or while the behaviour is running. Activation
  0x58f6a0 rechecks count < max (and: not on a flagged path, 38 not already active) before it stops and locks the unit
  and starts state 1.
- So the field caps **the number of live deployed trooper squads per unit**; when one dies, another deploy is allowed.
  Vanilla: 2 on every user (AT-AT / Veers variants). The spawn itself does not read it (`REMAINING_GAPS.md` §5.2 stands).
- An unset tag leaves the record default (the Unit_Ability ctor's value at +0xa0 was not read [L: probably 0, which would
  make `Can_Activate` always false; every vanilla DEPLOY_TROOPERS user sets it]).

### 3.3 `Enable_Bombing_Runs` / `Enable_Special_Weapons` / `Enable_Planetary_Bombardment` (Battlefield_Modifier) [H]
`BattlefieldModifierAbilityClass` vf30 0x6f10b0 = apply, vf31 0x6f17d0 = remove; with `Reverse_Application_Logic`
(+0x196) the two swap. Affected players (0x6f1130): the owner only, or with `Apply_To_All_Allies` (+0x195) every player
(index order) allied with the owner. Per player, its tactical data (mode vfunc +0x220(player)):
- apply 0x6f02c0 pushes, for each set flag, an entry {value **true**, expiry −1 (none), key 0x215a30(...), source = this
  ability on this object (0x48e1e0)} onto a bool list: `Base_Shield_Upgrade` (+0x190) → list +0x1b0, `Enable_Bombing_Runs`
  (+0x191) → **+0x1c8**, `Enable_Special_Weapons` (+0x192) → **+0x1e0**, `Enable_Planetary_Bombardment` (+0x193) →
  **+0x250**, `Disable_Reinforcement_Vulnerability` (+0x194) → +0x2c8;
- remove 0x6f13b0 deletes the entries with the same source from the same lists (0x3a5920).
Readers (each is the AND of the list's values, 0x2800d0 with `BitwiseAnd<bool>`):

| Gate | Function | Empty list | Non-empty list | Used by |
|---|---|---|---|---|
| bombing run allowed | 0x527550 (+0x1c8) | allowed except in a stand-alone skirmish (0x28afb0) | AND | bombing-run service 0x495dd0 / 0x495250 (`L1_S4_REMAINING.md` §3.7) |
| bombardment allowed | 0x5275a0 (+0x250) | allowed except in skirmish | AND | bombardment service 0x52a110 / 0x5294d0 (§3.8) |
| special weapons allowed | 0x527620 (+0x1e0) | (skirmish only) false; campaign true | AND, and tactical data +0x2f0 set | 8 callers (ion cannon / Hypervelocity etc., `SCRIPT_GAPS.md` §1.1) |

- So in skirmish these features are off until a player owns an object carrying such a modifier: vanilla uses them on the
  buyable "Use" upgrades (`RL_Bombing_Run_Use_Upgrade`, `RS_Ion_Cannon_Use_Upgrade`, Underworld `..._Planetary_Bombard_...`),
  whose `DUMMY_DESTROY_AFTER_BOMBING_RUN` / `_SPECIAL_WEAPON_FIRED` / `_PLANETARY_BOMBARD` behaviours delete the object after
  use, which removes the entry again [M: removal through the modifier's remove when its object goes]. In campaign an
  empty list allows.
- Lua `PlayerWrapper.Disable_Bombing_Run` pushes into the same +0x1c8 list (`L1_S4_REMAINING.md` §3.7), so a `false` entry
  there vetoes even a bought upgrade.
- Open [L]: the +0x2f0 byte for special weapons (not traced here; `SCRIPT_GAPS.md` §7 item 3).

### 3.4 Super_Laser `Face_Target` / `Turn_Rate` / `Firing_Cone` / `Fire_Continuation` (ability 55) [H unless tagged]
Ctor 0x714e30 defaults: `Face_Target` (+0x1c8) false, `Turn_Rate` (+0x1cc) **10.0**, `Firing_Cone` (+0x1d0) **45.0**,
`Fire_Delay` (+0x1d4) 0, `Fire_Continuation` (+0x1d8) 0. Vanilla: Death Star Face_Target true, Turn_Rate −1, Firing_Cone 10,
Fire_Continuation 0.01; Eclipse has no Face_Target.
- **Apply** (vf30 0x714fa0): SAP (obj +0x100) +0x300 "fired" := 0, +0x304 := NOW + int(FPS × Fire_Delay), +0x308 := −1.
  When the owner has TACTICAL_SUPER_WEAPON (slot +0x35e) **and** Face_Target: give the TSW pack (obj +0x148) the target
  id (+0x4c), the turn rate `Turn_Rate` (+0x50) and "facing on" (+0x54 := 1); SAP +0x301 "animation started" := 0.
  Otherwise play attack clip type 5 / `Attack_Anim_Sub_Index` at once and +0x301 := 1.
- **Turn** (TSW facing step 0x42b4e0, run from the TSW service vf6 0x42a7b0 in its states 3, 4, 5 and the default branch
  [M: per TSW service]): while +0x54 is set and the target id resolves, `Turn_Toward` (0x3abe40) the owner towards the
  target's position, **yaw only** (pitch untouched): yaw delta wrapped to (−180, 180], clamped to ±rate degrees per
  call. A rate ≤ 0 means the type's turn rate (0x372560: `Max_Rate_Of_Turn` × the mode speed multiplier;
  `S9_DEATH_CLONE_SHIELDS.md` §2.3), which is the Death Star case (−1).
- **Cone gate** (update vf4 0x715450, laser not fired, a target present, NOW ≥ SAP +0x304, Face_Target and +0x301 = 0):
  yaw to target = atan2(target.y − owner.y, target.x − owner.x) in degrees, mapped to [0, 360) (0 when the XY points
  coincide; 0x20acd0); compare with the owner's yaw (obj +0x8c, degrees) by 0x20af60: |difference wrapped to (−180, 180]|
  ≤ cone. **A cone of 90 or more is replaced by 0.01.** Outside → wait (next update). Inside → play the attack clip,
  +0x301 := 1 and TSW "facing on" := 0 (stop turning). Firing then waits for the clip frame ≥ `Fire_Frame` as described
  in `ABILITIES_HARDPOINTS.md` (Attack_Anim_Sub_Index paragraph).
- **Fire_Continuation** (vf13 0x7151c0, the per-target release hook [M: called when the ability's target link is dropped,
  e.g. the target dies, `ABILITY_VISUALS.md` §4.3]): if the SAP still lists that target under this ability's name
  (0x48df60), SAP +0x308 := NOW + int(FPS × `Fire_Continuation`), then the base End_Effect 0x48e920 on the target. While
  +0x308 ≠ −1 the update runs **with no target**: ability 55 no longer active, or the laser not yet fired → finish (vf31
  0x715350: beam cleanup, owner back to idle clip); fired and +0x308 ≤ NOW → finish and +0x308 := −1; else draw the beam
  to the remembered point (0x715a70). **No damage is dealt in this phase.** Vanilla 0.01 s → int(0.3) = 0 frames, so the
  laser finishes at the next update.
- Port needs: Face_Target turn (yaw-only clamp, rate ≤ 0 → type rate), the cone test with the ≥ 90 → 0.01 quirk, and the
  continuation as "finish at NOW + int(FPS × Fire_Continuation), beam only, no damage".

### 3.5 `Infection_CategoryMask` (Infection_Ability, unit ability 66) [H]
Who uses it: FoC `IG88_Infection` (`Infection_CategoryMask` Infantry, Distance_To_Infect 70, Seconds_To_Infect 2,
Duration 30, DPS 2).
- **First infection** (projectile hit 0x4cb280): projectile type has `Causes_Infection` (+0xa7), the shooter (projectile
  data +0x58 id) has ability 66 and its SAP holds the Infection_Ability by name. The **hit victim** gets INFECTION (0x54)
  attached, or resumed if it has it. **No category test here**: whatever the projectile hits is infected. The victim's
  infection pack (obj +0x218) gets: +8 frames left = int(FPS × `Duration_Of_Infection`), +0x10 the 64-bit category mask,
  +0x18 `Distance_To_Infect`, +0x1c `Damage_Per_Second`, +0x20 duration (s), +0x28 particle type, +0x30
  `Seconds_To_Infect`, +0x34 the attached particle's id.
- **INFECTION service** (vf6 0x636e80, interval 1): nothing while garrisoned (+0x3a7). On frames with frame % 60 = 0 it
  makes **one synced draw** `Get_Int(0, 4)` and plays the coughing SFX on 0 (draw only on those frames). Then frames left
  −= 1; at ≤ 0 the behaviour suspends itself (this is the cure: expiry) and the particle is removed. Otherwise
  `Take_Damage(kind 0x14, DPS / FPS)` and the **spread** step 0x636ae0.
- **Spread** 0x636ae0 (the mask's only reader): for every player in index order (all players, including the infector's own
  and allies), the GOM radius query 0x2a8fb0 for that player's objects within `Distance_To_Infect` (XY) of the infected
  object, query order. A candidate C counts only if C has a TD pack and TD +0x114 ≠ NOW, and passes 0x6cc790:
  C ≠ infected; obj +0x3a0 bits 0x40, 0x80 and 2 clear (not dying / removed); not a PROJECTILE; has a locomotor
  (slot +0x333); obj +0x2a0 non-null; not garrisoned; and **C's type `CategoryMask` (type +0x1648) & mask ≠ 0**
  (0x39f300 with no type lists).
  - C already has an active INFECTION → its counter is reset (TD +0x118 := 0), nothing else.
  - Else consecutive-frame counter on C's TD: +0x118 += 1 when TD +0x114 = NOW − 1, else 0; TD +0x114 := NOW. While
    (float)count < FPS × `Seconds_To_Infect`: wait. Then C is infected (attach or resume INFECTION) with a **copy of the
    infector's pack** (duration reset to full, same mask / distance / DPS / particle / seconds) and its counter := 0.
  - The counter lives on the candidate, not per infector pair: two infected units near C both bump it, but the
    "+0x114 ≠ NOW" test lets only the first one per frame count.
- So the mask is a **victim filter for spreading only** (Infantry in vanilla); vehicles, structures and anything without
  a locomotor never catch it from proximity, but can still be infected by a direct projectile hit.

### 3.6 Not settled
- `Max_Num_Spawned_Objects` record default when the tag is absent (Unit_Ability ctor not read) [L].
- The exact trigger of Super_Laser vf13 (which engine path drops the ability's target) [M].
- `Is_Pulsing` and `Target_Types`: "no other reader" is a search result over the record getters, not a proof that no code
  reads the record through the object slot directly [M].

---

## 4. Summary for the port

| Item | Status | Facts the port needs |
|---|---|---|
| t04a stickiness | answered [H] (gameplay effect [M], probe 1.5) | TD +0x158 float starts at **0.0**, saved (chunk 0x21); type value used only when the override is **negative**; written by Lua (unit + team members) and by every task-force release (type value) |
| t04b prefer ground | answered [H] | UNIT_AI pack +0x39, default **true**, saved (chunk 0xf); read only by the galactic AI free-store auto-land of transports in an orbiting fleet (false = stay in orbit); no tactical reader |
| LURE pulsing | answered [H] (`Is_Pulsing` unread [M]) | every int(FPS × freq) frames up to Max pulses, each pulse orders enemies of exact `Target_Types` within 3D `Effective_Radius` to attack the lure (+ coordinator move); then the behaviour suspends |
| Max_Num_Spawned_Objects | answered [H] | DEPLOY_TROOPERS only: cap on live deployed squads per unit (Can_Activate and activation); a squad's removal frees a slot |
| Target_Types | answered [H] | LURE only; exact type match, XML order, GOM list order |
| Enable_* lists | answered [H] | modifier pushes `true` entries per affected player (removed by source); gates are AND of the list; empty = allowed in campaign, disallowed in skirmish |
| Super_Laser | answered [H] (vf13 trigger [M]) | yaw-only turn at Turn_Rate (≤ 0 → type rate); cone |Δyaw| ≤ Firing_Cone with ≥ 90 → 0.01; continuation = beam only, finish at NOW + int(FPS × Fire_Continuation) |
| Infection mask | answered [H] | spread-only victim filter (type CategoryMask & mask); direct hit unfiltered; per-candidate consecutive-frame counter in TD +0x114/+0x118; one synced draw every 60 frames per infected unit |

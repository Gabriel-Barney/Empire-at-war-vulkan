# Hardpoint "disabled" flag (HardPointClass +0x6e)

Recovered 2026-10-28 from the FoC x64 binary (RVAs below). Answers the open question in
ABILITIES_HARDPOINTS.md §1.2 / TACTICAL_COMBAT.md §3.1, §5.1 / TACTICAL_GAPS.md §1.12: what sets the
hardpoint byte at +0x6e. Confidence per finding: [H] read directly, [M] inferred, [L] guess.

## 1. Summary

- **+0x6e is a "disabled" flag that is separate from "destroyed".** Destroyed = the type is
  `Is_Destroyable` (type +0x4d) and health (+0x28) ≤ 0. Destruction does **not** write +0x6e. [H]
- **In vanilla only one event sets it: the tactical space-station upgrade** (`Starbase_Upgrade_Ability`).
  When the station is replaced by its `Next_Level_Base`, each hardpoint of the new station that matches a
  destroyed, disabled or under-repair hardpoint of the old station starts **disabled**. [H]
- **The only thing that clears it is hardpoint repair reaching full health** (command 45). There is no timer.
  [H]
- **No combat effect sets it.** Ion cannon shots, ion stun, EMP and land stun, buzz droids, shield loss,
  damage thresholds, story events and Lua do not write +0x6e. They work through health, modifiers or the
  object-level weapons-disabled bit (obj +0x3a0 bit 0x10). [H]
- **It is not saved.** Load leaves it at the constructor value 0. [H for "not written", M for the
  consequence]
- The special-weapon can-fire test (0x386220) **does not read it**. A disabled special-weapon hardpoint is
  still blocked: `Update_Weapon` never fires it, the HUD shows its readiness as 0, and in skirmish its
  disabled state also turns off the owner side's `SpecialWeaponsAllowed` (§4). [H]

## 2. Writers (complete list) [H]

A scan of every byte, word and dword store that covers +0x6e in hardpoint code found exactly three writers:

| RVA | What | Value |
|---|---|---|
| 0x381a90 | `HardPointClass` constructor: dword +0x6c := 1, then word +0x6c := 1 | **0** (+0x6c = 1, +0x6d = 0, +0x6e = 0, +0x6f = 0) |
| 0x387170 | repair service (TACTICAL_GAPS §1.12 step 4): health reaches max → repairer list cleared, **word** +0x6e := 0 (clears +0x6e and the "being repaired" byte +0x6f), then `Set_System_Online(hp, online = 1, destroyed = 0)` (0x384d00) | **0** |
| 0x387d10 | `HP_Set_Disabled(hp, b)`: +0x6e := b, then `Set_System_Online(hp, online = !b, destroyed = 0)` | b |

`HP_Set_Disabled` (0x387d10) has **one caller**, `Starbase_Upgrade_Copy_Hardpoints` (0x70ec70), and it always
passes 1. Nothing found takes its address (no vtable or Lua binding). [H]

Not writers (checked):
- **Hardpoint destruction** (`HP_On_Health_Changed` 0x386660): at health ≤ 0 it calls
  `Set_System_Online(hp, 0, destroyed = 1)`, plays the death effects and raises the "last of its class"
  event. +0x6e is untouched. [H]
- **The other byte flags:** +0x6c is written only by the constructor (1) and by load chunk 9. +0x6d is the
  "fired / in burst" byte (Update_Weapon, 0x387b70, chunk 0xb). +0x6f = repairer list non-empty (0x387d40,
  0x387170). [H]
- **Ion cannon shot, ion stun** (0x62b920; modifier lists 5/6), **land stun** (0x4d01c0), **buzz droids**,
  **EMP / Lucky shot / energy drain**, **object weapons-disabled bit** obj +0x3a0 bit 0x10 (read by
  Update_Weapon as a separate gate), **shield behaviour**: none of them reaches 0x387d10 or stores to hp +0x6e.
  [H, by exhaustive writer scan]
- **Lua / story:** no Lua function or story reward touches hardpoint enable state (lua_api.json has no
  hardpoint setter). [H]

## 3. The station-upgrade copy (0x70f070 → 0x70ec70) [H unless tagged]

`Starbase_Upgrade_Ability` (XML `<Starbase_Upgrade_Ability>` under `RS_/ES_/US_Level_N_Starbase_Upgrade`
UpgradeObjects, `Activation_Style` = `Skirmish_Automatic`) apply 0x70ec20 → 0x70f070:
1. Resolve the station: the object itself, or its container (+0x2b0) when the object is not a station (+0x38e
   = −1).
2. Next type = station type +0x8c0 (the resolved `Next_Level_Base`, [M] for the offset/tag pairing). None → no
   upgrade.
3. Spawn the new station at the old one's position/facing for the same owner (0x29f810).
4. **Copy hardpoint state** (0x70ec70), by **index** i over the old station's `HardPoints` list (0x396cb0 on
   both; skipped when either side has no hardpoint i):
   - **old hardpoint has repairers** (+0x6f): new.`HP_Set_Disabled(1)`; new health := old health (0x387ca0,
     clamped to [0, max], destroyable types only); every repairer player id of the old hardpoint is re-added
     to the new one (0x387d40). So a hardpoint that was **being repaired, even if not destroyed or
     disabled, comes back disabled** and is re-enabled when that repair completes.
   - **else, old was destroyed** (destroyable and health ≤ 0) **or old was disabled** (+0x6e): new
     `HP_Set_Disabled(1)` and new health := **0.1** (0x3dcccccd). The 0.1 is what makes it repairable:
     repair drops a destroyed (health ≤ 0) destroyable hardpoint's list (TACTICAL_GAPS §1.12 step 2).
   - else: nothing copied. The new hardpoint keeps its fresh full health and is enabled.
   - If the new hardpoint type has `Allows_Special_Weapon_Use` (type +0x3d8), for every player allied to the
     station owner (0x2823e0) that has tactical player data: player-data +0x2f0 := (new +0x6e == 0) and the log
     line "SpecialWeaponRequiredHardpointEnabled flag set to …".
5. Copy other state (0x70eec0), move selection/AI/fleet bookkeeping, remove the old station (0x391310), play
   `SFXEvent_Starbase_Upgraded`.

Because `HP_Set_Disabled(1)` runs `Set_System_Online(…, 0, 0)`, the per-type offline effects of TACTICAL §5.1
apply at once to the new station (engine/shield/gravity-well/weapon/special-ability offline; "disabled" in
the log rather than "destroyed").

Vanilla data: every FoC station level has an `HP01` hardpoint with `Allows_Special_Weapon_Use True` (HARDPOINTS.XML
comment: "The Ion Cannon will be disabled for all Rebel players if this hardpoint is destroyed", and the
same for the Empire's Hyper Velocity Cannon).

## 4. Readers: what "disabled" does [H]

A disabled hardpoint keeps its health, stays targetable and damageable, still accepts repair, and can still be
destroyed. It is treated as **non-functional**:

| RVA | Reader | Effect when +0x6e = 1 |
|---|---|---|
| 0x387400 | `Update_Weapon` | returns before the cooldown step (weapon never fires; cooldown frozen) |
| 0x384850 | `Does_Weapon_State_Allow_Firing_At` | false (weapon types 5–10) |
| 0x394eb0 | `Collect_Weapon_Projectiles` (threat/AI and missile-shield checks) | hardpoint skipped |
| 0x397c10 | `Count_Functioning_Hardpoints(obj, type)` | not counted. This feeds `Set_System_Online` (engines/shields go offline only when the count of that type reaches 0), the Blast counter K (REMAINING_GAPS §5.5) and the "last weapon of its class" event on destruction |
| 0x4f4c60 | weapon-envelope recompute of the owner's behaviour 0x12 (run from `Set_System_Online` on weapon types) | hardpoint excluded [M for the behaviour's name] |
| 0x317d00 | HUD special-weapon button | readiness shown as 0 when the station's special-weapon hardpoint (0x394e60) is destroyed or disabled |
| 0x381a90 | constructor | an `ENABLE_SPECIAL_ABILITY` (11) hardpoint is put online only when +0x6e = 0, which is always true there |
| 0x70ec70 | upgrade copy | old disabled → new disabled (§3) |

Not readers: `special_weapon_can_fire` 0x386220, special-weapon readiness 0x385c00, target assignment
0x3822e0, `Fire_At` 0x3825b0, the closest-hardpoint pick 0x394a80, repair service 0x387170 (apart from the
clear). So the Lua/command special-weapon path (SCRIPT_GAPS §1.1 step 5) accepts and assigns a target to a
disabled station special weapon. It then never fires, because `Update_Weapon` gates it.

### Player special-weapon flag (player tactical data +0x2f0) [H]
`SpecialWeaponsAllowed` (0x527620) in skirmish requires this byte (SCRIPT_GAPS §1.1). It is initialised to 1
(0x526890). It is written by:
- `Set_System_Online` (0x384d00) tail, **every call**, for a hardpoint whose type has
  `Allows_Special_Weapon_Use`: for every player allied to the owner, +0x2f0 := online. So **destroying** that
  hardpoint (online 0) turns allied special weapons off, **disabling** it (upgrade copy) does the same, and
  **repairing it to full** (online 1) turns them back on.
- the upgrade copy (§3): := !disabled.

## 5. Interaction with destroyed state [H]

- They are independent. Every gate tests `(destroyable && health ≤ 0) || disabled`.
- Destruction never sets +0x6e. A disabled hardpoint can be shot down to 0 and becomes destroyed as well. From
  then on repair is refused (TACTICAL_GAPS §1.12 step 2), so it stays both destroyed and disabled for good.
- Repair of a damaged, non-disabled hardpoint to full also runs the clear and `Set_System_Online(1, 0)`. That
  is harmless when it was already online, but it does re-fire the special-ability enable for type 11 and
  re-assert player +0x2f0 = 1 for `Allows_Special_Weapon_Use` hardpoints.
- A non-destroyable hardpoint (+0x4d = 0) can still be disabled by the upgrade copy (health 0.1 is ignored by
  0x387ca0 for those), and is re-enabled only by repair to max.

## 6. Serialization [H]

The hardpoint save (0x386c50) writes chunks 0 owner, 1 health fraction, 2 target, 0x10 manual target, 0x12,
0x13, 3 target hardpoint id, 4 opportunity target, 5 cooldown, 6 pulses, 7 frame, 10, 0x11, **9 (+0x6c)**,
**0xb (+0x6d)**, 8 and 0xc (pointer remap). **+0x6e, +0x6f and the repairer list (+0x78) are not saved**, and
load (0x3863c0) has no chunk for them. After a load a disabled hardpoint comes back **enabled**, with its saved
health (e.g. 0.1), and repair is cancelled. Player +0x2f0 save status was not checked [L].

For the port, save +0x6e (and the repairer list) anyway, then emulate the vanilla behaviour on load only if
parity with vanilla saves is ever needed. Load-time behaviour is not observable in the CRC of a running match.

## 7. Implementation notes for `CombatSystem::set_hardpoint_disabled`

1. `set_hardpoint_disabled(hp, true)` = +0x6e := 1 then `set_system_online(hp, false, destroyed=false)`.
   Its only caller is the station-upgrade copy (§3), executed by index across old→new hardpoint lists, in list order,
   inside the upgrade ability apply (sim tick, no RNG).
2. Clear only in the repair service when health == max: clear disabled and being-repaired, empty the repairer list,
   `set_system_online(hp, true, false)`.
3. All weapon gates and the functioning-hardpoint count use `is_out(hp) = (destroyable && health <= 0) || disabled`.
4. `Allows_Special_Weapon_Use` → every `set_system_online` call writes `special_weapon_hp_enabled` on every
   allied player's tactical data. Skirmish `SpecialWeaponsAllowed` ANDs it.
5. No timers, no XML constant and no GameConstants entry is involved. The only data inputs are
   `Allows_Special_Weapon_Use` (HardPoint, +0x3d8), `Is_Destroyable`, `Repair_Amount_Per_Frame` /
   `Repair_Cost_Per_Frame`, `Next_Level_Base` and `Starbase_Upgrade_Ability`, plus the literal 0.1 health.

## 8. Named entry points

| RVA | Proposed name |
|---|---|
| 0x387d10 | `HP_Set_Disabled` |
| 0x384d00 | `HP_Set_System_Online(hp, online, destroyed)` |
| 0x386660 | `HP_On_Health_Changed` (destruction path) |
| 0x387ca0 | `HP_Set_Health` |
| 0x386120 | `HP_Is_Being_Repaired(hp, player = −1)` |
| 0x397c10 | `Count_Functioning_Hardpoints` |
| 0x70ec20 / 0x70f070 | `Starbase_Upgrade_Ability` apply / `Do_Starbase_Upgrade` |
| 0x70ec70 / 0x70eec0 | upgrade: copy hardpoint state / copy other state |
| 0x527620 / 0x526890 | `SpecialWeaponsAllowed` / player tactical data init |
| 0x386c50 / 0x3863c0 | hardpoint save / load |
| 0x393980 / 0x3924f0 | `Engines_Online(obj)` / `Engines_Offline(obj, destroyed, t)` |
| 0x5545a0 / 0x554640 / 0x557ba0 | `Locomotor_Set_Engines` / `Locomotor_Engines_On` / `Locomotor_Update_Engine_Timer` |
| 0x38ccf0 | `Engines_Permanently_Destroyed` |
| 0x549590 / 0x549550 | `Shield_Generator_Online` / `Shield_Generator_Offline` |
| 0x3a89c0 / 0x3a88a0 | `Gravity_Well_Online` / `Gravity_Well_Offline` |
| 0x384bb0 / 0x288890 | `HP_Set_Owner_Special_Ability` / player `Set_Special_Ability_Enabled` |
| 0x593350 / 0x594b30 | SpawnSquadron can-spawn (bay pruning) / bay list build |

## 9. `Set_System_Online` (0x384d00): the online branch, per hardpoint type

Added 2026-10-28. The switch is on hardpoint type +0x48: ENGINE 0, SHIELD_GENERATOR 1, GRAVITY_WELL 2,
FIGHTER_BAY 3, TRACTOR_BEAM 4, weapons 5–10, ENABLE_SPECIAL_ABILITY 11. The only caller with online = 1 is the
repair service at full health (0x387170, §2), so "online" always means "a hardpoint of this type was just
repaired to max". **The online branches have no live-count check**: any one hardpoint of the type reaching full
health runs them. The offline branches of ENGINE and SHIELD run only when the live count reaches 0. [H]

### 9.1 ENGINE (0) → 0x393980(obj) [H unless tagged]
Engine state lives in `LocomotorDataPackClass` (obj +0xa8, ctor 0x553f00). Its three engine fields are part of
the data pack's CRC (0x554930):

| Field | Init | Meaning |
|---|---|---|
| +0xd0 byte | 1 | engines online (`Are_Engines_Online`, SCRIPT_GAPS §2.2) |
| +0xd1 byte | 0 | engines **permanently** destroyed |
| +0xd4 int | −1 | timed disable: frame at which the engines come back; −1 = none |

Online (0x393980):
1. If a data pack exists and +0xd0 == 1 already, **return at once** (no signal, no audio).
2. Otherwise (0x554640) **word** +0xd0 := 1, so +0xd0 = 1 **and +0xd1 = 0** (the permanent flag is cleared),
   and +0xd4 := −1. Then, if data pack +0x1f0 is set, call 0x422040(+0x1f0): when that object's state +0x1d0 is
   neither 0 nor 3 and the top entry of its 0x68-byte list lacks flag 0x10, state := 0 and +0x21c := −1.0.
   [L meaning: it resets a pending movement/coast state. The offline path makes the same call.]
3. Send object signal **0x17** (engines online) via 0x220ed0 on obj +0x38. The offline path sends 0x16. The
   signal is also sent when there is no data pack.
4. Engine audio: start the **idle** loop with fade (0x3a7cf0(obj, 1), type +0x1400) unless it is already
   playing (obj +0x3b6). Offline also stopped the **moving** loop (type +0x1410, obj +0x3b5); it is not restarted
   here, and the locomotor restarts it on the next move [M].

Not restored: **TURBO (9) and SPOILER_LOCK (0x2b)**, which offline forced off. They stay off until re-activated.
**Speed** needs no write. The max-speed getter (0x3728f0) and the 0x376990 getter (acceleration/turn [M])
multiply by `Engines_Disabled_Speed_Modifier` (0x140b16d30, 0.4) **live** while +0xd0 == 0, so full speed
returns on the frame +0xd0 becomes 1.

**Destroyed vs disabled.** The offline path is 0x3924f0 → `Locomotor_Set_Engines` 0x5545a0(dp, destroyed, −1.0):
- It acts only when (+0xd0 == 1 or +0xd4 ≥ 0) **and** +0xd1 == 0. Otherwise nothing changes.
- destroyed = 1 (last engine hardpoint shot down): word +0xd0 := 0x100, which gives online 0, **permanent 1**,
  and +0xd4 := −1.
- destroyed = 0 (station-upgrade disable; t < 0): +0xd0 := 0 and +0xd4 := −1. Permanent stays 0.
- Timed disable, which no hardpoint triggers (listed for completeness): in Take_Damage (0x3a9e30), damage
  absorbed by behaviour 0xf (the energy pool [M]) from a projectile with its +0x48 flag set calls
  0x5545a0(dp, 0, projectile +0x4c8 seconds). That sets word +0xd0 := 0 and +0xd4 := max(+0xd4, now + int(30·t)).
  The locomotor update (0x3a6b80 → 0x557ba0) turns the engines back on when now ≥ +0xd4 and +0xd1 == 0, then
  sends signal 0x17. That path does not call the engine audio.

What **permanent** (+0xd1) does that plain "offline" does not:
- It blocks the timed re-enable (0x557ba0) and any further timed disable (the 0x5545a0 entry test).
- `Engines_Permanently_Destroyed` (0x38ccf0) is read by the space retreat coordinator (0x532c80). A space unit
  with permanently destroyed engines is **not put on the retreatable list** [H read, M meaning]. Separately, at
  the end of the retreat countdown (0x5337c0) a unit is dropped when +0xd0 == 0 (any cause), or when it has
  engine hardpoints and none is live [H].
- Repair cannot clear it in practice. Offline with destroyed = 1 runs only when the last live engine hardpoint
  is destroyed, and a destroyed hardpoint cannot be repaired. The online path *would* clear +0xd1 (word write)
  if a disabled but intact engine hardpoint were repaired afterwards. Vanilla only disables station hardpoints,
  so this cannot happen in vanilla [M].
- Edge case: if the engines are already off indefinitely (disabled: +0xd0 = 0, +0xd4 = −1) when the last engine
  hardpoint is destroyed, the 0x5545a0 entry test fails and **permanent is never set** [H].

### 9.2 SHIELD_GENERATOR (1) → 0x549590(shieldBeh, obj) [H unless tagged]
The shield behaviour is obj behaviour 0x10; the combat data is obj +0xf0.
- Online: if obj +0xf0 exists, **+0x131 := 0**, which clears the regen block. Nothing else happens: the pool is
  not refilled, there is no event and no SFX, and DEFEND is not re-activated.
- What offline left behind (0x549550 → 0x549810(beh, obj, 0)):
  - pool +0xfc = 0;
  - depletion frame +0x108 := now, unless the depleted lockout was already running;
  - DEFEND (1) forced off;
  - shield state +0x40 := 2 (down), with the shield-off SFX (type +0x1290; `SFXEvent_Shield_Off` [M]) and the shield mesh hidden (on the
    transition only);
  - then +0x131 := 1.
- So regen **resumes from the current pool, normally 0**, at the next regen tick (cadence 0x5494b0,
  TACTICAL_COMBAT §3.7):
  - new = clamp(pool + `Shield_Refresh_Rate` × SHIELD_REGEN × live/total generators, 0, max);
  - capped at `Depleted_Shield_Regen_Cap` (0.25 points) while (now − +0x108)/30 < `Depleted_Shield_Disable_Time`
    (5 s). Repair usually takes longer than 5 s, so the cap rarely applies [M];
  - units with an energy pool still pay per point.
- The first tick that raises the pool above 0 makes the 2 → 1 transition in 0x549810: +0x108 := −1,
  the shield-on SFX (type +0x12a0; `SFXEvent_Shield_On` [M]), shield mesh shown. That is the moment the shields are heard and seen to
  come back.
- DEFEND becomes activatable again, because `Can_Activate_Unit_Ability` reads +0x131 via 0x549460.
- The block covers regeneration only. Direct shield writes (owner-change rescale 0x38e5a0, shield set 0x3a91e0)
  can raise the pool while +0x131 = 1 [H read, M relevance].

### 9.3 GRAVITY_WELL (2) → 0x3a89c0(obj) [H]
- Online: obj **+0x3ca := 0**, and nothing else. INTERDICT (unit ability 3) becomes activatable again (0x39b480
  tests +0x3ca) but is **not** switched back on. The player or AI has to re-activate it.
- Offline (0x3a88a0) queued an INTERDICT-off unit-ability event for the object (0x4305d0, ability 3, owner), then
  set +0x3ca := 1, with the log line "Gravity Well is disabled/destroyed".
- **Neither branch counts hardpoints.** With two gravity-well hardpoints, losing one sets +0x3ca. Repairing the
  other one (damaged, not destroyed) to full then clears +0x3ca, although the first is still destroyed. Vanilla
  interdictors have one gravity-well hardpoint [M for the data claim].

### 9.4 FIGHTER_BAY (3) and TRACTOR_BEAM (4) [H]
These types have no branch. On/offline does nothing for them apart from the common tail (§9.7). Their gates read
the hardpoints live:
- **Hangar spawning** (SpawnSquadron, space, 0x593350): before each spawn, every **destroyed** bay (destroyable
  and health ≤ 0) is **erased** from the cached bay list (built by 0x594b30 from type-3 hardpoints). An empty
  list means no spawn. The check does **not** test +0x6e, so a **disabled** bay still spawns. Erased bays never
  come back.
- DEPLOY_SQUAD (75) and TRACTOR_BEAM (30) need at least 1 live bay or tractor hardpoint by
  `Count_Functioning_Hardpoints`, which excludes disabled ones. They become available again on the frame the
  repaired hardpoint is live.

### 9.5 Weapons (5–10) [H, M for the behaviour name]
- Online and offline run the same code: log "Weapon hardpoint is enabled/disabled", then, if the object has
  behaviour 0x12, recompute its weapon envelope (0x4f4c60), which now includes the hardpoint again.
- Firing resumes because the gates (§4) read health and +0x6e. The cooldown was frozen while the hardpoint was
  disabled (Update_Weapon returned before the cooldown step), so it continues from the frozen value. A destroyed
  weapon cannot come back.
- Online fires no event and no SFX. The "last weapon of its class" event exists only on destruction (0x386660).

### 9.6 ENABLE_SPECIAL_ABILITY (11) → 0x384bb0(hp, online) [H, M for 0x4c62d0]
- Logs "Owner's '%s' special ability is enabled". It is skipped when obj +0x3a0 bit 0x80 is set (the owner is a death clone; TACTICAL_COMBAT §5.4.3).
- Reads the hardpoint type's `Special_Ability_Name` (type +0x450). If it is empty, nothing happens.
- Player-level `Set_Special_Ability_Enabled(player, ownerType, name, disable = !online)` (0x288890):
  1. updates the player's per-type ability set (player +0x378, 0x4c62d0) [M: so that later spawns inherit it];
  2. for every object of that type owned by the player that has a special-ability list (+0x100), sets the
     ability's enabled state (0x37df00).
  It therefore toggles the ability on **all** the player's objects of that type, not just this one. On failure
  it logs "failed to enable special ability".

### 9.7 Common tail (every type, both directions) [H]
If the hardpoint type has `Allows_Special_Weapon_Use` (+0x3d8), every player allied with the owner gets
tactical data +0x2f0 := online (§4). For a station's special-weapon hardpoint this is the only online effect: it
has no type branch of its own.

### 9.8 Port checklist for `set_system_online(hp, true, …)`
| Type | Online effect | Comes back by itself | Does not come back |
|---|---|---|---|
| ENGINE | if the engines are off: online := 1, permanent := 0, timer := −1; signal 0x17; idle-loop SFX | speed and acceleration (the live 0.4 multiplier no longer applies) | TURBO, SPOILER_LOCK; moving-loop SFX (the locomotor restarts it) |
| SHIELD | regen block := 0 | regen from the current pool (normally 0); "shield on" SFX at the first rise | pool value, DEFEND |
| GRAVITY_WELL | +0x3ca := 0 | INTERDICT can be activated | INTERDICT itself |
| FIGHTER_BAY / TRACTOR | none | DEPLOY_SQUAD / TRACTOR_BEAM gates (live count) | nothing to restore: automatic spawning never stopped for a disabled bay |
| Weapons | envelope recompute | firing; the cooldown continues | nothing |
| SPECIAL_ABILITY | enable the ability on all the player's objects of the owner's type | | |
| any type with `Allows_Special_Weapon_Use` | allied players +0x2f0 := 1 | skirmish `SpecialWeaponsAllowed` | |

Determinism: the engine fields +0xd0/+0xd1/+0xd4 are in the locomotor CRC, and the shield pool and +0x131 are
sim state. The port must write them on the same tick, inside the repair service. None of the online branches
draws RNG.

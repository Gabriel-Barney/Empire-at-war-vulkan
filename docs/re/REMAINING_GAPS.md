# Remaining gaps: tech theft and unlocks, tech advance, map placement, movement, abilities, galactic (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe: the Ghidra 12.2 export in `re_reference/`, plus llvm-objdump on
`corruption\StarWarsG.exe` where the decompiler lost arguments. This file is prose and tables only.

Conventions:
- Addresses are RVAs (image base 0x140000000).
- Confidence: [H] read directly, [M] inferred from strong evidence, [L] guess. **probe** = needs an in-game run, which Gabriel launches; in-game probes are paused.
- "Synced draw" = the lockstep LCG 0x140a13e24, used through float(lo, hi) 0x532480 / 0x1ffbb0, its 0..1 variant 0x5324d0 and int(lo, hi) 0x1ffb40 (MAIN_LOOP §4.1).
- GC = GameConstants (base 0x140b15800). FPS = 30.

Data checked: vanilla FoC `Data\XML`, and **Thrawn's Revenge** (TR, workshop 1125571106: loose `Data\XML` over `Config.MEG`).

Questions come from the wave A/B "under-specified" lists. Read this file together with TACTICAL_GAPS, STRATEGIC_GAPS, P4_SIM_SYSTEMS and BATTLE_TRANSITIONS; it does not repeat them.

## 0. Status per item

| § | Item | Status | Unblocks |
|---|---|---|---|
| 1.1–1.4 | Rebel tech theft (`Slicer_Ability`, R2D2_Slicer, RebelAdvanceTechPlan) | [H]; dialog / command setter [M]. **Port ✅ 2026-10-28** (galactic: sweep, slicer apply / AI pick / execute, command kind 2; apps/game slice dialog) | engine/galactic (abilities, unlocks), engine/ai (A2 plan), engine/lua |
| 1.5–1.6 | Every unlock source for `Build_Initially_Locked`; FoC / TR lock lists | [H]. **Port ✅ 2026-10-28**: items 2–5 wired (corruption capture variant 0x3fb040 wired, tested 2026-10-28), locks on by default | engine/galactic: `set_initial_build_locks` can go on once 1.5 is wired |
| 2 | `Build_Advances_Tech_Level` sets **to**, not by | [H]. **Port ✅ 2026-10-28** | engine/galactic production |
| 3.1–3.2 | Map base layouts: base-level filter, markers, structure placement, starbase, units | [H]; a few call arguments [M]. **Port ✅ 2026-10-28** (base-level / owner / branch filters, spawn-table draws, marker start placement; not modelled: pass-1 markers, planet upgrades, escorts, DUMMY_STAR_BASE) | engine/apps battle host, engine/tactical |
| 3.3 | Map ownership and reinforcement / capture points at land start | [H]; slot ↔ player-id meaning **probe**. **Port ✅ 2026-10-28** (map owners kept, Is_Neutral gate, countdown from GameConstants) | battle host, tactical capture points |
| 3.4 | TEAM objects for `Create_Team` companies | [H]; land snap [M]. **Port ✅ 2026-10-28** (Create_Team_Type, Max_Squad_Size chunks; centroid not snapped) | battle host, tactical spawn |
| 4 | Corridor squeeze, space ops 25–32 | [H]; threat weight [M]. **Port ✅ 2026-10-28** (threat planners; corridor pass stand-alone, no formation path to wire; threat fn = tactical AI point danger, Lua Move_To T wired) | engine/tactical movement |
| 4 | Jetpack, flying, bike, land-bomber locomotors | [H]; timed follower [M]. **Port ✅ 2026-10-28** (battle host picks the locomotor, feeds clip lengths and combat targets; circling caller and flying attitude open) | engine/tactical movement |
| 4 | Fighter locomotor | [M] (attack run untraced). **Port ✅ 2026-10-28** partial: 0x1B / Idle; attack run, Dead_Stop, hyperspace states open | engine/tactical movement |
| 5 | EJECT_VEHICLE_THIEF, DEPLOY_TROOPERS, JET_PACK, stun release, Blast, Leech_Shields | [H]; small [M] notes. **Port ✅ 2026-10-28** (battle host wired: fire gate, blast projectile, occupants, bones, trooper spawn, capture eject) | engine/tactical abilities, combat |
| 6.1 | Launch reveal (`Galactic_Influence_Range`) | [H] mechanism; trigger and reader [M]. **Port ✅ 2026-10-28** (influence set filled on launch; nothing reads it yet) | engine/galactic visibility, engine/ai |
| 6.2–6.3 | `Is_Stealth_Company`, `Planet_*_Stealth` | [H] (AI-only / UI-only). **Port ✅ 2026-10-28** (AI delivery path class 5 [M → port]; `GalacticSim::can_see_starbase_info` / `can_see_groundbase_info`, galactic planet labels gated; step-3 planet-visibility hook defaults to visible) | engine/ai, UI |
| 6.4 | Story flag that blocks battles | [H]. **Port ✅ 2026-10-28** (set by LINK_TACTICAL / corruption missions, cleared at battle start; in `paused()` and the OI hash) | engine/galactic conflicts, engine/lua (LINK_TACTICAL) |
| 6.5 | Build pads: UC bone, persistence, kill "list", capture reset | [H]; persistence ints [M]. **Port ✅ 2026-10-28** (bind-pose bone, Previous_Upgrade_Level_Type kill list, per-planet records in CampaignBattles) | battle host, engine/tactical |

**Port-affecting findings:**

> **Status 2026-10-30:** Items 2-6 below are written as pending but are done in the port (section 0 rows marked Port 2026-10-28). Item 2: `Set_Tech_Level` with the unlock sweep (`engine/galactic/include/alamo/galactic/campaign.hpp:518`). Item 3: no unlocked-list bypass of the tech test (`engine/galactic/src/campaign.cpp:607`). Item 4: a `Build_Advances_Tech_Level` item sets the level to its value (`campaign.cpp:824-827`). Item 5: section 0 row 3.1 (base-level filter). Item 6: section 0 row 3.3; the capture countdown is read from GameConstants (`engine/apps/sim/src/battle_catalog.cpp:94-95`). Items 1 and 7 are unchanged findings.

1. **FoC does not lock `Rebel_Star_Base_3..5` or `Underworld_Star_Base_3..5`.** Their elements carry `Build_Initially_Locked` twice (Yes, then No), and the last scalar wins (OBJECT_TYPES rule 6). TR repeats tags the other way: about 70 fighter squadrons end on **Yes**.
2. Raising the tech level **automatically unlocks** every sliceable locked type below the new level (§1.4). This is the main Rebel unlock path, and the port has no such sweep.
3. Buildability has **no unlocked-list bypass of the tech test** (STRATEGIC_GAPS §1.3 step 16). The port's "[M] unlocked types skip the tech-level test" is wrong.
4. `Build_Advances_Tech_Level` sets the level **to** the value (§2). The port's "raise by" only matches FoC because the data happens to agree.
5. Map objects are dropped when their **base level** (map mini 0x0B) exceeds the battle's base level (§3.1).
6. Capture points keep their **map owner** and are frozen for `*_Capture_Allowed_Countdown_Seconds` (FoC 30 s, TR 10 s) (§3.3). The port's anchor-distance rule should go.
7. New synced draws: the slicer (§1.3), `Map_Load_Spawn_Table` (§3.1), the space-station marker pick (§3.2) and the flying-locomotor loiter (§4.4).

---

## 1. Rebel tech theft and unlock sources

### 1.1 Who and what [H]
- **Ability.** `Slicer_Ability` (SlicerAbilityClass, vtable 0x8acbe8). It sits on the **R2D2** hero inside `Droids_Team` in both EaW and FoC, as `R2D2_Slicer` in NAMEDHEROUNITS.XML. **TR ships no `Slicer_Ability`**, so in TR the whole path is dead and only §1.5 applies.
- **Loader post-check** (0x619980):
  - `Activation_Style` is forced to **Ground_Activated** (6).
  - `Causes_Despawn` is forced to **true**.
  - The Min/Max respawn lists must be equal-length and Min ≤ Max. If not, both lists are cleared.
  - Each of the three curves needs ≥ 2 points. Otherwise it becomes (0, 0)–(5, 0); the cost curve becomes (0, 1)–(5, 1).
  - The curves are the same piecewise curve class as `Political_Income_Curve` (GALACTIC_ECONOMY; linear [M]).
- **Fields** (xml_field_tables "Slicer_Ability"):

  | Field | Offset | FoC R2D2 value |
  |---|---|---|
  | `Min_` / `Max_Respawn_Time_Per_Tech_Level` | +0x178 / +0x190, 5 floats | 60, 60, 60, 70, 70 |
  | `Chance_Modifier_By_Base_Level` (curve) | +0x1a8 | all 0 |
  | `Chance_Modifier_By_Planet_Difficulty` (curve) | +0x1d0 | all 0 |
  | `Cost_Multiplier_By_Planet_Tech_Availability` (curve) | +0x1f8 | 0→1.75, 1→1.5, 2→1.25, 3→1.0, 4→0.75, 5→0.5 |
  | `Chance_Affinity_Modifier` | +0x220 | +0.25 |
  | `Cost_Affinity_Modifier` | +0x224 | −0.25 |

- **Planet inputs** (planet type):
  - `Planet_Slice_Difficulty_Rating` +0x7d4 (read as float);
  - `Planet_Tech_Availability_Rating` +0x7d8 (float);
  - `Planet_Slice_Affinity_Types` +0x7e0 (resolved pairs at +0x7e8, count +0x7f0).
- **Type inputs:**
  - `Tech_Level` +0x89c;
  - `Build_Initially_Locked` +0xc1;
  - `Build_Can_Be_Unlocked_By_Slicer` +0xc2;
  - `Slice_Cost_Credits` +0x868 (float);
  - `Build_Cost_Credits` +0x86c;
  - `Required_Star_Base_Level` / `Required_Ground_Base_Level` +0xf10 / +0xf0c;
  - `Base_Level` +0x8a0.

### 1.2 Gates [H]
- **Accept (vf5 0x619040).** All of these must hold:
  - the target is a planet (+0x335 ≠ −1) and not held by the slicer's player or an ally (0x48e070 → 0x39a230);
  - the planet owner's faction is `Is_Playable` (+0x20a8);
  - the candidate list of §1.3 is non-empty.
- **Apply (vf30 0x6187a0)** re-checks accept through vfunc +0x28, then checks a **hard-coded faction pairing** (0x48d900). Slicer owner → planet owner (planet data +0x68 block, +0xC) must be:
  - Rebel → Empire or Underworld;
  - Empire → Rebel or Underworld;
  - Underworld → Rebel or Empire;
  - the owner must never be the slicer's own player.

  On failure the local player gets the message at 0x140b301c8 and apply returns false.
- **How it fires.** Through the Ground_Activated trigger (P4_SIM_SYSTEMS §6.1), from command kind 1, Lua `Activate_Ability` or `Land_Units`.

### 1.3 Apply, candidates, price, chance, RNG [H unless tagged]
1. **Candidates** (0x618cb0, type-manager order). A type qualifies when:
   - it is affiliated with the slicer player's faction (0x3751a0);
   - `Tech_Level` ≤ the player's tech level (+0x84);
   - `Build_Initially_Locked` is set;
   - it is not in the player's unlocked list (+0x1b0) or locked list (+0x1c8);
   - `Build_Can_Be_Unlocked_By_Slicer` is set.
2. **No candidates:**
   - if tech = max tech (+0x88): message, return false;
   - otherwise raise the tech by 1 (0x282190, with the §1.4 sweep) and **call apply again** (vf30 recursion). The recursion repeats until there are candidates or the tech is at max.
3. **Per candidate T** (0x619680), with P = player, X = planet type, D = planet data:
   - **affinity** = T is in X's `Planet_Slice_Affinity_Types` (pointer compare) → (ca, ka) = (`Chance_Affinity_Modifier`, `Cost_Affinity_Modifier`), else (0, 0).
   - **price** = max(0, (CostCurve(`Planet_Tech_Availability_Rating`) + ka) × `Slice_Cost_Credits` × m(P)).
     - m(P) is the timed tech-cost modifier 0x281960 (1.0 unless P +0x270 has entries; P4_SIM_SYSTEMS §1.3).
     - The affinity term is **added to the curve value**, not multiplied.
   - **base term** b = 0, unless `Build_Cost_Credits` > 0 and either:
     - (`Required_Star_Base_Level` > 0, or T is DUMMY_STAR_BASE 0x5c with `Base_Level` = 1): b = BaseCurve(starbase level D +0x148);
     - (`Required_Ground_Base_Level` > 0, or T is DUMMY_GROUND_STRUCTURE 0x5f): b = BaseCurve(ground-base level D +0x130).
   - **chance** = clamp(1 + b + DifficultyCurve(`Planet_Slice_Difficulty_Rating`) + ca, 0, 1).
   - Entry = {T, price, chance}, 16 bytes, in candidate order. With the FoC R2D2 data the chance is always 1.0.
4. **Respawn time R** = synced float(Min[t], Max[t]) with t = P's tech level, **drawn now** when the Min list is non-empty and t ≤ 4. Otherwise R = 0, and §6.2 of P4_SIM_SYSTEMS then uses GC `Default_Hero_Respawn_Time`.
5. **Human local player with P +0x62 set:**
   - plays `slice_dialog`;
   - opens the slice dialog (0x3c95d0, TEXT_SLICE_DIALOG_TITLE / _DESCRIPTION, one row per entry, Cancel);
   - returns **false**, so the trigger does not despawn the unit.
   - Picking a row sends **SpecialAbility event 13, kind 2** (§1.3.1). Cancel sends nothing [M].
6. **AI player (P +0x360 set):**
   - keep the entries with price ≤ credits (P +0x70), with weights 1 each, cumulative, in entry order;
   - none left → false;
   - otherwise r = synced float(0, count) and pick the first cumulative ≥ r (binary search);
   - then execute (step 7) with "only one" = (total entry count == 1).
   - Any other player kind → false.
7. **Execute** (0x619160; entry, onlyOne, slicer id, R, planet):
   1. The slicer must still exist, else log "Error - Cannot complete slice action…" and return false.
   2. price > P credits → **return false**: no draw, no despawn.
   3. **Vigilance.** If X `Planet_Vigilance` (+0xc0) is set and the slicer type is `Is_Generic_Hero` (+0x14):
      - TEXT_SLICE_FAILURE_VIGILANCE goes to the slicer owner and TEXT_SLICE_PREVENTED_VIGILANCE to the planet owner (local only);
      - the respawn is scheduled with time 0 (0x48eb10, so the default time) and the team despawns (0x48cf20);
      - return false; no draws, no charge.
      - R2D2 is a named hero, so this never triggers in FoC.
   4. s = synced float(0, 0.999) (0x1ffbb0). **Success when s < chance.**
   5. **Success:**
      - Add_Credits(P, −price);
      - append T to the unlocked list (0x27f860);
      - local player: faction `SFXEvent_New_Construction_Options_Available` (+0x670) and HUD text TEXT_SLICE_SUCCESS + T's display name;
      - if onlyOne, **also** raise the tech level (0x282190, with the sweep);
      - local player: `SFXEvent_Slice_Success` (+0x610); local "forced steal slot" cleared (0x140b29088 := 0);
      - log "Slice: Object Type %s has been unlocked."
   6. **Failure:** the failure-kind draw (0x618e30) k = synced int(1, 3):
      - 1 = captured: R × 2, TEXT_SLICE_FAILURE_CAPTURED;
      - 2 = lost effort: no extra effect;
      - 3 = cheated: the price **is charged**, TEXT_SLICE_FAILURE_CHEATED.

      Then faction +0x620 speech for the local player.
   7. Both outcomes: schedule the respawn at X with R (0x48eb10, P4_SIM_SYSTEMS §6.2), despawn the team (0x48cf20), and return the success flag.
8. **Draw order per apply:**
   - R (step 4, when the lists exist);
   - the AI pick (when there is ≥ 1 affordable entry);
   - s;
   - k on failure.

   On an AI success the trigger's own Causes_Despawn branch then draws vf12 once more and schedules again. This is the same **double departure** as the black market (P4_SIM_SYSTEMS §6.1 note).

#### 1.3.1 Command encoding: SpecialAbility event 13, kind 2 [H layout, M setter]
- **Pack** 0x4b4990, **unpack** 0x4b4470, **execute** 0x4b4d70 case 2.
- **Payload after the 3-bit kind:**
  - id32 planet (+0x13c);
  - id32 slicer (+0x140);
  - 16-bit type index (type +0xf0) → +0x48;
  - price on quantizer 8 (+0x50);
  - chance × 1000 on quantizer 5 (+0x54; unpack divides by 1000);
  - R on quantizer 5 (+0x144);
  - 1 bit onlyOne (+0x150).
- **Execute:**
  - re-quantizes price (q8), chance (q5, **without** the ×1000) and R (q5) locally (COMMANDS §3.2);
  - requires the planet to exist and be a planet;
  - then runs §1.3 step 7.
- The setter used by the dialog is not identified. 0x4b53b0 is kind 5 (sabotage) [M].
- The general execute guards of event 13 apply (pending battle, linked battle §6.4).

### 1.4 Tech level setter and the unlock sweep (0x288980) [H]
`Set_Tech_Level(P, L, sweep, speech)`:
- L < 1 → 0; tech := min(L, max tech +0x88).
- Notifies the GUI (0x32e8c0).
- **When `sweep` is set:** every type, in type-manager order, that meets all of these is appended to P's unlocked list:
  - affiliated with P's faction;
  - `Tech_Level` **<** the new tech (strict);
  - `Build_Initially_Locked`;
  - `Build_Can_Be_Unlocked_By_Slicer`;
  - not already unlocked.
- With `speech` and ≥ 1 unlock, the local player hears faction +0x670.

Callers:

| Caller | Level | Sweep |
|---|---|---|
| 0x282190 (+1 when below max): slice, Give_Random_Sliceable_Tech, BUILDABLE_UNIT | tech + 1 | yes |
| production of a `Build_Advances_Tech_Level` item (0x402840) | **the item's value** (§2) | yes |
| campaign start (0x292590): campaign `Starting_Tech_Level` / `Max_Tech_Level` per faction (0x4af410 / 0x4af2d0); game-options override when record flag 0x20 is set: option −1 if the faction has `Displayed_Tech_Level_Adjustment` | start | yes |
| story SET_TECH_LEVEL (reward 0x41, 0x461fc0): P1 faction, P2 level; raises max first if needed | P2 | yes |
| Lua `PlayerWrapper.Set_Tech_Level` (0x604480) | arg | yes |
| mode start with a pending tech option < 6 (0x44a3f0, all players) [M: what sets it] | option | yes |
| skirmish init (0x294fa0, 0x295cf0, 0x35cb10) | 1 | no |
| skirmish: object with `Tactical_Destruction_Decrements_Tech_Level` killed (0x391310) | owner tech − 1 | no |
| tactical build of `Tactical_Build_Increments_Tech_Level` (0x498170): every allied player of the same faction | builder's tech + 1 | no |

So FoC Rebels at tech L can steal tech-L units one at a time. The last theft raises them to L+1, and the sweep then unlocks anything left below L+1. The campaign start sweep unlocks the sliceable tech-0 types for a Rebel starting at tech 1.

### 1.5 Every unlock source for `Build_Initially_Locked` types [H]
Player lists:
- **unlocked** +0x1b0 / count +0x1b8: add 0x27f860, remove 0x286150, query 0x282580;
- **locked** ("obsolete") +0x1c8 / +0x1d0: add 0x27f7c0, remove 0x286100, query 0x282550;
- **production-disabled** +0x1e0 / +0x1e8: add 0x27f710, remove 0x2860b0, query 0x282510.

Writers of the unlocked list:
1. **Slice** (§1.3) and the **tech sweep** (§1.4), which covers every tech raise including production research, story, Lua and campaign start.
2. **Story BUILDABLE_UNIT** (reward 1, 0x45e8b0):
   - P1 type: remove from locked, add to unlocked;
   - P2 type: add to locked;
   - then, **only if the plot player's faction is GC `Good_Side_Name`** (+0x12e0, "REBEL" in FoC and TR): if no sliceable candidate remains at the current tech (the §1.3 step 1 test), raise the tech (+1, with the sweep).
3. **Lua `Unlock_Tech`** (0x604540): warning if the type is already unlocked and not locked. Otherwise remove it from locked and add it to unlocked, with **no** tech check.
4. **Lua `Give_Random_Sliceable_Tech`** (L1_S4_REMAINING §2.9; FoC TechPowerUp script).
5. **Corruption success** (0x3f9690; at campaign start 0x28dbe0 and on corruption change 0x3fc5b0):
   - the new corruption type's `Corruption_N_Success_Unlock_Unit_Type` is unlocked for the corrupting player when its planet cost (0x3711c0) > 0;
   - the replaced corruption type's unlock is **removed** (0x286150), even if another planet still grants it [H calls, M consequence];
   - the planet-capture variant 0x3fb040 does the same for corruption type 3 when a `Benefits_From_Corruption` faction takes the planet.
   - FoC uses this for the Underworld units: Broadside / Tartan / Marauder / Nebulon_B / Acclamator / Corellian_Corvette_Underworld, Underworld_TIE_Defender / A_Wing squadrons, the eight *_Slave_Company and Underworld_Ewok_Handler_Company.
   - TR has none.

Not unlock sources:
- ENABLE_BUILDABLE / DISABLE_BUILDABLE (rewards 0x70 / 0x6f) only edit the **production-disabled** list. That list is read by the GUI build bar (0x3145c0) and AI production (0x726010), **not** by buildability 0x2804d0.
- LOCK_UNIT (0x4d) and Lua `Lock_Tech` edit the locked list.
- UNIQUE_UNIT (2) spawns; it does not unlock.
- SET_MAX_TECH_LEVEL (0x5f) writes +0x88 only, with no sweep.
- SHOW_STEAL_SLOT / HIDE_STEAL_SLOT (0x77 / 0x78) set or clear a GUI "forced steal slot" object (0x140b30588; P1 = a unit type, first live instance). They are UI only.

Port correction: the PHASE4 L1 note maps ENABLE_BUILDABLE → `lock_type(false)` and the unlocked list. In the original, it only clears the production-disabled flag.

### 1.6 Locked types in the data [H]
- **FoC** (final value after repeated tags): 153 types.
  - **Locked and sliceable (22, all Rebel):** A_Wing_Squadron, Alliance_Assault_Frigate, B-Wing_Squadron, BWing_Squadron_Tech_Tree, Calamari_Cruiser, Corellian_Gunboat, Gallofree_HTT_Company, Ground_Infiltrator_Facility, Ground_Ion_Cannon, MC30_Frigate, Marauder_Missile_Cruiser, Nebulon_B_Frigate, R_Galactic_Turbolaser_Tower_Defenses, R_Ground_Base_Shield, R_Ground_Heavy_Vehicle_Factory, R_Ground_Light_Vehicle_Factory, R_Orbital_Jamming_Station, Rebel_Ground_Shutter_Shield, Rebel_Heavy_Tank_Brigade, Rebel_Orbital_Long_Range_Scanner, Rebel_Speeder_Wing, Y-Wing_Squadron. Their unlock paths are §1.3 and §1.4. The other `Build_Can_Be_Unlocked_By_Slicer` = Yes types (e.g. Rebel_Star_Base_3..5) are not locked, so the flag has no effect on them.
  - **Underworld units:** corruption unlocks (§1.5 item 5).
  - **Story-only:** heroes' cinematic clones, `*_Survivors_*`, `Skirmish_*_Star_Base_*`, build pads, CIN_ / Fin_ props, Jedi_Cruiser_*, Sundered_Heart and the like. They are unlocked by BUILDABLE_UNIT / `Unlock_Tech` or never.
  - `Rebel_Star_Base_3..5` and `Underworld_Star_Base_3..5` are **not locked** (finding 1).
  - FoC story use: 57 BUILDABLE_UNIT, 27 SET_TECH_LEVEL, 4 SET_MAX_TECH_LEVEL, 32 LOCK_UNIT, 142 DISABLE_BUILDABLE, 28 ENABLE_BUILDABLE.
- **TR:** 478 locked types (loose XML), 7 sliceable, no slicer. TR unlocks through Lua and story:
  - Lua: 103 `Unlock_Tech`, 204 `Lock_Tech`, 34 `Set_Tech_Level` references in the scripts;
  - story: 18 BUILDABLE_UNIT, 13 ENABLE / 13 DISABLE_BUILDABLE, 21 LOCK_UNIT, 3 SET_TECH_LEVEL.
- **Port:** wire §1.4 (the sweep, set-to semantics) and §1.5 items 2–5, then switch `set_initial_build_locks` on.

---

## 2. `Build_Advances_Tech_Level`: set **to**, not raise by [H]
- **Production** (0x402840). When the head item's type has `Build_Advances_Tech_Level` ≠ 0, the item calls `Set_Tech_Level(P, value, sweep = 1)` (0x288980, §1.4) instead of creating an object.
  - The level becomes min(value, max tech). It does not become tech + value.
- **Buildability step 7** (STRATEGIC_GAPS §1.3) only offers the item when P's tech = the item's `Tech_Level` and P's max tech ≥ value. In shipped data the two readings coincide: FoC DS_Primary_Hyperdrive TL1 → 2, DS_Shield_Gen 2 → 3, DS_Superlaser_Core 3 → 4, DS_Durasteel 4 → 5 (Empire). TR has no such items.
- **Port:**
  - set the level to the value, clamped to [0, max];
  - run the sweep;
  - keep the m_techupg cost factor of GALACTIC_ECONOMY §2.4.

---

## 3. Map base layouts, map-placed objects and start placement

### 3.1 Loading map objects: base-level filter, discard rule, spawn tables [H unless tagged]
1. **Base level** (BATTLE_TRANSITIONS §2.1 step 2) is the ground-base level D +0x130 (land) or the starbase level D +0x148 (space). It is passed as:
   - battle-load callback;
   - Load_Map 0x56480, which defaults to 5 and clamps to a per-map value initialised to 1 [M];
   - mode load vf15 (0x3b8cd0 land, 0x4d80b0 space);
   - object chunk 0x102 (vf13 0x3b8950 / 0x4d7da0 → 0x35f970);
   - GOM object loader 0x2b0a20.
2. Each map object (chunk 0x459, parsed by 0x39c3a0 / 0x39ce40) keeps its minis:
   - 01 type;
   - **02 owner = player id**. An unknown id destroys the object with "An object was saved with a Player ID that no longer exists!";
   - 04 position, 05 facing;
   - **0B base level byte → obj +0x39c**;
   - 14 slot index +0x3ac;
   - 33 remembered marker id +0x3b0.
3. **Base-level filter:** in map-placement mode, an object with **+0x39c > the battle base level is destroyed** before it enters the world. The port's .ted reader keeps mini 0x0B in `placement_raw`, and the battle host must apply this filter.
4. **Branched-map discard** (single player; defender id given; type `Is_Branched_Map_Discardable` +0x9f). The object is kept only if one of these holds:
   - owner = defender;
   - `Loads_When_Faction_Present` (+0x75) is set and owner = attacker;
   - the owner's faction does not have `Can_Control_Planets` (+0x109);
   - the type `Is_Marker` (+0x11);
   - a behaviour test (0x370320, arguments lost) passes.

   Otherwise it is destroyed. A global at 0x140b155d8 ≠ −1 additionally lets attacker-owned objects stay [M]. MP compares factions instead of ids [M].
5. **`Map_Load_Spawn_Table`** (type +0x20b0, 24-byte entries {type, cumulative weight}), skipped when 0x140b15490 is set:
   - one synced float(0, 1) r1;
   - a spawn happens only if r1 ≥ 1 − W, where W = the last cumulative weight;
   - then r2 = synced float(0, W) [M upper bound], first cumulative ≥ r2 (0x378d40 / 0x378b70).
   - The chosen type is queued with the marker's position and facing. Its owner is the first player whose faction is in the type's `Affiliation`, else the marker's owner. The placeholder is removed.
   - Data: FoC MARKERS.XML Rancor / Credit_Power_Up (0.25 each), Random_Secondary_Structure_R / E / P (7 × 0.14). These draws happen in map object order during the load (TACTICAL_GAPS §4.3).

### 3.2 Post-load placement of galactic forces (0x2a1eb0, first post-load step of BATTLE_TRANSITIONS §2.1 step 4) [H unless tagged]
Inputs:
- the start lists of GOM +0x498 (BATTLE_TRANSITIONS §2.3);
- the conflict record (space list GOM +0x470, land +0x488: {…, attacker id +4, defender id +8});
- the battle planet;
- every map object with the MARKER behaviour (+0x343 ≠ −1) and an owner, in GOM list order.

Markers are recognised by **type name** (case-insensitive; FoC MARKERS.XML):

| Marker type | Used for |
|---|---|
| `Space Station Position` | the starbase (space) |
| `Orbital Special Structure Position` | `Base_Position` = Orbital structures (space) |
| `In Base Special Structure Position` / `Out Base Special Structure Position` | `In_Base` / `Out_Base` structures (land) |
| any marker with `Marker_For_Specific_Object_Type` (+0x20f8, count +0x2108) | the listed types (shield, power generator, turbolaser, pirate command centre; MP build pads) |
| `Defending Forces Position` | defender start units, and the defender facing |
| `Attacker Entry Position` | attacker start units |

Steps:
1. **Starbase (space):**
   - collect the Space Station markers;
   - pick one with synced int(0, n − 1) when n ≥ 2 (no draw for n = 1);
   - spawn the defender faction's DUMMY_STAR_BASE type with `Base_Level` = D +0x148 (0x407b60) at the marker's position and facing, for the record's defender;
   - register it as the space-station super weapon (0x35c520) when the type `Is_Special_Weapon_In_Space` (+0x1e);
   - the local camera focus (+0xc0) is that marker when the defender is local.
2. **Planet structures** (0x4b81e0 list of D's structures). For each structure whose type has `Tactical_Additional_Structure_Type` (+0x8d0) and no structure of that type yet:
   - create a **temporary** galactic object of that type (for example Power_Generator_E / R);
   - append it, with tactical id override 0x3fffff;
   - kill it silently (0x391310) after the placement.
3. **"Every specific marker"** structures. A type with `Place_Other_Type_At_Every_Specific_Marker_Position` (+0x22f0; FoC `R/E_Galactic_Turbolaser_Tower_Defenses` → `R/E_Ground_Turbolaser_Tower`; TR power generator / turbolaser) spawns the other type at **every unused** specific marker that lists it. Owner = the structure's owner; spawned +0x54 := the structure's galactic id; the marker is marked used.
   - Land uses structures with +0x391 ≠ −1, space those with +0x392 ≠ −1 [M: the land / space structure behaviour slots].
4. **Other structures,** two passes: pass 1 for structures with a remembered marker (+0x3b0 ≠ −1), then pass 0 for the rest.
   - **Group** = a specific marker that lists the type; else by `Base_Position` (+0x1d78): `Orbital` → orbital markers, `In_Base` → in-base markers, `Out_Base` → out-base markers.
   - When in-base markers run out, In_Base falls over to out-base, and vice versa.
   - **Pass 1** reuses the marker whose object id equals +0x3b0. **Pass 0** takes the next unused marker of the group in list order.
   - On placement: +0x3ac := slot index; +0x3b0 := marker id (the battle-to-battle persistence of pad and structure positions); spawn the structure's own type at the marker for the **record defender**; +0x54 := its galactic id (0x3fffff for the temporary additional structures).
   - **Space, pass 0, no slot, type `Is_Special_Weapon_In_Space`:** spawn for the planet owner at the first behaviour-99 object [M: DUMMY_TACTICAL_SUPER_WEAPON_TARGET / `Death_Star_Position`].
   - **Unplaced structures** take any remaining free in-base, then out-base markers. With none left they are **not spawned**.
5. **Planet-specific upgrades:** for every player, each type in that player's per-planet list (0x281620 / 0x281760; the corruption `*_Planet_Specific_Tactical_Upgrade` [M]) that is affiliated and has behaviour 0x65 in the mode is spawned at the origin for that player.
6. **Defender units:**
   - the defender's facing slot (mode +0x288, 12 bytes per player index) := the first Defending Forces marker's facing;
   - in space, every start unit's `Space_Escort_Unit_Types` (+0x2028 / +0x2030) is spawned and appended (flag +0x3a6 = 1);
   - then formation placement 0x2a00b0 places the start list on the Defending Forces markers (§3.4 for teams).
7. **Attacker units.** Collect the Attacker Entry markers and choose one:
   - **If** some attacker-filtered DUMMY_GROUND_STRUCTURE (0x2a9ff0 with 0x5f) has a non-zero `Reinforcement_Prevention_Radius` (+0x1df8): take the entry marker **farthest** from the first such object;
   - **otherwise** the entry marker **nearest** the first Defending Forces marker [M: argument order of the filter].

   Then set the facing slot, spawn the escorts (space) and place by formation, with the last Defending Forces marker as the look-at point (space).
8. No other synced draws here beyond the station pick. Object construction draws as in TACTICAL_GAPS §4.3.

### 3.3 Map ownership and reinforcement points at land start [H; probe for the slot meaning]
- Reinforcement and capture points are map objects. They **keep their map owner** (mini 02), subject to §3.1 items 3–4; nothing reassigns them at battle start.
  - FoC `Reinforcement_Point` (CAPTURE_POINT, REINFORCEMENT_POINT) is a plain map object.
  - `Reinforcement_Point_Rebel/Empire/Underworld` are MP / skirmish specific markers.
- **Capture start delay** (CapturePointBehaviorClass service vf6 0x501980). In campaign (not skirmish), capture logic is skipped until mode frame ≥ int(30 × countdown + 0.5), where the countdown is GC `Land_Capture_Allowed_Countdown_Seconds` (+0x11e74) for land or `Space_…` (+0x11e70) for space.
  - Points with no owner, or owned by an `Is_Neutral` faction (+0x108), are serviced at once.
  - FoC 30 s / 30 s; TR 10 s / 10 s. Skirmish delay 0.
- **Port:** drop the inferred "attacker owns points nearer its anchor". Use the map owner, the §3.1 discard rule and the countdown.
- **Probe (run 2026-10-29, tools/probe_mod/s10_map_owners; PHASE4 S10 "Probe run 2"):** the map owner (mini 02) is a **faction index** (FactionFiles order) resolved to that faction's player. Campaign: every faction has a player at its faction index (Rebel 0, Empire 1, Pirates 2, Neutral 3, Underworld 6), so raw r = player r for every found object. Skirmish: the playing sides are in slot order (Empire 0, Rebel 1), Neutral 3, Hostile 6; raw 3 -> Neutral, raw 7 (Hutts, no player) -> Hostile. Almost every reinforcement / capture point is raw 3 (Neutral). **Attacker landing:** the attacker starts on a Neutral reinforcement point (3 / 17 units from its start centroid) that it owns at the first frame; every other point stays Neutral.
- **Port (2026-10-29):** `TacticalBattle::bt_map_owner` (faction -> first player of that faction; a faction neither playable nor neutral without a player -> the Hostile player [inferred]; else unknown id, dropped); `bt_landing_points` gives an attacker without points the Is_Neutral-owned REINFORCEMENT_POINT nearest its anchor [inferred: nearest the Attacker Entry marker]. Campaign galaxies add a Hostile player (faction order, inert) so native owners resolve in campaign battles [inferred].

### 3.4 TEAM objects for companies (`Create_Team`) [H; M for the land snap and some callers]
- **Start placement 0x2a00b0.**
  - Each content object of a galactic company is spawned in content order as its mode variant (0x3729f0), with +0x54 := the persistent id.
  - The team helper runs when the **first spawned member's type has `Create_Team`** (+0x67), **or always in space**: 0x2a5a60(GOM, members, company type, 1, …).
- **Team helper 0x2a5a60:**
  1. k = the **company's** `Max_Squad_Size` (company type +0x1e44, default 10).
  2. Walk the members in list order in chunks of min(k, remaining). Grouping is by position, not by type.
  3. Per chunk, create one container of the company's `Create_Team_Type` (+0x1e50 / resolved +0x1e58). Fall back to the type named **"Team"**; if that does not resolve either, nothing is created.
  4. Owner = the first member's owner; facing = the first member's facing; position = the centroid of the chunk (float mean).
  5. In land, the centroid snaps through a passability query using the team type's movement field (+0x1688) [M].
  6. TEAM behaviour (slot 0x16): its type is the team type, or the company type when `Create_Team_Type` is empty. The company type is recorded, and members are added in chunk order (0x404190).
  7. Registration: 0x2b3670, then 0x3a82a0.
  8. Space with the formation flag: members go to the team's formation offsets (0x4fa6f0 / 0x4fa6e0 / 0x406510). In land, members keep their positions.
  9. With the last argument set, the team gets +0x3a2 |= 2. The first team is returned.
- **Land members without `Create_Team`** stay separate, with no container.
- **Same helper elsewhere** [M]: REPLENISH_WINGMEN 0x3a2970, SpawnSquadron 0x593960 / 0x595a30, SpawnIndigenousUnits 0x531eb0, 0x2bf2d0, 0x29e960, 0x2a5790.
- **The land reinforcement drop footprint** (0x643130) uses the same squads of min(count, `Max_Squad_Size`).
- **Port:** grouping by `Max_Squad_Size` is right. It must also use:
  - the company's size;
  - list order;
  - the "Team" fallback;
  - centroid placement;
  - the first member's owner and facing;
  - containers for every company in space.

---

## 4. Movement

### 4.1 Corridor squeeze (formation path narrowing, 0x412210) [H unless tagged]
Runs once per land formation after its shape (MOVEMENT §4.3) and path are known.
- W = the formation's full width = 2 × half-width (+0x254). If that is 0, W = 20 × the cell width (0x41a740).
- D = the depth in cells = 2 × (+0x250) / 20. If that is 0, it is recomputed with the §4.3 size rule.
- **Sample step** s = **30** world units (0x140b2f0f4 = 1.5 × 20, set by the static initialiser 0x1400183f0).
- **Sampler by formation type** (+0x218):
  - 1 = clearance probe 0x4138b0;
  - 2 = fixed width: every sample is (W/2, W/2), stepping s to the path end;
  - any other type: no squeeze.
- **Clearance probe** (h = W/2). If flag +0x9c is set, every sample is (h, h). Otherwise, per path sample (cursor step s):
  - cast a length-h passability line (0x5a4c10, the formation's movement class +0x1c4) along the left normal (−dy, dx). Clear → h; hit → the hit distance. Do the same on the right.
  - **Refine** a shortened side: step the cursor back by s, take 4 probes s/2 apart, and keep the minimum.
  - **At a vertex** (the waypoint index changes): α = acos(dot of the incoming and outgoing directions); n = int(α·h / (s/4) + 1) rays at angles k·α/n on the turning side, both sides of the vertex, length h. Each side keeps its minimum, and the previous sample is min-merged into the current one.
  - Vectors use the fast inverse sqrt (MOVEMENT §3.5 item 5).
- **Look-ahead window:**
  - K = int(W / (2s) + 2) − 1;
  - Lᵢ = min of L over [i, min(i + K, n)), and the same for R, written in place;
  - wᵢ = Lᵢ + Rᵢ.
- **Smoothing,** front to back, for each wᵢ < W:
  - **back-fill:** walk j = i−1, i−2, … setting wⱼ := wᵢ. Stop at the start, or once i − j > 2 and wⱼ ≥ W;
  - **forward fill:** walk k > i while wₖ ≥ wᵢ. A sample with wₖ < W fills the gap since the last one with wᵢ and resets the gap counter. A full-width sample grows the gap. The walk stops when the gap reaches int(2.5 × D) samples.
- **Outputs:**
  - width-change table +0xb8: 12-byte {width in cells = wᵢ/20, 0, flag}. Entry 0 = full width; an entry is added only on change;
  - per-sample index list +0xd0: 0 when wᵢ ≥ W, else the current entry;
  - lateral shift list +0xa0: {0, shift, 0}, with hᵢ = min(wᵢ, W)/2. The shift is 0 unless Lᵢ < hᵢ or Rᵢ < hᵢ. Then shift = hᵢ − Rᵢ if Rᵢ ≤ Lᵢ, else Lᵢ − hᵢ, which centres the block toward the roomier side.
- 0x4241b0 and 0x415720 then re-sample the path at step s [M].
- No RNG; everything runs in path order.

### 4.2 Space operators 25–32 (compass moves) and threat mode [H unless tagged]
- **Operator sets** (planner setup 0x59a900, inclusive ranges):
  - plain: 0–7, or 0–9 with turn-in-place (request +0x12e);
  - facing mode (+0x120): 10–24;
  - **threat mode** (+0x121): **25–32 only**;
  - after the first expansion, 9 → 7 and 24 → 22, so turning in place happens only on the first expansion.
- **Threat mode** is honoured only when the unit's `Space_Layer` (type +0x16dc) is 0 (None or unset). Otherwise it is cleared.
  - Fighters (no tag), the 11 FoC types tagged None, and 1 TR type qualify.
  - Corvettes, frigates and capitals never use it.
- **Request** (0x417520):
  - threat flag (+0x69) set when the order threshold T (order +0x54) ≠ 1e18 (the "off" sentinel);
  - threshold +0x50 = T;
  - weight +0x54 = 1 / max(T, 1) [M].
- **Where T comes from.** `GameObjectWrapper.Move_Internal` (0x57da90) takes T from an optional numeric Lua argument, default 1e18 [H plumbing; M which argument slot]. Shipped `Move_To(target, 10)`: FoC 4 scripts (MoveToLocation, HideSurpriseUnits, BusyTacticalFreeStore, PGEvents); TR 12.
- **Compass table 0x140a43b80** (not normalised):

  | op | 25 | 26 | 27 | 28 | 29 | 30 | 31 | 32 |
  |---|---|---|---|---|---|---|---|---|
  | (dx, dy) | (0, 1) | (1, 1) | (1, 0) | (1, −1) | (0, −1) | (−1, −1) | (−1, 0) | (−1, 1) |

- **Operator body** (0x596d40 cases 0x19–0x20):
  - d = min(3-D distance to the goal, step +0x188); x += d·dx, y += d·dy (a diagonal covers d·√2);
  - time += length / speed (+0x194), measured at the node's z;
  - the heading is unchanged; then z := goal z (+0xcc).
- **Cost** (0x599380):
  - reject a successor outside the playable box;
  - with a threat grid: t = the threat at (x, y, 0) for the owner (TacticalPerceptionGrid point query 0x653ff0, AI_TACTICAL_PERCEPTION §4.6);
  - if t ≥ the threshold, cost × max(1, t·weight + 1), then the normal obstacle multiplier;
  - the land region planner uses the same rule (0x5a38a0, fields +0x134 / +0x138, cell-centre query 0x5a37d0).
- No RNG.

### 4.3 JETPACK_LOCOMOTOR (vf6 0x641910, land only) [H unless tagged]
Users: FoC Boba_Fett, Dark_Trooper_PhaseII and Dark_Trooper_PhaseIII, all as `Secondary_Locomotor_Name` (they spawn on
WALK and swap, MOVEMENT §3.1.1); TR 6.

| state | behaviour |
|---|---|
| 0 None | with a path → 0xF if path +0x6d is set, else 0x16 |
| 0xF Start Turn | if type +0x36: turn anim 0xC (error in (0°, 180°)) or 0xE; then 0x10 |
| 0x10 Turning (0x642740) | yaw toward the next waypoint at +0xe8 or the type turn rate (0x372560); details below the table |
| 0x11 End Turn | end anim 0xD / 0xF (0x640b30), then 0x12 |
| 0x12 Start Move (0x642530) | take-off; details below |
| 0x13 Moving (0x6420e0) | flight and landing test; details below |
| 0x14 End Move (0x641df0) | landing sequence; details below |
| 0x16 Stopped | speed 0, idle; then swap back to the primary locomotor (0x3a9910(obj, system, 1); MOVEMENT §3.1.1) |

- **Turning** (0x10). With type +0x36 set, for heading error e and type turn rate r:
  - |e| < r: no animation;
  - |e| < 22.5°: anim 0x16 / 0x18 at rate r × 0.25 × |e| / 11.25;
  - |e| < 45°: anim 0x15 / 0x17 at rate r × 0.5;
  - otherwise anim 3 / 4 at the full rate.

  Without +0x36: anim 3 / 4 at speed (angle / rate) × 0.03333, capped by GC 0x140b16d28.
- **Start Move** (0x12): anim 0x30 (Jump). When it finishes, shift every waypoint time by (now − first waypoint time), loop 0x31 (FlyIdle), set airborne (0x3a7e30) → 0x13.
- **Moving** (0x13): follow the timed path (vf +0x1d8 [M]) and set the altitude (below). No movement → 0x16. Path done → 0x14.
  - **Landing test,** while FlyLand is not yet playing and (last waypoint time − now) ≤ 7 frames (0x140a44334): the last waypoint must pass the movement-class test (0x50e280) and be unoccupied (0x538200). Then play 0x32 (FlyLand) and clear airborne. Otherwise re-issue a move to the same point (0x640880, radius 50).
- **End Move** (0x14):
  - if 0x31 is playing, play 0x32; when 0x32 ends, loop 0x33;
  - during 0x33, z descends by at most Max_Lift per frame;
  - when |z − ground| ≤ 0.01, play 0x34; when 0x34 ends, go to 0x16 and snap to the terrain.
- **Altitude** (0x6404e0):
  - base = formation slot z offset + `Hover_Offset` (+0x1b68);
  - look ahead 60 frames (object tracker 0x538830 at now + i, or extrapolate facing × speed);
  - inside the map box: h = max(terrain + Hover_Offset, map floor 0x136650); if h > base, h = (h − base)(1 − i/60) + base;
  - target = the maximum h;
  - z steps toward the target by at most ±Max_Lift (+0x3a0 × the mode multiplier 0x372440), then is clamped to ≥ the terrain and ≥ the map floor.
- Waypoints are timed (0x34-byte records, time +0x30). So the jetpack uses the **timed lattice planner on land** [M]; this answers MOVEMENT §8 item 6.
- No RNG. The ability side is §5.3.

### 4.4 FLYING_LOCOMOTOR (vf6 0x63d7f0, land only) [H unless tagged]
Users: FoC 5 types, TR 20.
- **States:** 0 → 0x13 Moving; 0x26 Circling (0x63db30); 0x27 No Path (0x63e2b0); 0x16 idle.
- **Moving:** follow the timed path, then step attitude and altitude (0x63b920), then commit. Path done → 0x27.
- **No Path / loiter:** keep flying forward, stepping pitch and bank (0x63d530), in a one-unit formation.
  - **Re-issue,** when the formation has been idle for > 2·FPS + 1 frames (+0x1dc) and has no target:
    - L = 1.1 (0x140a4432c) × `XYExpansionDistanceLand` (GC +0x147c);
    - at least 0.3·L from the destination: aim at the destination direction rotated by **one synced float(−90°, +90°)** (0x1ffbb0; +90 bound [M]);
    - otherwise turn back along the facing (+0x240 set);
    - then move to unit + dir × L.
  - With an attack target: aim at target + facing × `Strafe_Distance` (+0x2118), at height `Prepare_Strafe_Height` (+0x1e24).
- **Circling** around +0x278:
  - beyond the max radius (+0x284): head straight at the target;
  - between the min (+0x280) and max radius: fly tangent, side by the sign of the cross product with the velocity;
  - inside the min radius: head away.
  - Altitude target = max(target z, terrain at 11 points along the velocity) + Hover_Offset. The z step is clamped to ± the max speed (0x370f00). A descent is scaled by (z − target) / max(1, Prepare_Strafe_Height − Hover_Offset).

### 4.5 BIKE_LOCOMOTOR (vf6 0x639e90) [H]
Users: FoC Scout_Trooper only; TR none. States 0x20–0x25 mirror the walk locomotor:
- 0x20 Turning In Place;
- 0x21 Start Move: anim 0x10;
- 0x22 Moving: anim 2, step 0x63abe0 (walk-style budget and test 0x50e280);
- 0x23 End Move: anim 0x11, until the speed reaches the minimum + 0.0001;
- 0x24 Pre Stopped (0x63a8e0);
- 0x25 Stopped.

**Lean** (+0x1f8):
- during turn anim 3, lean −= roll rate; during anim 4, lean += roll rate; otherwise it decays toward 0 by the roll rate;
- roll rate = `Max_Rate_Of_Roll` × the mode multiplier (0x3724d0);
- clamped to ±`Bank_Turn_Angle` (+0x39c) and added to the roll after the terrain conform.

No RNG.

### 4.6 LANDBOMBER_LOCOMOTOR (vf6 0x646800) [H]
Users: FoC 3, TR 1.
- **State 0x13:** follow the timed path (0x645370, bomber vf +0x308). The clock +0x1e8 advances by GC 0x140b16c4c per frame.
- **At path end:** the bomber is killed silently (0x391310 with flag 1).
- **State 0x16** (0x646990): new pass to the formation destination + 2 × `XYExpansionDistanceLand`, at z = `Prepare_Strafe_Height` + the target z.
- No RNG.

### 4.7 FIGHTER_LOCOMOTOR (vf6 0x5cb830) [M]
- **Hyperspace states:** 4 → 0x5cc970; 5 → 0x5cd010; 0x28 → 0x5cd680; 0x2c → 0x5cbc10.
- **Other states:**
  - 0: → 0x1E with an attack target, else 0x1B;
  - 0x1B Moving (0x5ce010): → 0x1E when a target appears;
  - 0x1C Idle (0x5cd8e0): squadron-offset station keeping, blending 0.75 × (slot − position) with velocity × 40 (×60 look-ahead terms);
  - 0x1D Idle_Combat: no-op;
  - 0x1E Directed_Combat (0x5cc220): the attack run, **untraced**;
  - 0x1F Dead_Stop (0x5cbea0): repositions above 1.33× the threshold, with a doubled step.
- **Per frame:** a pitch beyond ±90° folds (yaw += 180°, pitch = ±(180° − |pitch| − Max_Lift), roll += 180°). Squadron membership is synced (0x4f9d40 / 0x4f9d50).
- None of the five handlers read uses the synced RNG.

### 4.8 Still open (movement)
1. Fighter attack-run geometry (0x5cc220, 0x5ce010) and the four hyperspace handlers.
2. The timed-path follower vf +0x1d8 (assumed to be the SimpleSpace Hermite timing), and the flying attitude steps 0x63d530 / 0x63b920.
3. The Lua argument slot of T. Optional probe: a fighter `Move_To(x, 10)`, checking the path for compass steps.
4. Optional probe: synced draws during a flying loiter (expect one per re-issue, more than 61 frames apart).

Items 1 and 2 are answered in FIGHTER_FLYING.md (fighter states incl. 0x1E, cell grids, flying attitude / altitude /
circling, follower vf +0x1d8 = SimpleSpace Hermite with strict bracketing); its §9 corrects §4.4 and §4.7 above.

---

## 5. Abilities

### 5.1 EJECT_VEHICLE_THIEF (unit ability 46, inline in 0x38a350) [H flow, M meaning]
- **On:** if the vehicle has the countdown behaviour (0x46), start 46's **recharge** (0x42fb70) and set the slot byte. Nothing else.
  - The FoC record comment ("time that the user can't eject thief…") fits: the captured variant gets 46 on at capture, which blocks ejecting for `Recharge_Seconds` (FoC 10 s).
- **Off (46 off, and also CAPTURE_VEHICLE 18 off, which shares the code):**
  - 18 off first restarts 18's recharge when 18 is present and active (0x39b950(obj, 18, 1)).
  - Then, when 46 is present and active (team members included):
    - the thief = the **first occupant** of the vehicle's container list (obj +0xf8 → list +0x20, count +0x28). An empty list → false, nothing happens;
    - unless the vehicle is already dying (+0x3a0 bit 0x40): `Take_Damage(kind 0xD, 1,000,000, source = thief)` (0x3a9e30). **The eject kills the stolen vehicle**, and the kill is credited to the thief;
    - the thief's 18 starts recharging (0x3a9840).
- The thief leaves through the vehicle's normal death and garrison release (OBJECT_RUNTIME). The death path draws (TACTICAL_GAPS §1.9); the eject itself makes no draw.
- Users: FoC 18, TR 107.

### 5.2 DEPLOY_TROOPERS spawn (state 4 of 0x591a90 → 0x590730) [H]
- **Spawned type.** Ability 38's record `Spawned_Object_Type` (+0x90 / +0x98) must have `Company_Units` (+0x958 / +0x960) or else `Squadron_Units` (+0x978 / +0x980). If neither exists, or there is no model → state 10 (failed).
- The owner first plays anim 0x5e.
- **Count:** N = member count − troopers already deployed (behaviour data +0x40).
- **Bones.** Walk the record's `Owner_Attachment_Bone` list (+0x110) by index i:
  - a bone the **render model** lacks is skipped, but it still uses member index i;
  - for each bone found, spawn member type i (0x29f810, **one object id each**) at the bone position, with the owner's facing and player;
  - locomotor vf +0x200; append to the trooper list (+0x10), +0x40 += 1;
  - play anim 0x5f (0 when missing), attach at the bone (0x38d320), +0x3a2 |= 2;
  - stop at N.

  So the spawn count = min(N, bones present). Then state 5 (release, LANDING 0x1e).
- **No draws.** `Max_Num_Spawned_Objects` (+0xa0) is not read on this path [M].
- **Headless:** spawn positions come from model bones (0x12d2a0 / 0x12d2c0), so headless runs need the skeletons.
- **Data:**
  - FoC: 5 users (AT-AT / Veers). `Veers_Deployed_Imperial_Stormtrooper_Squad` has 9 members and bones B_Trooper_00..02, so **3** troopers deploy.
  - TR: 20 users, the same 3 bones, min(members, 3).

### 5.3 JET_PACK (unit ability 17, 0x3a38d0), ability side [H flow, M order flags]
- **Gates** (TACTICAL_GAPS §2.1, confirmed):
  - Force / garrison / mode / team `Can_Activate`;
  - 2-D distance ≤ record `Effective_Radius` (+0x20) unless it is negative;
  - (corrected 2026-11-01, S7 t11) there is no garrison release here. The type +0x74 test belongs to the
    **locomotor swap**: after the gates pass, when the unit has a locomotor (obj +0x333 ≠ −1), type +0x74
    (`Uses_Multiple_Locomotors`) = 1 and the mode has a movement-coordinator system (mode vf +0x228), it calls
    0x3a9910(unit, system, 0): swap to `Secondary_Locomotor_Name` (JETPACK_LOCOMOTOR in FoC). That re-issues the
    unit's current order in a coordinator of its own, rebuilds the locomotor (pack state 0) and disables TARGETING,
    WEAPON and SPECIAL_WEAPON for the flight (MOVEMENT §3.1.1). It is not a gate: the ability goes on either way.
- **Order.** Builds an ordinary move order for the unit or its team and executes it through 0x2a7b80, which takes the
  unit out of the swap's coordinator into a new one. Move record: destination = the target point, flags +0x48 = 0x04,
  mask +0x4c = 0x3f, +0x60 = 0x1000001, type 1 [H offsets; meanings MOVEMENT §6.1]. The −999 is a field of the
  temporary unit list (the ReferenceListClass local passed as the selection), not of the move record [M]. The JETPACK locomotor (§4.3) does the
  flight and swaps back to the primary in state 0x16 (after the landing).
- **Countdown.** The acting record (the member's record in a team) with `Expiration_Seconds` (+0x28) = 0 starts recharge at once (0x42fb70); otherwise the active phase starts (0x42fde0).
  - Every shipped user has no Expiration, so 17 is never "active". The locomotor cannot use `Is_Active(17)` as a flight flag [M].
- No RNG in the ability itself (the swap's re-issued `Move_To` may draw formation stagger, MOVEMENT §3.1.1 [M]).
- **Data:**
  - FoC: 6 users, recharge 28 / 45 / 60 s, `Effective_Radius` −1 on two;
  - TR: 7 users, recharge 40 / 45 / 90 s, −1 on all five that set it.

### 5.4 Stun release (STUNNABLE 0x38: apply 0x647290, service vf6 0x647200) [H]
- **Apply:**
  - if +0x3a0 bit 0x10 is clear, set it and **suspend** (vfunc +0xc0) behaviours 1, 9, 0xB and 0x35 (table 0x1408b38f0);
  - resume STUNNABLE itself (vfunc +0xb8) and set its next service (+0x30) := mode frame + frames. This is a plain assignment, so a second stun can shorten or lengthen the stun;
  - break stealth (0.3 s); attach `Stunned_Effect`.
- **Release** (first service at or after +0x30): if bit 0x10 is set, clear it, **resume** the four behaviours in table order, then suspend STUNNABLE. The stun effect is not removed here [M: its own lifetime].
- No RNG.
- Callers: the land detonation stun (0x4d01c0, TACTICAL_GAPS §1.10) and `Stun_Ability` (0x712190).

### 5.5 Blast (`Blast_Ability`: apply 0x71c0b0, update 0x71c190, consumer `Fire_At` 0x3825b0) [H]
- **Apply:**
  - counter (SAP pack +0x394) := int(FPS × `Charge_Up_Seconds`), 150 frames for 5 s;
  - "buildup" mesh shown; charging particle id → +0x398;
  - recharge 70 starts; +0x3a1 |= 0x10.
- **Counter > 0 (charging):** `Fire_At` refuses **every** hardpoint and weapon of the unit. The update decrements the counter.
- **At 0:** the charging particle is destroyed, the "charged" mesh shown, the charged particle id stored, and counter := **−K**, where K = the number of WEAPON_SPECIAL (type 10) hardpoints (0x397c10).
- **Counter < 0:**
  - only type-10 hardpoints may fire, using the alternate projectile (+0x220) when set, else +0x210;
  - each shot does counter += 1, and its damage (hardpoint +0x478 when > 0, else projectile +0x474) is **multiplied by `Damage_Multiplier`** (special +0x17c);
  - at 0: meshes hidden, particle killed, +0x398 := 0x3fffff, normal fire resumes.
- No RNG.
- **Data:** FoC Peacebringer_Blast (5 s, ×5); TR Iblis_Ion_Blast (5 s, ×5; SuperCapital | Capital | Corvette | Frigate).

### 5.6 Leech_Shields (apply vf30 0x7177a0, update vf4 0x717d60, remove 0x717f70) [H; M for the source key]
- **Apply:** needs a live WEAPON_SPECIAL hardpoint (58), the range and accept gates, and a target. Then:
  - 58 on;
  - `Damage_Multiplier` (+0x188) is inserted as a **permanent** entry (duration −1, replace by source 0x3fe810) into the **owner's own ModifierSet list 1, "damage dealt"** (obj +0xf0 → +0x138, list +0x20), stacking category 0, with the ability source key (0x48e1e0) [M]. So **all damage the owner deals is × (1 + m)** while draining;
  - counter = int(FPS × `Duration_In_Secs`) (+0x180); effect object id → +0x374; recharge 58 starts.
- **Update:**
  - end at once if 58 cannot activate, the target is lost or out of range, accept fails, or the counter hits 0 after the decrement;
  - otherwise the target's shield takes `Shield_Damage_Per_Second` / FPS (0x549ac0, source = owner);
  - end when the target's shield fraction is 0.
- **Remove:** 58 off; every list-1 entry with that source key deleted; effect destroyed; counter 0.
- No RNG.
- **Data:**
  - FoC Kadalbe_Leech_Shields: 25 s, 100 DPS, m = 3.0 (**×4 dealt**), range 10–2000, `Must_Be_Bought_On_Black_Market`;
  - TR Sykes_Leech_Shields: 20 s, 100 DPS, m = 1.0 (×2), range 10–3000.

**Synced RNG added in §5: none.** The only draws on these paths are the existing Vehicle_Thief chance (TACTICAL_GAPS §2.3.10) and the death-path draws the EJECT kill triggers.

---

## 6. Galactic

### 6.1 Launch reveal (`Galactic_Influence_Range`, 0x4f40c0) — corrects STRATEGIC_GAPS §2.2 [H mechanism, M trigger and reader]
- **Trigger.** Runs only from 0x4f4690, the TRANSPORT "leave the surface" handler. That handler removes the transport from D +0xe0, places it at the planet, then calls 0x4f40c0. Callers:
  - land auto-resolve 0x2b76a0;
  - invasion result 0x2a7580;
  - 0x2ba850;
  - planet object removal 0x3f7950;
  - FleetManagement 0x5af260.

  So the reveal happens on lift-off [M].
- **Range** R = the maximum `Galactic_Influence_Range` (type +0x1a00) over the transport and its cargo.
- **R ≤ 0:** the transport and cargo go into the launch planet's set only. Then the planet visibility is refreshed for the owner (0x3a51e0).
- **R > 0:**
  - candidates = PLANET objects from the radius query (0x2a9ff0, category 3, centre = launch planet, radius R);
  - the transport is added to every candidate within its range (squared 3-D distance with ≤, 0x397640);
  - each cargo unit with range 0 goes to the launch planet only; a non-zero range adds it to every candidate within its own range;
  - every candidate gets a visibility refresh for the owner.
- **"Added"** (0x3fd9e0) means insertion into a unique set at D +0xf0. It is not a reveal byte.
  - FLEET-slot objects (+0x336 ≠ −1) are skipped.
  - On first insertion only: an event (0x220eb0) and a behaviour-manager notification (0x3a04d0 → 0x37cf20).
  - No direct reader of +0xf0 was found; treat it as an influence set feeding visibility [M].
  - Other writers: 0x3f7950, 0x3fcd60, 0x434940, 0x435020 (fleet arrival / orbit, same per-unit rule), 0x4bb4a0.
- **Data:**
  - FoC: Mara_Jade, Obi_Wan_Kenobi, Darth_Vader (+ Expansion) and General_Dodonna 100; planet Bothawui 150; Orbital_Long_Range_Scanner types 0.
  - TR: 8 heroes at 200 (Isard, Kadann, Odumin, …) and `Cheat_Dummy_Vision` 2.5e8.
- No RNG; candidate order = query order.

### 6.2 `Is_Stealth_Company` (type +0x2b) [H readers, M meaning]
- 0x48d5e0 is **not** its reader. It tests +0x2b of an ability-enable map entry, inside the per-object ability-enable lookup used by 0x39b310 / 0x48e240.
- **The only type readers** are ProductionBehaviorClass 0x6955a0 / 0x695c90, from the AI build-delivery check 0x53cd60. They choose the AI path class for moving the produced unit:
  - 2 by default;
  - 3 when the task flag +0xda is set;
  - **5** when the type `Is_Stealth_Company`.

  The class goes to the AI path / threat vfunc +0x28 [M: 5 = ignore enemy-held planets].
- No galactic-sim reader; actual hiding is only `Galactic_Stealth_Ability`.
- Data: FoC 18 types, TR 9.

### 6.3 `Planet_Starbase_Stealth` / `Planet_Groundbase_Stealth` (+0xbb / +0xbc) [H]
Display queries only: the per-bit planet-info queries 0x50b2a0 … 0x50c420. Callers are the galactic HUD only (labels, organiser, fleet / credit tooltips); the `IDD_PLANET_INFO` dialog uses none of them (GALACTIC_DIALOGS §9.3, 2026-10-28).

**Correction 2026-10-28.** This section used to call 0x50b3f0 / 0x50b970 the "starbase info" / "ground base info" checks with bits 0x40 / 0x100. The bits were right, the labels were wrong. 0x50b3f0 is the **Fleet_Contents** (0x40) query and 0x50b970 the **Ground_Company_Contents** (0x100) query; the base-stealth flags only gate them. The actual base-level queries are 0x50c420 (Starbase_Level, 0x2) and 0x50b840 (Ground_Base_Level, 0x4). The bit names in GALACTIC_REVEAL_CORRUPTION §2.2 are correct. [H]

0x50c550(planet, &player) only returns the folded mask. If player is −1 or the local player, it returns PD +0x1A8 and writes the local id back. Otherwise it folds that player's list (PD +0x1B8[player +0x48]) with the min/max clamps. Each query then tests its own bit. [H]

Common shape of a query "P can see X" [H]:
1. **Corruption bypass.** True if the planet is corrupted (D +0x2f4 ≠ −1) and P's faction (+0x68) has `Benefits_From_Corruption` (+0x10b).
2. **Stealth gate.** With starbase stealth (+0xbb) set and a starbase owner (D +0x13c ≠ −1), P must pass 0x2823c0 [M: allied with that owner], else false. Ground-base stealth (+0xbc) does the same with owner D +0x124.
3. **Bit test** on the mask from 0x50c550.

| Query | Bypass | Gate | Test |
|---|---|---|---|
| 0x50c420 starbase level | yes | +0xbb | bit 0x2; else true if P passes 0x2823c0 with the starbase owner, or P's faction +0x20ac ≤ D +0x148 (starbase level) [M: field meaning] |
| 0x50b840 ground-base level | yes | +0xbc | bit 0x4; else the same fallback with owner D +0x124 and level D +0x130 |
| 0x50b3f0 fleet contents | yes | +0xbb | 0x40 |
| 0x50b4c0 fleet presence | yes | +0xbb | 0x10, or 0x50b5a0 |
| 0x50b5a0 number of fleets | yes | +0xbb | 0x20 … (rest not traced) |
| 0x50b970 ground company contents | yes | +0xbc | 0x100 |
| 0x50ba40 number of ground companies | yes | +0xbc | 0x80 or 0x100 |
| 0x50bb10 most powerful ship | **no** | +0xbb | 0x10000 |
| 0x50c300 special structures | yes | both | 0x200 |
| 0x50c270 political control | yes | none | 0x400 or 0x800 |
| 0x50b2a0 credit income | no | none | 0x8 or 0x8000 |
| 0x50b2d0 credit income breakdown | no | none | 0x8000 |
| 0x50b370 / 0x50b2f0 minor / major stealth heroes | yes | none | 0x2000 / 0x4000 |
| 0x50b820 force-sensitive heroes | no | none | 0x20000 |

0x50bf10 combines several of these into the ownership check: true for the local owner or with bit 0x1 (only when neither stealth flag is set), or when P has an object in the planet's list 0x395900. Otherwise it falls through to the base-level, ground-company and orbit checks. 0x50bba0 is the per-object form: special structures 0x200 under both gates, or 0x2000 / 0x4000 for stealth heroes. [M: composite reading]

No sim effect. Data: FoC 1 planet with starbase stealth and 7 with ground-base stealth; TR never sets either to Yes.

### 6.4 The story flag that blocks battles (BATTLE_TRANSITIONS §1.4 step 2) [H]
- **Flag.** 0x140b154d0 = GMM +0xf0, the "queued battle record present" byte (BATTLE_TRANSITIONS §2.7).
- **Set by** 0x28bd70 (refused when a record is already queued), from:
  - story **LINK_TACTICAL** (reward 0x3e, 0x45f9c0);
  - the corruption-mission start 0x52bf50.

  The same call sets 0x140b30545 and 0x140b305ed.
- **Cleared by** 0x28c640 when the queued battle starts (0x28f590), by 0x28e890 (new game / map setup) and by the
  record destructor 0x289840 (GMM dtor 0x289750). Correction 2026-10-31 (BATTLE_TRANSITIONS §2.9): with the default
  LINK_TACTICAL Param13 = 1 the record stays set while the one-button pending banner is up (pause raised, the
  conflict not yet queued, so the state-machine block does not apply); with Param13 = 0 (all Underworld links)
  0x28f590 clears it in the same call that wrote it, so the block is never seen.
- **The list at 0x140b2b360** has no writer anywhere in the binary (reads plus the destructor 0x7ef5b0 only), so it is always empty.
- **Effect.** While set, in SP and when the local player is in a queued conflict (0x2b0350: the local player is
  attacker or defender of any record, not only the head, in the space or the land queue; the SP test is in the
  callers; A1_READINGS §31.6):
  - conflict state machines do not run (0x2be640);
  - SpecialAbility commands are dropped (0x4b4d70 prologue);
  - FleetManagement 0x5af260, Lua free-store `Move_To` 0x68f5b0 and the GUI moves 0x3c4460 / 0x48ff50 refuse moves;
  - the galactic "paused" query 0x4b2360 returns true.

  These are all seven callers of 0x2b0350. None of them tests the event's issuer, so AI players' commands are blocked
  only while the local player is in a queued conflict (table: A1_READINGS §31.7, a1i t03).
- **Port:** `linked_battle_pending`, set by LINK_TACTICAL and the corruption mission, cleared when that battle starts.
  `GalacticSim::story_block()` = the flag and `in_queued_conflict(local player)`; `paused()` = pause counter or
  `story_block()`, used by every command gate (a1i t03, 2026-11-02; it replaces "[port: for every player]"). Still
  [port]: the mode-frame freeze in `GalacticSim::update` ignores the story block; 0x2be640's block is not wired
  (`CampaignBattles::story_sequence_active`).

### 6.5 Build pads (extends L1_S4_REMAINING §3.6) [H unless tagged]
- **UC attachment bone** (0x42e280):
  - no pad model (+0x2a0) → the pad position;
  - otherwise the bone `Tactical_Build_Attachment_Bone_Name` (type +0xa38; skipped when empty) in the pad model (0x12d2a0), world translation from 0x12d2c0 with an identity input. A missing bone → the pad position;
  - the finished object is created at the same point + 0.1 z;
  - the pose is the current pose [M: static pads, so the bind pose suffices].
  - Data: FoC `ATTACH_POINT_00` on 5 pad types; TR sets none, so TR always uses the pad position.
- **Galactic persistence** (0x42f160):
  - **gate:** tactical mode with a galactic-campaign parent (not skirmish); the battle record's planet id ≠ 0x3fffff; parent GOM state +0x618 ≠ 9; the battle planet has PLANET and planet data.
  - **add** (0x4b6c70): appends {type index (+0x11c), pad x, y, z, small int ≤ 5, int} (0x18 bytes) to D's persistence vector (+0x10, count +0x18). Refused when the position is already present or the small int > 5. Logs "%s: Successfully added to persistent tracking".
  - **remove** (0x4bb3a0): position and type must both match.
  - **restore** (0x4b7c00): matches exact float x and y; the **last** match wins.
  - Used when `Tactically_Built_Child_Object_Persists` is set; child death or sale removes the record. The ints' meaning [M: health / level]; save chunk [L].
- **The "kill list" +0xa60** is the single type `Previous_Upgrade_Level_Type` (+0xa58 name). On construction complete (0x42e890), every object of exactly that type in the mode (0x2a87d0) whose owner is allied with the pad owner (0x39a230) is killed silently (0x391310 with flag 1).
  - Data: FoC UL_*_L1 → L2 chains (8 types); TR tech / supply upgrade chains.
- **Capture-point reset:** when the child goes away and the pad has CAPTURE_POINT (slot +0x36f), 0x501050(capture behaviour, neutral player 0x294aa0, −1) runs:
  1. owner := neutral (0x38e5a0);
  2. capture record +8 := the neutral id;
  3. skin / particle refresh (0x3a92f0, 0x371070);
  4. reveal refresh (0x5013b0, 0x266040);
  5. state notify (0x5021f0).

  No RNG. With −1 as the second argument the function takes the dominant occupant (0x500a00).

---

## 7. Probes (prepare under `tools/probe_mod/`; paused)
1. ~~Map owner ids vs player ids in campaign land battles (§3.3).~~ Run 2026-10-29: faction index -> player (§3.3).
2. Slice success / failure kinds and the draw count around an AI R2D2 slice: debugger hit counter on 0x532480 / 0x1ffbb0 / 0x1ffb40 (§1.3).
3. Optional movement probes, §4.8 items 3–4.

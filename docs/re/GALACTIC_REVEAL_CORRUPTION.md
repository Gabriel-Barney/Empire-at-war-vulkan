# Galactic planet reveal and corruption transition fraction (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-28 by an RE agent for the engine agents that own `engine/galactic` (reveal state, visibility
masks, corruption transitions) and the presentation side (planet corruption particles). Clean-room: prose and
tables only. Tags: **[H]** read directly in the decompile or disassembly, **[M]** strong inference from
structure, data or call sites, **[L]** guess. RVAs are image-relative. "PD" = the planet data pack
(`PlanetaryDataPackClass`, planet object +0xB8). "GC" = GameConstants (base 0x140b15800). "Local player" =
0x294a40 on the player list 0x140a16fd0.

Builds on: STRUCTS.md (PlanetaryDataPackClass), PHASE2_ANSWERS Q5/Q6, GALACTIC.md (hop gate), FOG_OF_WAR.md
(tactical grids), STRATEGIC_GAPS §2.2 / §6.2, REMAINING_GAPS §6.1 (launch influence set), STORY.md (rewards),
L1_S4_REMAINING §4.5 (corruption actions), MAIN_LOOP (galactic pause).

---

## 0. Summary

1. **Correction:** galactic mode **+0x340 is `Planet_Auto_Reveal`**, not a "fog disabled" option. It is copied
   from the campaign (`CampaignDataClass` +0xCB) at mode start (0x28e890, mode type 0) [H]. The campaign default
   is **true** (ctor 0x4ae110 writes +0xCB := 1) [H]. Shipped story campaigns set it False; FoC
   `CAMPAIGNS_UNDERWORLD_E3_DEMO.XML` sets True; every other campaign leaves it at the default (true) [H data].
   PHASE2_ANSWERS Q6 and GALACTIC.md row 5 call it "fog disabled"; read "auto-reveal" there.
2. PD +0x2C9 ("revealed") is **one byte per planet for the local player only**. It is set by exactly three code
   paths and cleared by one: campaign start (home planet), the galactic pre-step 0x4b2600 (every galactic
   frame), and the story rewards REVEAL_PLANET / REVEAL_ALL_PLANETS. Only REVEAL_PLANET with Param2 = 0 can
   clear it, and no shipped event does that [H]. It is saved (PD chunk 1 micro 0x3C) [H].
3. Fleet arrival reveals a planet only after the local player has an unhidden unit in orbit for more than
   `Planet_Reveal_Delay_Time` seconds of galactic frames. `Hyperspace_Fleet_Reveal_Range` does **not** write the
   revealed byte. It controls whether *enemy fleets in hyperspace* are drawn, and it reads the revealed bytes
   (§1.6) [H].
4. The visibility mask PD +0x1A8 (local player) is the bitwise OR of a per-(planet, player) modifier list
   (PD +0x1B8). The list is fed by a baseline entry (owner 0x9FFF, else the faction default), the AI presence
   entry, `System_Spy_Ability`, and the story reward SET_PLANET_VISIBILITY_LEVEL. Entries can expire and the list
   is saved (§2) [H].
5. Corruption transition fraction 0x4b7e20 = clamp((now − start) / (end − start)) over the PD frames
   +0x2E8 / +0x2EC, with unsigned 32-bit arithmetic. `end − start` = (int)(logic FPS × seconds). The seconds
   come from the caller: 10 s on an ownership change, `Corruption_Removal_Time_In_Secs` for the Remove
   Corruption ability, 5 s for corruption actions, and so on (§4.3). The fraction itself uses **no** game
   constant [H]. It only drives presentation (particles, path tint, UI percent). Completion is an integer frame
   test (§4.4).
6. Pair→tag mapping, raised to [H]:
   - `Particle_Brightness_Per_Corruption_Level` → PD +0x308/+0x30C → particle colour (scalar 4, RGBA all equal);
   - `Particle_Scale_Per_Corruption_Level` → +0x310/+0x314 → scalar 1;
   - `Particle_Energy_Per_Corruption_Level` → +0x318/+0x31C → scalar 0.

   List element [0] is "not corrupted" (type −1), element [1] is "corrupted" (any type 0..7) (§4.5).

---

## 1. The revealed byte PD +0x2C9

### 1.1 Readers (for context) [H]
- `Is_Planet_Revealed` 0x3fa420(planet): true in multiplayer (GMM 0x28af60); else false without a PD or with
  hidden PD +0x2E0 set; else true if galactic mode +0x340 (**auto-reveal**) is set; else PD +0x2C9. It takes
  **no player**: AI hop gating (GALACTIC.md flag 5) therefore tests the *local human's* reveal state. This is a
  quirk of the original; keep it [H].
- Human single-player hop gate, trade-route visibility (§2.4), the hyperspace fleet visibility test (§1.6), the
  pre-step's presentation state (§1.3 step 5), UI/camera code (0x3de570, 0x3f98a0, 0x3cd4a0, …) read the raw byte.
  The hop gate and trade routes ignore auto-reveal [H].

### 1.2 Writers (complete census) [H]
Every write to PD +0x2C9 in the binary (byte, word and qword writes covering +0x2C8..+0x2CF checked):

| Writer | When | Value |
|---|---|---|
| PD ctor 0x4b5700 | creation | 0 (word write +0x2C8 := 0) |
| PD load 0x4bbc70 / save 0x4bbef0 | load/save | chunk 1 micro 0x3C (1 byte) |
| Galactic mode start 0x28dbe0 | new campaign | 1 for the local player's home planet (§1.4) |
| Pre-step 0x4b2600 | every galactic frame | 1 (§1.3) |
| Story reward REVEAL_PLANET (0x33) in 0x456970 | event fire | atoi(Param2) ≠ 0 (§1.5) |
| Story reward REVEAL_ALL_PLANETS (0x39) | event fire | 1 on every planet (§1.5) |

So the byte is **monotonic in shipped data**: it is never cleared once set, except by a REVEAL_PLANET with
Param2 "0", and no shipped XML has one (284 FoC REVEAL_PLANET events, all Param2 = 1; two in
STORY_CAMPAIGN_EMPIRE_CHEATS.XML have no Param2 and are no-ops) [H data]. Nothing resets it on ownership loss,
fleet departure or corruption removal [H].

`Lua`: no Lua function writes it. `FogOfWar.*` are tactical-only (FOG_OF_WAR §6). Story Lua can reveal planets
only through story rewards [H: census].

### 1.3 Galactic pre-step 0x4b2600(mode, frameArg) [H unless tagged]
This is called first in the galactic mode update 0x4b2ef0 (GalacticModeClass vf22, before the common tick and
the planet services; GALACTIC_ECONOMY §5.1 "pre-step, not traced"). It does nothing without a local player.

For each planet in the GOM category-3 list (0x2a9ff0(gom, 3, −1, −1, null, 0, ∞, −1); list +0x560, the same
order as the income loop), with PD `d`, set `source := false` and do:

1. **Held by me or an ally**: if 0x4ad9f0(d+0x68 control record, local) (owner ≠ −1, relation[owner] == 0,
   control > 0), then `source := true` and go to step 4.
2. **Orbit presence with delay** (counter PD +0x2CC, int):
   - Count, over every fleet in the orbit list d+0xC8 (count d+0xD0) whose owner (+0x58) is the local player,
     the fleet's contents (0x3965f0(fleet, out, 1)): `total`, and `hidden` = units for which the stealth
     predicate 0x3a9760 is true (STRATEGIC_GAPS §2.2).
   - If total < 1, or total ≤ hidden (no unhidden unit), or the galactic mode is paused (vf9 0x4b2360: pause
     counter or pending conflict, MAIN_LOOP), or d+0xD0 < 1, then counter := 0.
   - Else, if `(float)counter <= (float)FPS × Planet_Reveal_Delay_Time` (FPS = int 0x140b0a340; GC +0x11880,
     FoC 0.5), then counter += 1. Otherwise `source := true` (the counter is not incremented further).
   - Consequence: with n = (float)FPS × delay, the reveal happens on the (⌊n⌋ + 2)-th consecutive qualifying
     galactic frame [H arithmetic].
3. **Corruption**: if d+0x2F4 ≠ −1 (any corruption type, whoever made it) and the local faction has
   `Benefits_From_Corruption` (faction +0x10B), then `source := true`.
4. If `source`: if d+0x2C9 == 0, then:
   - d+0x2C9 := 1;
   - planet highlight 0x392ba0(planet, 7) (presentation fade, `GUI_Planet_Fade_Duration` GC +0x1224);
   - visibility refresh 0x3a51e0(planet, local);
   - `newly := true`.
5. Presentation [M]: if the planet object has a render object (+0x2A0), a display state on 0x2648b0() +0xC4 is
   set to 2 when unrevealed, to 1 when revealed and (0x140b2eea0 is null or its +0x44 ≠ 0), else to 0.
   This is not sim state.
6. **Auto-reveal spread**: only if `source` **and** mode +0x340 (auto-reveal):
   - every category-3 object whose owner (+0x58) ≠ the local player's id (+0x4C), with squared 3-D distance to
     this planet's position (+0x78/+0x7C/+0x80) **strictly less than** R², where R =
     `MaximumFleetMovementDistance` (GC +0x108; FoC 150). Distance is
     `(double)(dx·dx) + (double)(dy·dy) + (double)(dz·dz)` with float products, compared to
     `(double)R·(double)R` (0x397640, 0x2a9ff0);
   - every trade-route neighbour (PD +0x58 links, +0x60 count, neighbour at link +0x18);
   - each of them with a PD and a clear byte gets the step-4 treatment.

After the loop: if `newly` and frameArg > 0, play `SFXEvent_Planet_Revealed` once. This is AudioConstants +0x78
(0x140b301d8; base 0x140b30160 per AUDIO.md; FoC `GUI_Planet_Mouse_Over`), played non-positionally via 0x2d5290.
What frameArg is beyond "> 0 in normal frames" is [L].

Notes:
- The counter PD +0x2CC is **not saved** (absent from 0x4bbef0) [H]. After a load it restarts at 0 (ctor value).
- The ground list d+0xE0 plays no part here. Only orbiting fleets count [H].
- Auto-reveal re-runs the spread every frame for every source (idempotent) [H].

### 1.4 Campaign start 0x28dbe0 [H]
After the mode start (0x28e890) and per-player setup:
- home = 0x4af240(campaign, local faction) (`Home_Location`);
- the first object of that type (0x2a87d0);
- if it is a planet (+0x335 ≠ −1) with a PD, then +0x2C9 := 1 and a visibility refresh 0x3a51e0(home, local);
- camera / selection focus (0x3de570), and 0x140b28ee0 := home.

Every other owned planet is revealed by the first pre-step (§1.3 step 1).

### 1.5 Story rewards (0x456970 switch; names from the enum table at 0xa29440) [H]
| Reward (id) | Params | Behaviour |
|---|---|---|
| REVEAL_PLANET (0x33) | P1 planet type name, P2 int | both required (else no-op); for every instance (0x2a87d0) with +0x335 ≠ −1 and a PD: +0x2C9 := (atoi(P2) ≠ 0), refresh 0x3a51e0(planet, local), log |
| REVEAL_ALL_PLANETS (0x39) | none | every category-3 object with a PD: +0x2C9 := 1, refresh |
| ENABLE_GALACTIC_REVEAL (0x44) | P1 int | mode +0x340 (auto-reveal) := (atoi(P1) ≠ 0); missing P1 → logged error. Unused in shipped XML |
| SET_PLANET_VISIBILITY_LEVEL (0x51) | P1 planet, P2 faction, P3 level name or "none", P4 key, P5 optional seconds | modifies the visibility modifier list, not the byte (§2.3) |

### 1.6 `Hyperspace_Fleet_Reveal_Range` (type +0x64C) — a reader, not a writer [H mechanism, M details]
- Objects whose type has +0x64C > 0 are registered at creation (0x29f810 / 0x2b5ad0) in a GOM revealer list
  (+0x210).
- 0x433090 is a fleet behaviour's visibility vote for the local player. A moving fleet (+0xC0 set, not
  contained) owned by someone else, when the player's +0x368 ≠ 1, is visible when:
  - not hidden (0x3a9760 false), its owner equals the local player via 0x4329c0, and no content passes 0x375270;
  - or, failing that, a revealer of the local player (0x2aa200), or (when a corruption player 0x2948d0 exists)
    any planet with corruption type 5, lies within range of the fleet. The range test is squared, float,
    strict `<`. The range is `Bribery_Fleet_Reveal_Range` (GC +0x11F7C) for type-5 planets, else the
    revealer type's +0x64C.
  - The match also needs:
    - for a revealer whose owner has no AI system: the fleet's previous or next path planet must be revealed
      (+0x2C9);
    - otherwise: the revealer's planet must be revealed.
- FoC data: Bothawui 150, two heroes 75. This is presentation-visible state, not the reveal byte.

---

## 2. Visibility mask PD +0x1A8 and its modifier lists PD +0x1B8

### 2.1 Storage [H]
- PD +0x1B8: an array (count +0x1C0) of one `SimpleModifierListClass<tVisibilityLevelType>` per player,
  indexed by player list index (player +0x48). It is built in the PD ctor for every player.
- List header:
  - +0 head;
  - +8 count;
  - +0x10 min, +0x14 max (int);
  - +0x18 has-min, +0x19 has-max (both 0 at creation; no writer found [L]).
- Entry (0x40 bytes):
  - +0x10 value (uint mask);
  - +0x14 expiry frame (uint; 0xFFFFFFFF = never);
  - +0x18 name hash (0x215a30);
  - +0x20 key = {object id (+0x50), object serial (+0x298), sub-key, extra} (0x4ad4c0; match 0x4ad580 /
    0x4ad550).
- Fold (0x3fa490, 0x3fd080, 0x50c550; 0x50c550 is the read-only query used by the planet-info checks, see
  REMAINING_GAPS §6.3, corrected 2026-10-28): value = OR of all entry values in list order (`BitwiseOr` functor
  [M: its arguments are lost in the decompile]). Then, if has-min and value < min, value := min; if has-max and
  value > max, value := max (signed compares).
- Expiry 0x285e40(list): removes entries whose expiry < store frame (0x4c2030 = store +0x10) and returns
  whether any were removed.

### 2.2 Bits (`VisibilityLevelType`, enum/bitfield converter 0x510120) [H]
| Bit | Name |
|---|---|
| 0x1 | Planet_Ownership |
| 0x2 | Starbase_Level |
| 0x4 | Ground_Base_Level |
| 0x8 | Credit_Income |
| 0x10 | Fleet_Presence |
| 0x20 | Num_Fleets |
| 0x40 | Fleet_Contents |
| 0x80 | Num_Ground_Companies |
| 0x100 | Ground_Company_Contents |
| 0x200 | Special_Structures |
| 0x400 | Political_Control |
| 0x800 | Political_Control_Breakdown |
| 0x1000 | Object_Under_Production |
| 0x2000 | Enemy_Minor_Stealth_Heroes |
| 0x4000 | Enemy_Major_Stealth_Heroes |
| 0x8000 | Credit_Income_Breakdown |
| 0x10000 | Most_Powerful_Ship |
| 0x20000 | Force_Sensitive_Heroes |
| 0x40000 | Super_Weapons |

Named composites: Evil_Default 0, Good_Default 0x11, Planet_Owner 0x9FFF, Maximum 0xFFFFFFFF.
`Can_See_Visibility_Level` (type +0x1D74) is only copied (0x36c000) and is unused in shipped XML [H].

### 2.3 Contribution sources [H unless tagged]
| Source | Key (object, sub-key) | Value | Expiry | Players |
|---|---|---|---|---|
| **Baseline** (0x3fa490, planet behaviour vf20 +0xA0) | (planet, 0) | 0x9FFF if PD +0x74 (owner) == local player id; else the local faction default by name: "Rebel" 0x11, "Empire" 0, "Underworld" 0x11, other 0 | never | local only |
| **Presence** (0x3fd080, from vf21 0x3fd930) | (planet, 1) | starts at the player's faction default. 0x71 if any orbit-list object's control-record owner (0x3956c0 +4) is the player, then OR 0x191 if any ground-list (+0xE0) object is. If the planet's owner (vf +0x98 → +0xC) equals the player id, no entry is written and the signalled level is −1 (all) | never | players with an AI system (+0x360) only [H: 0x2814d0]; whether the local human has one is [M: no] |
| **System spy** (`SystemSpyAbilityClass`, apply 0x70f510, remove 0x70fb90) | (ability, …) via 0x48e1e0 | ability +0x18C mask (§2.3.1) | `Duration_In_Secs` > 0: now + (int)(FPS·d + 0.5); −1: never; 0: expires at once | spy owner. Add/remove and the refresh of every player (0x3a51e0) are [H]. The triggers are [M]: activation (0x70f4a0); arrival or ownership change (0x70f950), which applies to the target planet, or to every planet within the unit's `Galactic_Influence_Range` (type +0x1A00) when > 0; deactivation (0x70fb20) |
| **Story** SET_PLANET_VISIBILITY_LEVEL (0x461a60 → add 0x3feb90 / remove 0x3fdbc0) | (planet, hash(P4)) | P3 parsed by the VisibilityLevelType converter (`Maximum`, `Planet_Owner`, bit names…); P3 "none" (case-insensitive) removes the entry | P5 seconds, as for spies; default −1 (never) | the player of faction P2 (0x294d30 [M: first match]). Shipped: Empire Act I Fondor, Maximum then none |

The presence entry also sends `VisibilitySignalDataClass` signal 0xE (player, level) on the planet (AI
perception) [H]. A null slot in the orbit list aborts 0x3fd080 before the signal [H quirk].

#### 2.3.1 Spy mask (load-time 0x70fd20) [H]
Each `See_*` flag adds bits:

| Flag | Bits |
|---|---|
| Base_Levels | 0x6 |
| Credit_Income | 0x8 |
| Credit_Income_Breakdown | 0x8000 |
| Num_Fleets | 0x30 |
| Most_Powerful_Ship | 0x10000 |
| Fleet_Contents | 0x40 |
| Num_Ground_Companies | 0x80 |
| Ground_Company_Contents | 0x100 |
| Special_Structures | 0x200 |
| Political_Control | 0x400 |
| Political_Control_Breakdown | 0x800 |
| Current_Production | 0x1000 |
| Minor_Stealth_Heroes | 0x2000 |
| Major_Stealth_Heroes | 0x4000 |
| Force_Sensitive_Heroes | 0x20000 |
| Super_Weapons | 0x40000 |

- A spy never grants Planet_Ownership (bit 0).
- Dependent flags force their prerequisite on, with a load error logged:
  - Most_Powerful_Ship or Fleet_Contents → Num_Fleets;
  - Ground_Company_Contents → Num_Ground_Companies;
  - Political_Control_Breakdown → Political_Control;
  - Credit_Income_Breakdown → Credit_Income.
- Duration fixes:
  - `Galactic_Automatic` with duration ≥ 0 → −1;
  - `Ground_Activated` with duration ≤ 0 → 30;
  - any other style → style 0 (error).
- FoC data: Probe_Droid_System_Spy (Ground_Activated, 60 s) and Smuggler_Limited_Spy (Galactic_Automatic,
  Num_Fleets).

### 2.4 When the local mask is recomputed [H]
- **Refresh 0x3a51e0(object, player)**: for the local player, every behaviour votes (vf +0xA0; for planets
  this is 0x3fa490, which always returns 1). Then every behaviour gets vf +0xA8. For planets that is 0x3fd930:
  - local: PD +0x1A8 := 0, +0x98 := 0, +0x9C := −1, then 0x3fa490 again and 0x3fd560 (orbit-fleet display);
  - for players with an AI system: 0x3fd080;
  - PD +0x1A0 := 0.
- 0x3fa490 itself (local player only):
  1. upsert the baseline entry;
  2. expire, fold and store PD +0x1A8;
  3. if bit 0x400 dropped versus the previous mask, PD +0x98 := (int)control (+0x68); if bit 0x1 dropped,
     PD +0x9C := owner (+0x6C). These are the "last seen" snapshots. Through 0x3fd930 they are wiped at once,
     because 0x3fd930 zeroes the old mask first [M: net effect as read];
  4. set every trade route of this planet visible (0x5c3bb0(route, b)) where b = this planet's +0x2C9 and the
     neighbour's +0x2C9 are both set (a neighbour without a PD counts as set).
- Planet service 0x3fe1b0 (vf6), every frame: for each player whose list had expired entries (0x285e40),
  run vf +0xA8 for that player.
- Callers of 0x3a51e0 include every reveal (§1.3/1.4/1.5), spies, story visibility, launch influence
  (REMAINING_GAPS §6.1), first ownership initialisation (0x3fb040) and object visibility changes generally.

### 2.5 Save [H]
- PD +0x1A8: chunk 1 micro 21.
- Lists: chunk 6 per player, in index order. Each entry is a chunk 1 with micro 0 value, micro 1 expiry,
  micro 2 name hash, then the key subchunk (0x4ad6d0).
- The list min/max header is not saved.
- Mode +0x340 (auto-reveal): galactic mode save 0x4b2d60 chunk 1 micro 0.

---

## 3. Is reveal ever undone?
- **Revealed byte:** only by REVEAL_PLANET with Param2 0 (story); never in shipped data [H].
- **Auto-reveal:** ENABLE_GALACTIC_REVEAL can turn it off. Bytes already set stay set [H].
- **Visibility mask bits:** yes. Expiring spy and story entries, the baseline changing when ownership is lost,
  and presence entries reverting to the faction default all lower the mask on the next refresh [H].
- **Influence set PD +0xF0** (launch reveal, REMAINING_GAPS §6.1) is a separate mechanism. It does not touch
  +0x2C9 [H].

---

## 4. Corruption transition fraction 0x4b7e20

### 4.1 State (PD) [H]
| Offset | Field | Save (chunk 1 micro) |
|---|---|---|
| +0x2E2 | gaining (transition to corrupted in progress) | 81 |
| +0x2E3 | removing | 82 |
| +0x2E8 | start frame (uint) | 83 |
| +0x2EC | end frame (uint) | 84 |
| +0x2F0 | frame of the last completed change | 101 |
| +0x2F4 | current corruption type (−1 none, 0..7) | 103 |
| +0x2F8 | pending type (the type being gained; −1 while removing or idle) | 104 |
| +0x300 | corruption particle object (`Corruption_Particle_Name`, GC +0x11F30, resolved type +0x11F38) | 87 |
| +0x308/+0x30C | brightness start/end | 88/89 |
| +0x310/+0x314 | scale start/end | 90/(91) |
| +0x318/+0x31C | energy start/end | 92/93 |

Ctor 0x4b5700: flags 0, start = end = 0, +0x2F4 = +0x2F8 = −1, all six pair values 1.0 [H].

### 4.2 Formula [H, disassembly 0x4b7e20]
```
fraction(pd, nowArg):
  end = u32 pd+0x2EC ; start = u32 pd+0x2E8
  if end == 0 or start == end:  return 1.0
  if start > end (unsigned):    return 0.0
  now = nowArg != 0 ? nowArg : (store ? u32 store+0x10 : 0)      // store = 0x140b15418
  t = float(u32(now - start)) / float(u32(end - start))            // u32 zero-extended, cvtsi2ss 64-bit: exact unsigned
  return min(1.0f, t)                                              // t ≥ 0 always; now < start wraps → 1.0
```
Callers pass nowArg = 0 (store frame), except the particle lerps, which pass the planet service's frame argument
(0x3fe1b0 param 3, kept in r12d) [H]. The two are the same galactic frame in normal play [M].

Helpers [H]:
- 0x4b7ed0 / 0x4b7f50 / 0x4b7f10: `a + (b − a) × fraction` on the pairs +0x308/+0x30C, +0x310/+0x314 and
  +0x318/+0x31C;
- 0x4b7e90 "effective type": +0x2F8 while either flag is set, else +0x2F4.

### 4.3 Begin, complete, cancel [H]
| Function | Guard | Effect |
|---|---|---|
| Begin gain 0x4b72e0(pd, secs, type) | no flag set and +0x2F4 == −1 | +0x2E2 := 1; +0x2F8 := type; start := store frame; end := start + (int)((float)FPS × secs). secs = 0 → end = start (fraction 1 at once) |
| Begin removal 0x4b7260(pd, secs) | no flag set and +0x2F4 ≠ −1 | +0x2E3 := 1; +0x2F8 := −1; secs = 0 → 0.1; start/end as above |
| Complete 0x4b7a50(pd) | exactly one flag set | +0x2F4 := +0x2F8; start = end = 0; flags := 0; +0x2F8 := −1; +0x2F0 := store frame |
| Cancel 0x4b6c50(pd) | none | flags := 0; start = end = 0; +0x2F8 := −1 (+0x2F4 unchanged) |

`(int)` is truncation (cvttss2si). FPS is the int at 0x140b0a340.

Callers and durations:

| Caller | Direction | Duration |
|---|---|---|
| Ownership change 0x3fb040: the new owner's faction has `Benefits_From_Corruption` and the old one's does not; not gaining; after Cancel, +0x2F4 == −1 | gain to type **3** | **10.0 s** (const 0x140805898) |
| Ownership change 0x3fb040: the new owner (or no owner) lacks the flag and the old one had it; after Cancel, +0x2F4 ≠ −1 | removal | 10.0 s |
| First ownership initialisation of a planet whose owner benefits | instant +0x2F4 := 3, no transition, then 0x3ff230 | — |
| `Remove_Corruption_Ability` vf30 0x5b13c0 (cost: planet type `Planet_Corruption_Removal_Base_Cost` +0x804 × ability `Corruption_Removal_Cost_Multiplier` +0x17C must not exceed the player's credits) | removal | ability `Corruption_Removal_Time_In_Secs` (+0x180, read as **float**; FoC 3–20) |
| Corruption action, direct path 0x52bf50 (L1_S4 §4.5) | gain | 5.0 s |
| Corruption mission success, battle end 0x28bf10 | gain | GMM +0x1FC (5.0) |
| `Corrupt_Planet_Ability` vf30 0x6f6120 | gain | ability +0x17C |
| Death Star hack effect 0x61b790 / 0x61b9f0 | removal | 5.0 s |
| Story REMOVE_CORRUPTION (0x7D) | removal | P2 seconds (default 10.0) |
| Planet reset 0x3f76c0 [M: destroy / clear] | Cancel, +0x2F4 := −1 at once, 0x3ff230 | — |

The wrappers 0x3f6810 (gain) and 0x3f6770 (removal) also:
- call 0x3ff230 (rebuild visuals and pairs, §4.5);
- raise the story/script notification "increase_corruption" / "decrease_corruption" (0x32edd0).

The ownership-change path calls 0x4b72e0 / 0x4b7260 **directly**, without 0x3ff230 [H: only one 0x3ff230 call
in 0x3fb040, on the instant path]. Its pairs and particle object therefore stay as they were until completion
(see §4.6).

### 4.4 Completion (planet service) [H]
0x3fe1b0 → 0x3fc5b0(beh, planet, pd, frameArg), which returns early when no flag is set or when
frameArg < end (unsigned). Otherwise it:
1. completes (0x4b7a50);
2. rebuilds (0x3ff230);
3. sends a corruption-changed notification (0x326e50, new type or −1);
4. plays the local SFX: faction +0x730 (removed) or +0x720 (gained) of the planet owner or the corruption player
   (0x2948d0), when that player is local;
5. runs 0x3f9690.

This is the only sim-relevant use of the transition: integer frames. The fraction itself is presentation only.

### 4.5 Pairs and their tags [H]
0x3ff230, when the particle object +0x300 exists, writes:
- start = f(current type +0x2F4);
- end = f(effective type 0x4b7e90).

The accessors read GameConstants lists, at data pointer = list + 8. The list objects are at GC +0x11EE8,
+0x11F00 and +0x11F18:

| Accessor | List (GC offset) | Tag | PD pair | Particle scalar (0x173220 index) |
|---|---|---|---|---|
| 0x2c7c90 | +0x11EE8 | `Particle_Brightness_Per_Corruption_Level` (FoC 0.3, 1.0) | +0x308/+0x30C | 4 = colour; R = G = B = A = value |
| 0x2c7cf0 | +0x11F00 | `Particle_Scale_Per_Corruption_Level` (1.0, 1.0) | +0x310/+0x314 | 1 = emission scale |
| 0x2c7cc0 | +0x11F18 | `Particle_Energy_Per_Corruption_Level` (1.0, 1.0) | +0x318/+0x31C | 0 = detail / energy |

- Accessor rule: type −1 → element [0]; type 0..7 → element [1]; any other value → 1.0. The list length is not
  checked [H].
- The particle object +0x300 exists while corrupted or gaining. It is destroyed (0x391310) when idle and
  uncorrupted.
- Every planet service frame, 0x3fe1b0 sets the three scalars with smoothing 0 (snap) on the particle systems of
  +0x300's render object (+0x2A0, system id 0x140a124d8) [H].

### 4.6 Behaviour over a transition (FoC values) [H formula, M visuals]
- **Gain via a wrapper** (ability, action, mission): pairs = (0.3 → 1.0) brightness, others 1.0. The particle
  object is created at begin. Brightness ramps 0.3 → 1.0 linearly over the duration. At completion the pairs
  become (1.0, 1.0) and the fraction is 1.
- **Removal via a wrapper**: pairs = (1.0 → 0.3). Brightness ramps down. At completion +0x2F4 := −1 and the
  particle object is destroyed.
- **Ownership-change transitions** (10 s): pairs are not rebuilt at begin.
  - Gain from clean: there is no particle object until completion, so no ramp; particles appear at full
    brightness when it completes.
  - Removal: brightness stays at 1.0 until completion, then the particles vanish.
- **Path/line tint** (0x3fe1b0, all transitions): factor f, applied to `Corruption_Path_Color` (GC +0x11F5C;
  channels = (int)((float)(byte)(c·255) × f), alpha 255) and set on each corruption-line object in PD +0x338.
  - Idle: f = 1.
  - Gaining with +0x2F4 == −1: f = fraction(store frame).
  - Removing with +0x2F4 ≠ −1: f = 1 − fraction.
- **UI** 0x3ec380 shows (int)(fraction × 100) with TEXT_CORRUPTION_PERCENT or TEXT_CORRUPTION_REMOVE_PERCENT
  while a flag is set.
- **After load**: 0x3f8100 (planet behaviour vf11) calls 0x3ff230, which rebuilds the pairs from the saved
  types [M: vf11 = post-load].

---

## Port implementation notes

> **Status 2026-10-30:** The accessors named in items 4, 6, 8 and 10 exist as `GalacticSim` members (`engine/galactic/include/alamo/galactic/campaign.hpp:425-499`; code in `engine/galactic/src/reveal.cpp` and `visibility.cpp`). The local player is the first human player pinned at start (`campaign.hpp:425`), and the visibility baseline, presence and auto-spy parts are recomputed per query instead of stored (`campaign.hpp:455-457`).

Everything below is sim-side and deterministic. Presentation reads the sim state and never writes it.

**Reveal (engine/galactic)**
1. Per planet, store:
   - `revealed` (u8, saved);
   - `reveal_delay_counter` (i32, **not** saved, 0 on load);
   - `hidden` (existing +0x2E0).

   Per galactic mode, store `planet_auto_reveal` (u8, saved, from campaign `Planet_Auto_Reveal`, default true).
2. Run the pre-step (§1.3) first in the galactic frame, before planet services:
   - category-3 list order;
   - threshold = `(float)fps * Planet_Reveal_Delay_Time`, computed in float with no FMA;
   - compare `(float)counter <= threshold`;
   - spread distance in double built from float squares, strict `<` against `(double)R*(double)R`.
3. The revealed state belongs to "the local player" in the original. For single-player/replays, pin it to the
   human player id chosen at campaign start, not to whatever player the viewer is. Multiplayer always answers
   revealed (and stays deferred).
4. Accessors to expose:
   - `bool is_planet_revealed(PlanetId)` — 0x3fa420 semantics: MP → true; no PD or hidden → false;
     auto-reveal → true; else the byte;
   - `bool planet_revealed_raw(PlanetId)` — the byte: hop gate, trade routes, fleet-transit test;
   - `void set_planet_revealed(PlanetId, bool)` — story, start;
   - `bool planet_auto_reveal()` / `set_planet_auto_reveal(bool)`;
   - `bool consume_reveal_sfx()` — or emit an event when a pre-step revealed something and frameArg > 0.
5. Campaign start: reveal the human's `Home_Location` planet after player setup.

**Visibility masks**
6. Implement per (planet, player-index) an ordered modifier list of {value u32, expiry u32 (0xFFFFFFFF = never),
   name hash u32, key}. Keys are stable ids: (object id, serial, sub-key). Never use pointers or iterate hash
   maps.
   - Fold = OR in list order. Expiry check against the store frame.
   - Accessors: `uint32 visibility_mask(PlanetId, PlayerIdx)`, `add_visibility_modifier(planet, player, key,
     value, seconds)` (seconds > 0 → frame + (int)(fps·s + 0.5); −1 → never; 0 → expires next check),
     `remove_visibility_modifier(planet, player, key)`.
   - Keep the cached local mask (+0x1A8) only if the UI needs it.
7. Contribution rules as §2.3:
   - baseline 0x9FFF for owner, else by faction name;
   - the presence entry only for AI-controlled players;
   - spy mask from the `See_*` flags with the load-time fix-ups;
   - story SET_PLANET_VISIBILITY_LEVEL.
8. Trade-route visibility = both endpoints' raw revealed bytes. Refresh it when either changes.

**Corruption transition**
9. Per planet: `gaining`, `removing` (u8), `start_frame` / `end_frame` (u32), `pending_type` (i32),
   `last_change_frame` (u32); all saved. Implement begin/complete/cancel exactly as §4.3. Durations are
   `(int)((float)fps * secs)` with truncation, and removal secs 0 → 0.1. Completion: in the planet service,
   `frame >= end_frame` (unsigned).
10. Expose:
    - `float corruption_transition_fraction(PlanetId, uint32 now = 0)` (§4.2, unsigned wrap behaviour
      included);
    - `int effective_corruption_type(PlanetId)`;
    - `CorruptionParticlePairs pairs(PlanetId)` — or compute start/end from the GameConstants lists using
      current/effective type, but only at the points the original does (§4.3/4.5) to keep the
      ownership-change quirk.

    Game logic must not branch on the float fraction. It feeds particles (scalars 4/1/0, snap), the path tint
    and the UI percent only.
11. GameConstants loader: parse the three `Particle_*_Per_Corruption_Level` lists. Index [0] = uncorrupted,
    [1] = corrupted. Out-of-range type → 1.0. A missing element should also read 1.0 (the original does not
    bounds-check) [L].

---

## Open items
1. frameArg of the galactic update 0x4b2ef0 (gates the reveal SFX), and the planet service's frame argument
   versus store +0x10 (assumed equal) [L/M].
2. Whether the local human player ever has an AI system (+0x360). If so, the presence entry (0x71 / 0x191) also
   feeds the human's mask [M: no].
3. Writers of the modifier-list min/max flags (+0x18/+0x19): none found [L]. Treat as unused.
   (2026-10-28: the save loader 0x4ba9d0 / 0x4b8a40 recreates each list with both flags 0 and restores entries
   only; the three folds 0x3fa490 / 0x3fd080 / 0x50c550 only read them [H]. No other writer found [L].)
4. The "last seen" snapshots PD +0x98/+0x9C appear to be wiped by 0x3fd930 straight after 0x3fa490 sets them.
   Confirm the vf order in 0x3a51e0 against the UI that reads them [M].
   (2026-10-28: the only writers found are 0x3fa490 (set on a dropped 0x400 / 0x1 bit) and 0x3fd930 (reset to
   0 / −1, then calls vf +0xA0 = 0x3fa490 with the old mask already 0, so it never snapshots) [H]. Both are
   reached only through vtables. Net effect still [M].)
5. 0x2648b0's +0xC4 display state and 0x140b2eea0 (+0x44) — presentation only [L].
6. Whether +0x2C9, +0x2CC or the modifier lists enter the per-frame CRC (OBJECT_RUNTIME §5.1 lists the store,
   RNG and players only) [L]. The port should include `revealed` and the transition fields in its CRC anyway.
7. Writers of the hidden byte PD +0x2E0 / +0x2E1 (setter 0x3fe730; caller not traced, possibly the story
   SET_PLANET_RESTRICTED reward [L]).
8. Hyperspace fleet visibility 0x433090: meaning of player +0x368 == 1, 0x4329c0 and 0x375270, and why
   corruption type 0 planets are skipped as revealers [M].

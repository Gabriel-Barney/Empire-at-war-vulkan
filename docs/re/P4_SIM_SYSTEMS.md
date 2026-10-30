# FoC galactic special systems: black market, sabotage, super-weapon hack, income abilities, corruption requirements (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Scope: the simulation behind the P4 GUI panels already in `engine/ui` (black market, sabotage, hack, corruption
choice, economy breakdown). Written for the `engine/galactic` code agent, who cannot see the decompile. Addresses
are citations only. Confidence: [H] read directly, [M] inferred from strong evidence, [L] guess or unread.

Reused facts (not restated): command wire format and quantizers (COMMANDS §1.2, §3.2); synced RNG = LCG at
0x140a13e24, `Get_Float(lo,hi)` 0x532480, `Get_Float01` 0x5324d0, `Get_Int(lo,hi)` 0x1ffb40 (AI_GALACTIC.md
line 24; AI_GALACTIC_2 §1.4: "0x1ffb40 draws nothing when lo == hi"); special-ability framework, `Activation_Style`
enum and the unit-ability black-market gate (ABILITIES_HARDPOINTS §2.4, §3.1); income breakdown list, siphon,
halt list and the black-market income reroll (GALACTIC_ECONOMY §1.2–§1.4); corruption tactical missions
(L1_S4_REMAINING §4.5); story event ids (STORY.md table at line 41); AI goal/plan tables (AI_GALACTIC §2.x,
AI_GALACTIC_TOKENS rows `BlackMarket*`).

Notation: P = player, D = planet data pack (planet object +0xB8), T(x) = object x's type (+0x298), GC = GameConstants
(base 0x140b15800 [H]: +0x11f0 → 0x140b169f0, +0x1008 → 0x140b16808, +0x12140 → 0x140b27940 all match the
reads below). "Local error SFX" = the SFX at 0x140b301c8 played only for the local player. FPS = 30.

---

## 0. Summary for the implementer

1. All four hero actions are **Ground_Activated special abilities** (style 6) fired by one trigger, and all four
   human follow-ups are **one command, `SpecialAbility` (event 13), with a 3-bit kind**: 1 activate, 4 black-market
   purchase, 5 sabotage a chosen structure, 6 corruption choice, 7 hack (kind 2 slice and 3 are other abilities).
   For a **human**, the ability's apply opens a dialog and returns "not applied"; the dialog then issues the
   kind-4/5/6/7 command whose executor does the work and the hero's departure. For an **AI** player, apply does
   the work at once (random pick on the synced RNG). For a **remote human** apply does nothing. [H]
2. **Thrawn's Revenge disables all of it except the income abilities.** TR 3.3 ships an empty
   `BlackMarketItems.xml` (one dummy entry), empty `Activated_{Black_Market,Sabotage,Corrupt_Planet,
   Hack_Super_Weapon}_Ability_Names`, an empty `Hack_Super_Weapon_Required_Type`, corruption income 0 for every
   type, no planet `Corruption_N_Type`, and no unit with these four ability classes. It uses
   `Planet_Income_Bonus_Ability` (~100 entries: admin/agent heroes, mining, stations, negative "Maintenance_Cost"
   upkeep) and one `Planet_Income_Gambling_Ability`. **Item 4 (§4) is the parity-critical part**; §1–§3 and §5 are
   needed for vanilla FoC only. TR also sets `Default_Hero_Respawn_Time` = −360, which disables every
   default-time hero respawn (§6.2). [H]
3. **Mining / income display**: the economy panel groups the planet income-breakdown entries by the CRC of
   their source text. "Mining facility income" is the sum of `Planet_Income_Bonus_Ability` entries whose
   `Specific_Mod_Source_Text` is `TEXT_ECONOMY_MINING_FACILITIES` (TR: its own text ids). The host shows 0
   because no income-bonus ability writes entries yet. [H]

> **Status 2026-10-30:** The mining column is filled: `engine/apps/game/app/game_info.cpp:249-312` sums each planet's income entries tagged `TEXT_ECONOMY_MINING_FACILITIES` and takes the total from `GalacticSim::mining_income`, and income-bonus and gambling entries are written by `engine/galactic/src/special_systems.cpp:985-1027`.

4. Determinism: every random choice uses the synced LCG; all lists are walked in their stored order (item table
   file order, planet structure lists, player index order, GOM type lists). The black-market AI pick and the
   corruption AI pick are weighted picks (cumulative weights, one `Get_Float` draw, binary search). [H]

---

## 1. Black market

### 1.1 Database: `BlackMarketItems.xml` [H]
- Loaded at boot by 0x48f5b0 from `Data\XML\BlackMarketItems.xml` (MAIN_LOOP §5.1 database list); each
  child element of the root becomes one `BlackMarketItemClass` (0x120 bytes, ctor 0x5afc00, parser
  `BlackMarketItem::Parse_Database_Entry` 0x5b0170), appended to the global table at 0x140a2aa78 (count
  0x140a2aa88) **in file order**. The child element name is not checked (vanilla `Item`, TR `BlackMarketItem`).
- `Name` attribute → +0x08 (case kept), **name CRC-32** (0x215a30 over the bytes as written) → +0xB8. This CRC
  identifies the item in commands and saves.
- Fields (offsets from xml_field_tables "Item" segment; defaults from the ctor):

| tag | off | type | default | use |
|---|---|---|---|---|
| `Uncommon_Price_Multiplier` | +0x28 | float | 0.0 | price × this when the owning faction lacks the tech (§1.3) |
| `Faction` | +0x30 (ref list), resolved faction ptr +0x50 | faction | none | items are offered only on planets owned by this faction |
| `Tech_Level` | +0x58 | int | 1 | tech of the item (owner-faction comparison) |
| `Ability_Names` | +0x60 (strings) → CRC list +0x78 (data +0x80, count +0x88) | list | empty | abilities this purchase unlocks (§1.5) |
| `Base_Cost_Credits` | +0x90 | int | 0 | |
| `Icon_Name` | +0x98 | string | | UI |
| `Localized_Name` / `_Description` | +0xC0 / +0xE0 | text id | | UI |
| `Used_By_Units` | +0x100 | list | | UI only (no sim reader found) [M] |
| `Tutorial_Tech_Level` / `Tutorial_Base_Cost_Credits` | +0x118 / +0x11C | int | not set by the ctor [L] (treat as 0) | used instead of Tech_Level / Base_Cost when the buyer's tutorial flag is set |

- Vanilla FoC: 10 items, 5 Empire + 5 Rebel, tech 1–5, cost 1000–4800, multiplier 5.5. TR: none.
- After parsing, each `Ability_Names` string is CRC'd (no upper-casing) into +0x78.

### 1.2 The hero ability `Black_Market_Ability` (BlackMarketAbilityClass, vtable 0x8ad158) [H]
Fields: `Min_Respawn_Times` +0x178 / `Max_Respawn_Times` +0x190 (5 floats each, indexed by tech level 1..5),
`Price_Modifier` +0x1A8 (default 1.0), `Tech_Level_Adjustment` +0x1AC (parsed; **no reader found** [M]).
Post-load (0x61b0e0): min ≤ max per entry else both vectors are cleared; `Activation_Style` is forced to 6
(Ground_Activated); `Causes_Despawn` forced Yes. Vanilla users: Tyber Zann (90 s, ×1.0), Urai Fen (30 s, ×1.5),
Silri (60 s, ×0.75).

**Accept** (vf5 0x61aad0) — the ability can fire at planet X only when all hold:
- X is a planet object (+0x335 ≠ −1) and the hero is not allied with X's owner (0x48e070 → 0x39a230 [M meaning]);
- X's owner faction `Is_Playable` (faction +0x20A8);
- **X's corruption type is 6 (`Corruption_Black_Market`)** (D +0x2F4; 0x397450).

**Apply** (vf30 0x61a4a0), hero H, owner P, planet X:
1. Faction pairing 0x48d900, by faction **name**: P Rebel → X owner Empire or Underworld; P Empire → Rebel or
   Underworld; P Underworld → Rebel or Empire; and X owner ≠ P. Failure → local error SFX, return false.
2. Build the offer list (§1.3) over the whole item table.
3. Respawn time R: t = tech(P) (0x2813c0: P +0x84, plus faction `Displayed_Tech_Level_Adjustment` +0x2FB0 unless
   GMM 0x28afb0 [skirmish]); if 1 ≤ t ≤ 5 and the min vector is non-empty, **R = Get_Float(min[t−1], max[t−1])
   (one synced draw, on every machine)**; else R = 0 (→ default respawn, §6.2).
4. P is the local player and human (P +0x62): open the black-market dialog (0x3c6680; stores offers, R, buyer id
   H +0x50, planet) and **return false** (ability not consumed). The purchase arrives as command kind 4 (§1.4).
5. P is AI-controlled (P +0x360 ≠ 0) and not human: candidates = offers with price ≤ P credits, item ≠ null,
   price > 0, in offer order (**already-purchased items are not excluded** [H]). Cumulative weight = price. One
   `Get_Float(0, total)` draw [M: decompiler drops the args; same pattern as Weighted_Random_Pick], first
   cumulative ≥ r. Then run the purchase steps of §1.4 with that offer and R; return true.
6. Otherwise (remote human): return false.

### 1.3 Offers and price (0x61ad20) [H]
For buyer P (tutorial flag tut = P +0x448, Lua `Set_Black_Market_Tutorial`), planet X, ability A, each item I in
table order:
- Skip unless I.faction is null, or X's owner/faction is null, or X owner faction id (+0x20) == I.faction id.
- base = tut ? I.Tutorial_Base_Cost : I.Base_Cost (int); tech_I = tut ? I.Tutorial_Tech_Level : I.Tech_Level.
- If not tut: price = (float(base) + `Planet_Black_Market_Modifier`(T(X) +0x800, read as float)) × A.Price_Modifier
  × m(P), where m(P) = 0x281960: 1.0 when P's timed-modifier list (+0x270/+0x278) is empty, else (fold of the
  live entries, clamped by optional min +0x280 / max +0x284) + 1.0, floored at 0.0001 [M: writer of the list
  unknown, vanilla 1.0]. If tut: price = float(base).
- Uncommon factor u = 1.0. If I.faction is set:
  - single player, or P not human (GMM 0x28af30 false [M: "is multiplayer"]): walk players in index order; at the
    **first** player whose faction id == I.faction id and tech(p) < tech_I, u = I.Uncommon_Price_Multiplier;
  - multiplayer human buyer: m = max tech over all players except P; u = Uncommon when m < tech_I.
- price = max(0, price × u). Entry = {item, price, purchased = P owns I}.
- Offer order = item table order. GUI shows all offers including purchased ones (UI state).

### 1.4 Purchase: command kind 4 and its executor [H]
- **Event 13 `SpecialAbility`, kind 4** (setter 0x4b5350, pack 0x4b4990, unpack 0x4b4470, execute 0x4b4d70):
  payload after the 3-bit kind = id32 planet (+0x13C), id32 buyer hero (+0x140), 32-bit item name CRC (+0x58,
  resolved by 0x48f550 on the table), q8 price (+0x60), q5 respawn seconds R (+0x144), 1 bit "only one offer"
  (+0x150, not read by the executor). The executor re-quantizes price and R (COMMANDS §3.2 local round-trip), so
  the price charged is the quantized one.
- Execute guard (all kinds): skipped when a galactic battle is pending (0x140b1540c > 0) or a pending-conflict
  condition holds (0x4b4d70 prologue; same rule as FleetManagement, COMMANDS §2.3). Planet must exist and be a
  planet.
- **Executor 0x61ab60**: buyer must exist (else log "Cannot complete black market action because the buyer no
  longer exists!", no effect). If price ≤ P credits: Add_Credits(P, −price); add item to P's **purchased list**
  (P +0x430, data +0x438, count +0x440; append if absent); for the local player play faction
  `SFXEvent_New_Construction_Options_Available` (+0x670) and `SFXEvent_Slice_Success` (+0x610); hero departure =
  schedule respawn (§6.2) at X with time R, then despawn H and its team (0x48cf20). If price > credits: nothing
  happens (no message). Already-purchased items are charged again (list add is idempotent).
- **Story event BUY_BLACK_MARKET (0x38)** is dispatched by the **dialog** (0x3caf30) at click time with the item
  `Name` string as parameter — local machine only, before the command executes, and not for AI purchases. [H]
- Story reward **`GIVE_BLACK_MARKET` (reward 0x7E)** adds the item whose Name equals Param1 (exact length +
  memcmp) to the plot's player, free, no departure. [H]

### 1.5 What a purchase unlocks [H]
- Purchased state is only the per-player item list. Nothing else changes (no unit unlock, no tech change).
- **Unit abilities**: 0x39bbd0 (ABILITIES §2.4 "black market gate") and the same test inside `Has_Ability`-style
  check 0x39b310: in campaign (0x140a2864c set) and not skirmish (0x28afb0 false), slot t's `Unit_Ability`
  record with `Must_Be_Bought_On_Black_Market` (+0x220) is **blocked only if some item lists the record's ability
  name CRC (+0x88) and P has not bought it**. An ability marked but listed by no item is allowed (this is the TR
  case: 2 TR abilities are marked, the item table is empty). Team containers use their first member's slot.
  Callers: GUI, Lua `Activate_Ability` (0x570a50), `Is_Ability_Ready` (0x57b580).
- **Special abilities**: 0x39b0b0, same rule on `Must_Be_Bought_On_Black_Market` at special-ability +0x168 and the
  ability name CRC +0x38. It is part of the generic trigger (0x37b580, §6.1), so gated special abilities never fire.

### 1.6 State, save, CRC
- Per player: purchased item list (ordered), tutorial flags +0x448 (BM) and +0x449 (sabotage).
- **Save** (PlayerClass::Save 0x286760): chunk 0x15 holds micro 0x3C = count, then one micro 0x3D per item =
  item name CRC (+0xB8), list order; micro 0x43 = +0x448, 0x44 = +0x449. [H]
- CRC: the frame CRC's player term (COMMANDS §7.1 item 3) is not recoverable from the decompile [L]; single
  player never computes the CRC. Port: include the purchased set in the per-tick CRC player term.

### 1.7 AI
Goal `Black_Market_Purchase` (AI_GALACTIC goal table: Hero category, Enemy, TL 0) with tokens
`BlackMarketAbilitiesAvailable / MinimumAbilityPrice / PriceModifier` (AI_GALACTIC_TOKENS rows; the "used"
test there is the same purchased list). Plan `AI_Plan_Underworld_BlackMarket.lua`: task force
`Silri_Team | Tyber_Zann_Team | Urai_Fen_Team = 1`; `Produce_Force(Target)`, `Move_To(Target)`,
`Activate_Ability()` (direct, GalacticTaskForce vf39 0x6cd180 → per member 0x3a00b0 → §6.1), then
`Set_Plan_Result(true)`. **Correction to AI_GALACTIC §2.7:** BlackMarket also produces the force first.

---

## 2. Sabotage (`Galactic_Sabotage_Ability`, GalacticSabotageAbilityClass vtable 0x8a3fb8)

### 2.1 Data [H]
| tag | off | default | notes |
|---|---|---|---|
| `Can_Reduce_Ground_Base_Level` | +0x178 | false | option 0 |
| `Can_Reduce_Starbase_Level` | +0x179 | false | option 1 |
| `Can_Cancel_Production` | +0x17A | false | option 2 |
| `Can_Destroy_Special_Structure` | +0x17B | false | option 3 (vanilla saboteur: **only this one**) |
| `Can_Halt_Credit_Production` | +0x17C | false | option 4 |
| `Duration_Of_Credit_Halt` | +0x180 | 0 | forced to 10.0 when halting is on and ≤ 0; −1 = permanent |
| `Sabotage_Cost_Increase_Factor` | +0x18C | 2.0 | vanilla 2.0 |
xml_field_tables lists these under a "Combat_Bonus_Ability" segment heading; that label is a locality error.
Post-load 0x5b40f0 forces Ground_Activated and `Causes_Despawn` Yes. Structure data: `Sabotage_Cost_Credits`
T +0x870 (vanilla 500–10000 on special structures), `Tutorial_Sabotage_Cost_Credits` T +0x874. Planet data:
`Planet_Vigilance` T +0xC0. Vanilla user: `Underworld_Saboteur` (generic hero). GC `Activated_Sabotage_Ability_
Names` (`Saboteur_Galactic_Sabotage`, plus EaW names absent in FoC data), `Sabotage_Particle_Effect`
(+0x12050, `Planet_Explosion_Galactic`).

### 2.2 Options at planet X (0x5b1e60), in this order [H]
0. ground base: flag 0 and D +0x130 (ground base level) > 0;
1. starbase: flag 1 and D +0x148 (starbase level) > 0;
2. production: flag 2 and the production behaviour has a head entry in queue 0 or 1 (0x401960);
3. special structure: flag 3 and **X is corrupted** (D +0x2F4 ≠ −1) and X has ≥ 1 structure (0x3f8ac0);
4. credit halt: flag 4 and the faction pairing of §1.2 step 1 (0x48d900).
Accept (vf5 0x5b20c0) = planet, not allied (0x48e070) and the option list is non-empty.
Because the saboteur's `Corruption_Ability` needs an **uncorrupted** planet (§5.1) and option 3 needs a
**corrupted** one, the same activation corrupts a clean planet and sabotages a corrupted one (the AI plan relies on
this, see §2.6).

### 2.3 Apply (vf30 0x5b1920), saboteur S, owner P, planet X [H]
1. Accept; P must be the local player, or AI-controlled and not human; else return false.
2. Options empty → local error SFX (local P), return false.
3. k = options[Get_Int(0, n−1)] (**0x1ffb40; no draw when n = 1**, the vanilla case).
4. **Vigilance**: if T(X).`Planet_Vigilance` and T(S).`Is_Generic_Hero` (+0x14): fail. Text
   `TEXT_SABOTAGE_FAILURE_VIGILANCE` to S's owner and `TEXT_SABOTAGE_PREVENTED_VIGILANCE` to X's owner (local only,
   via the message callback 0x140b313c0); r = ability vf12 = `Get_Float(Min_Respawn_Time, Max_Respawn_Time)`
   (special-ability base +0x54/+0x58, **one synced draw**); if r > 0 schedule respawn at X; kill S (0x391310).
   Return true.
5. Dispatch k:
   - **0 ground base** (0x5b2910): cancel queued base upgrades (0x5b1d40); replace the base with
     `Prev_Level_Base` (type +0x8B0) at its level (0x4bb1a0), or level 0 (destroyed) when there is none.
   - **1 starbase** (0x5b3b60): same via 0x3fe750; also sets planet +0x330 = P id [M: last attacker].
   - **2 production** (0x5b2f50): cancel the head entry of queue 1, else queue 0 (0x403900).
   - **3 special structure** (0x5b32c0): see §2.4.
   - **4 halt** (0x5b2450): put/refresh an entry in D's credit-halt list (+0x288; GALACTIC_ECONOMY §1.4 step 1) keyed
     by (ability name CRC, S): active = 1, expiry = now + int(FPS × duration + 0.5) (−1 when duration = −1),
     tag = CRC(T(S) name).
   Messages ("TEMP TEXT: … has destroyed/reduced …") go to S's owner or X's owner if local. UI only.
6. Option function false → return false. True → story events LAND_ON (2) and DEPLOY (0xC) for S at X
   (0x3284c0), effect registration and presentation (0x48cca0, 0x48e4d0), return true. The departure is done by
   the generic trigger (§6.1: Causes_Despawn → vf12 draw + respawn + despawn).

### 2.4 Destroying a special structure [H]
Candidate lists: D +0x158 (ground special structures, count +0x160) then D +0x170 (orbital, count +0x178).
**Cost(structure s)** (0x5b1cc0) = round_half_away(`Sabotage_Cost_Credits`(s) × powf(Increase_Factor, n)),
or −1 when the cost is ≤ 0, where n is the planet's sabotage counter D +0x350 [M: the exponent argument is lost in
the decompile; D +0x350 is the only counter incremented per sabotage]. Tutorial (P +0x449) GUI cost =
`Tutorial_Sabotage_Cost_Credits`, unscaled.
- **Human (local)**: 0x5b32c0 opens the sabotage dialog (0x3c82f0; rows = structures of both lists with cost ≥ 1
  (0x3e6ff0), cost computed with the saboteur's first ability from `Activated_Sabotage_Ability_Names`) and returns
  false. The choice is **command kind 5** (setter 0x4b53b0): id32 planet (+0x13C), id32 saboteur (+0x140),
  id32 structure (+0x148), q8 cost (+0x14C, the dialog's number), q5 respawn seconds (+0x144, the dialog passes
  0). Executor `0x5b21a0(structure, cost, R, saboteur id, planet, story = 0)`: saboteur must exist and cost ≤ P
  credits; find the structure in D +0x158 (destroy 0x4bb420, then re-check ground production against D +0x130)
  or D +0x170 (0x4bb470, starbase production vs D +0x148); if destroyed: **story event GALACTIC_SABOTAGE (0x39)**
  with the structure type name (0x327c30); Add_Credits(P, −cost); if R ≥ 0 schedule respawn (§6.2; R = 0 →
  `Default_Hero_Respawn_Time`); despawn S; spawn `Sabotage_Particle_Effect` at X; faction `SFXEvent_Sabotage_
  Success` (+0x760) for P; D +0x350 += 1.
- **AI**: candidates = live structures (not +0x3A0 & 0x42) of both lists in list order with cost ≤ P credits
  (cost −1 counts as affordable), one `Get_Int(0, n−1)`, destroy as above (no story event, no departure here —
  §6.1 does it), effect + SFX only when D +0x2C9 [L meaning], D +0x350 += 1, and **P +0x7C −= cost**. P +0x7C is
  the per-tick stat accumulator (MAIN_LOOP §3.3 step 7), not credits: **the AI is not charged** [H as read].
- Story reward **SABOTAGE_STRUCTURE (0x81)** (Param1 planet, Param2 structure type): first structure of that type
  in D +0x158 → 0x5b21a0 with story = 1 (no cost, no saboteur, still event 0x39, effect, counter). [H]

### 2.5 Cooldown, timing
No timer on the ability itself: the saboteur leaves (despawn) and comes back after the respawn delay (§6.2,
vanilla 360 s). The only escalating cost is the per-planet counter D +0x350. [H]

### 2.6 AI
Goals `Corrupt_Planet` (Infrastructure, TL 60) and `Sabotage_Planet` (Offensive, TL 90) share
`AI_Plan_Underworld_DeploySaboteur.lua`: `Underworld_Saboteur_Team = 1`, `AssembleForce`, `Move_To(Target)`,
`Activate_Ability()`, `Sleep(10)`; success only for Corrupt_Planet when `Target.Is_Corrupted()`; sabotage always
reports failure on purpose (script comment: avoid repeating because the cost goes up). [H]

---

## 3. Super-weapon hack (`Hack_Super_Weapon_Ability`, HackSuperWeaponAbilityClass vtable 0x8ad350)

### 3.1 Not TARGETED_HACK [H]
`TARGETED_HACK` (unit ability 20, ABILITIES §2.1 row 20) is R2-D2's tactical `Hack_Ability` (CONTAINERS.XML,
NAMEDHEROUNITS.XML). It shares nothing with the galactic hack.

### 3.2 Data [H]
Ability has no class fields (ctor 0x61b480; forced Ground_Activated; vanilla `Causes_Despawn` No). User:
`IG88_Hack_Death_Star` (IG-88). GC: `Hack_Super_Weapon_Required_Type` (+0x12040, `Death_Star_II`; resolved type
pointer read at +0x12048 [M]), `Hack_Super_Weapon_Cost` (+0x12140, 50000), `Hack_Super_Weapon_Particle_Effect`
(+0x12070, `IG88_Death_Star_Hack_Explosion`).

### 3.3 Target and apply [H]
- **Target** (0x61b610): planet X with an object of the required type in any fleet orbiting X (fleets of X in
  list order, members in order; first match, **any owner**). Accept (vf5 0x61b9b0) = X is a planet and such a
  super weapon exists.
- **Apply** (vf30 0x61b520 → 0x61b790): local human → open the hack dialog (0x3c8820, stores hero id and planet
  id) and return false (local error SFX is suppressed while a dialog is open). AI non-human → **free** hack
  (no cost) → effect below → return true (story LAND_ON/DEPLOY, presentation).
- **Command kind 7** (dialog OK 0x3cd300, setter 0x4b5400): id32 planet (+0x13C), id32 hacker (+0x140), q8 cost
  (+0x14C) = GC `Hack_Super_Weapon_Cost`. Executor 0x61b9f0: no super weapon or cost > credits → local error SFX;
  else Add_Credits(P, −cost) and the effect, plus faction `SFXEvent_Hack_Success` (+0x770).
- **Effect** (same in both paths), super weapon W, its owner Q:
  1. **Lock W's type for Q** (0x27f7c0 = the `Lock_Tech` list, P +0x1C0; saved as chunk 4 micro 0x11/0x12, type
     CRC +0x11C).
  2. Despawn the hacker (0x48cf20) **without** a respawn [H as read; IG-88 is gone for good unless another rule
     respawns him, L].
  3. If X is corrupted, start corruption removal (0x3f6770 → 0x4b7260: D +0x2E3 = 1, pending type +0x2F8 = −1,
     start +0x2E8 = mode frame, end +0x2EC = start + trunc(30 × 5.0)).
  4. **Destroy the planet** (0x3f76c0, the same routine as the Death Star's `Planet_Destruction_Ability` 0x707670
     and the tactical super weapon 0x42a7b0): story PLANET_DESTROYED (0x17); clear the siphon fields; remove every
     orbiting object (D +0xE0 list, reverse order; contained heroes get a default-time respawn) — **this includes
     W**; remove special structures (D +0x158); clear corruption to −1 with notifications; D +0x230 = 0; set
     destroyed D +0x2C8 = 1 (income then uses `Planet_Destroyed_Credit_Value`, GALACTIC_ECONOMY §1.4).
  5. Visual refresh (0x3a4820), remove every surface object (D +0xC8 list, reverse; heroes respawn at default
     time), clear the starbase (0x4bda40: D +0x138 = −1, +0x140 = 0, +0x148 = 0), owner := the `NEUTRAL`
     faction's player (0x3f8060), spawn the hack particle effect at X (command path only).
- No duration: the effect is instant and permanent.

### 3.4 AI [H]
Goal `Hack_Super_Weapon` (Hero, TL 0). Plan `AI_Plan_Underworld_HackSuperWeapon.lua`: `IG88_Team = 1`,
`Produce_Force(Target)`, `Move_To(Target)`, **`LandUnits(MainForce)`** — landing calls 0x3a00b0 per landed unit
(Lua_Land_Units 0x6cec80), which fires the Ground_Activated hack (§6.1). **Correction to AI_GALACTIC §2.7:** the
row "HackSuperWeapon = Move_To + Produce_Force" misses the LandUnits trigger.

---

## 4. Planet income abilities (mining facilities, palace, TR economy)

### 4.1 `Planet_Income_Bonus_Ability` (PlanetIncomeBonusAbilityClass, vtable 0x8c9738) [H]
| tag | off | default |
|---|---|---|
| `Percentage_Income_Modifier` | +0x178 | 0.0 — a **fraction**, ≥ −1.0 (smaller values are reset to 0 with an error, 0x708150) |
| `Absolute_Income_Modifier` | +0x17C | 0.0 (float) |
| `Additional_Multiplier_On_Mining_Colonies` | +0x180 | 1.0 |
| `Specific_Mod_Source_Text` | +0x188 | empty |
Default `Activation_Style` = 3 Galactic_Automatic (applied on add/remove of the owner, ABILITIES §3.1 "event
triggers").
- **Accept** (vf5 0x707fc0): owner is the planet itself, or the generic applicable test 0x48df20 [M: same
  planet / owner rules as other auras].
- **Apply** (vf30 0x707b90), owner O (structure or hero), planet X:
  value = float(b) × Percentage + Absolute, then × Additional_Multiplier when T(X) `Planet_Is_Mining_Colony`
  (+0xBE), where b = the planet base credit value (0x3733c0: runtime D +0x228, or `Planet_Destroyed_Credit_Value`
  when destroyed; **no black-market multiplier**). Write the income-breakdown entry (D +0x268 list) keyed by
  (CRC(ability name), O): value, expiry −1 (never), source tag = CRC(`Specific_Mod_Source_Text`) or, when empty,
  CRC of O's type string at T(O) +0x120 [M: a text id]. An existing entry is updated in place, else appended at
  the tail (GALACTIC_ECONOMY §1.4 determinism note). The value is fixed at apply time; it is not recomputed when
  b changes [H as read].
- **Remove** (vf31 0x708020): erase all entries with that key.
- Vanilla: `Empire_/Rebel_Mining_Income_Bonus` 2.0 / 0 / ×1.5 (the XML comments say "3 %", the code multiplies
  by 2.0), `Underworld_Palace_Income_Bonus` 7.0 / 0 / ×2.5. TR: e.g. `Mining_Income_Bonus` 0 / +400, `Tax_Income_
  Bonus` 1.0, `Station_Income_Bonus` 0.2, `Admin_Tier_Moff_Income_Bonus` 0.1, `Maintenance_Cost` 0 / −3000,
  `Influence_Credit_Penalty` −1.0 / −40 (TR comments match the code: "((base × X) + Y) × Z").

### 4.2 `Planet_Income_Gambling_Ability` (vtable 0x8c98b0) [H]
Fields: `Evaluation_Interval_In_Secs` +0x178 (< 0 → 0), `Min_Income_Modifier` +0x17C (−1..0),
`Max_Income_Modifier` +0x180 (≥ 0), `Chance_To_Win` +0x184 (0..1). Style 3.
**Evaluate** (vf4 0x708430, the service update [M: interval = Evaluation_Interval]), owner O, its planet X
(0x3963f0): u = Get_Float01(); m = (u < Chance_To_Win) ? Get_Float(0, Max) : Get_Float(Min, 0) (**two synced
draws**); entry value = m × float(b); key (CRC(name), O); tag = CRC(T(O) +0x120 string). Remove as §4.1.
Vanilla `Grand_Arena_Gambling_Income` (30 s, −0.2..0.6, 0.8). TR `Pirate_Activity` (120 s, −0.1..0).

### 4.3 Income display ("mining facility income") [H]
The economy breakdown (0x4b1950, GalacticModeClass) sums, per player, every breakdown entry of every owned planet
into a map keyed by the entry's source tag; siphon taken out is shown under `TEXT_ECONOMY_SMUGGLING`, siphon
gained under `TEXT_INCOME_SMUGGLER`, piracy pool under `TEXT_INCOME_PIRACY`. The mining row is the entry with tag
CRC(`TEXT_ECONOMY_MINING_FACILITIES`). Tactical `Income_Stream_Ability` / `Income_Stream_Mod_Ability` are
skirmish-tactical only (XML comment and Activation_Style); they are not galactic income.

---

## 5. Corruption requirements and the defiler's departure

### 5.1 Ability and choices [H]
`Corruption_Ability` (CorruptionAbilityClass, vtable 0x890758; ctor 0x52b0c0: style 6, Causes_Despawn). User:
`Saboteur_Corruption`. **Accept** (vf5 0x52be20): planet; the defiler not allied with X's owner (0x39a2c0); X's
owner is not the neutral player (0x294aa0); D +0x2E2 and +0x2E3 = 0 (no transition); **D +0x2F4 = −1 (not
corrupted)**; not destroyed (D +0x2C8 ≠ 1); T(X) byte +0xBA set [H for the test; M that +0xBA is
`Planet_Retains_Residual_Influence` per xml_field_tables]; at least one of `Corruption_1/2/3_Type` ≠ −1.
Choices (0x52b690): one `CorruptionChoiceProperties` record (0x68 bytes, 0x52c570) per non-empty slot, slot order
1, 2, 3: +0x00 type, +0x04 `Credit_Cost` (float), +0x10/+0x18 `Required_Unit_Type` list/count, +0x20
`Required_Unit_Count`, +0x24 `Required_Mission_Type`, +0x28 `Required_Mission_Count`, +0x38/+0x40
`Required_Planets` list/count, +0x48 a string [L: which field].

### 5.2 Requirements [H]
| check | function | rule |
|---|---|---|
| credits | 0x523780 | cost ≤ 0, or cost ≤ P credits |
| mission | 0x5237b0 | mission type = −1 or count < 1, or **P's completed-corruption counter[mission type] ≥ count**. Counters = P +0x458 int[8] (one per corruption type), +1 on each corruption applied by the direct path (0x2820a0) [M: also on a won corruption mission] |
| units | 0x523950 | list empty or count < 1 → met. Else take every object of every listed type (GOM type list order) owned by P; count it as follows: if it is on a planet (0x3963f0 ≠ 0) or its type is not `Destroy_When_Used_As_Corruption_Prereq` (T +0xE4), and its parent (+0x2B0) is a container (+0x337 ≠ −1), count the **container once** (distinct container ids); otherwise count the object. Met when count ≥ `Required_Unit_Count`. **The units can be anywhere in the galaxy** (no location test). A list such as `Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue` is a union |
| planets | 0x523810 | list empty → met; else P must own ≥ 1 object of any listed planet type |
Enforcement: the AI picker applies all four; the dialog (0x3c6eb0 / refresh 0x3e6630) shows "n/m" per row and
enables the button only when all hold; the **executor re-checks only credits and units** and charges the cost
**before** the unit check (no refund when the unit check fails) [H as read].

### 5.3 Consumed prerequisites (0x52b870) [H]
Runs after the requirements pass, when `Required_Unit_Count` > 0, for the first listed type that has live objects
(0x5235b0), and only if that type has `Destroy_When_Used_As_Corruption_Prereq` (vanilla: none).
- Named hero type (T +0x13): the first object of the type gets a default-time respawn at X and is despawned.
- Otherwise: candidates = P's objects of the type (deduplicated by container as in §5.2) that are on a planet;
  each gets weight `Get_Float(0, 4.0)` (one synced draw per candidate, list order) and goes into a max-heap; pop
  `Required_Unit_Count` times, each pop kills the container's members, the fleet contents and the object
  ("Destroying %s with weight %g").

### 5.4 Apply, command, AI pick [H]
- **Apply** (vf30 0x52b160, no direct callers): build choices; none → local error SFX. Local human → open the
  corruption dialog (0x3c6eb0, story hook "corruption_dialog") and return false. AI non-human: candidates =
  choices passing §5.2; weight per type t with n_t = counter[t]: Intimidation 2/(n0+1); Piracy 1 + 1/(n1+1);
  Kidnapping 0.75; Racketeering 1.5; Corrupt_Militia 0.5 + 1/(n4+1); Bribery 0.25; Black_Market 2.5;
  Bonded_Citizens 0.75. One `Get_Float(0, total)` draw, first cumulative ≥ r → executor.
- **Command kind 6** (dialog 0x3de390, setter 0x4b5450; pack 0x523440): id32 planet (+0x13C), id32 defiler
  (+0x140), then the record: 4-bit type, string (+0x48), q8 cost, 4-bit mission type, 6-bit mission count, 8-bit
  required unit count, 3-bit unit-type count, 3-bit planet count, then each unit type and each planet type as a
  32-bit type-name CRC (T +0x11C). **The executor uses the record from the payload**, not the XML.
- **Executor 0x52bf50**: as L1_S4 §4.5 (direct vs mission path), with the requirement details above.

### 5.5 The defiler's departure [H]
On both executor paths (direct: after "Corruption underway at %s"; mission: after queuing the battle and the
pending corruption mission): **respawn scheduled at the target planet X with time 0 → `Default_Hero_Respawn_Time`
(GC +0x11F0, vanilla 360 s)** (0x48eb10, skip-team-check = 1), then the defiler and its team are despawned at once
(0x48cf20). Where it reappears: §6.2 (nearest planet owned by its player). It does not travel; it vanishes from the
map and reappears later. On failure of any check the defiler stays.

---

## 6. Shared mechanics

### 6.1 Ground_Activated trigger [H]
Entry 0x3a00b0(unit U, planet X) → U's special-ability manager 0x37ccf0 → 0x37b580(style 6, context X,
"apply all" = 1). For each ability in the type's `Abilities` list order: style matches (or the 9/11 special case
0x48e240), not blocked by 0x48d5a0, mode-allowed (0x37c270), ready (0x37be10), accept (vf5) and black-market gate
(0x39b0b0) pass → apply via 0x48cc30 (vf30). If apply returned true and `Causes_Despawn` (+0x41): r = vf12 =
`Get_Float(Min_Respawn_Time, Max_Respawn_Time)` (one synced draw, base fields +0x54/+0x58, default 0 [M]), schedule
respawn (U, planet = null → U's current planet, r) and despawn U. Because "apply all" is set, **every** matching
ability is tried, in list order.
Callers: command kind 1 (human "activate", setter 0x4b5320: id32 planet +0x13C, id32 unit +0x140; for a fleet the
first member with a ready style-6 ability at X, 0x398670), Lua `Activate_Ability` galactic (GameObjectWrapper
0x570670 / GalacticTaskForce vf39 0x6cd180: the object must be in orbit, not landed; transports apply to each
carried unit; optional type filter), Lua `Land_Units` (0x6cec80).
**Double departure risk [L]:** the black-market AI path (§1.2 step 5) and the corruption executor already
schedule the respawn and despawn, then return true, so the trigger's Causes_Despawn branch runs a second time
(one extra synced draw; 0x391310 is idempotent; whether a second respawn entry is queued depends on 0x48eb10 with
a dead hero and was not resolved). Port: do the extra draw, schedule once, and note it for an in-game probe.

### 6.2 Hero respawn scheduling (0x48eb10 → 0x3fe0a0, service 0x3fc750) [H]
- Never in skirmish (0x28afb0). Hero type must resolve (0x396380: team type or own type). If time ≤ 0, time =
  GC `Default_Hero_Respawn_Time`. Named heroes with a pending entry are not queued twice (0x3fa300). Without the
  skip flag, a hero whose team/fleet/transport still holds live members is not queued ("Cannot schedule the
  respawn of %s yet because …").
- Entry in the **target planet's** spawn list (D +0x240, 0x28 bytes): due = galactic mode frame + int(30 × time
  + 0.5), type, owner id, flag (hero +0x330). **Time < 0 → nothing is queued** (0x3fe0a0 tests 0 ≤ time): with
  TR's −360 default every default-time respawn above is lost (heroes stay dead), but the caller still reports
  success. [H]
- Service: when due, collect the owner's planets (category-3 query by owner), order them with 0x3fefc0 relative
  to the scheduling planet [M: nearest first], spawn at the first that accepts (0x3f6650), "Successfully spawned
  %s at %s!", play `SFXEvent_Hero_Respawned` (T +0x1280) for the local player, notify the AI. No owned planet →
  the entry is dropped. (AI_GALACTIC_3 §4.3 covers the cancellation of these entries by HIJACK.)

### 6.3 Synced draws added here (all on 0x140a13e24)
| when | draws | where |
|---|---|---|
| black-market apply with a valid tech index | 1 Get_Float (respawn R) | 0x61a4a0 |
| black-market AI pick | 1 Get_Float (0, total) | 0x61a4a0 |
| sabotage option pick | Get_Int(0, n−1): 0 when n = 1 | 0x5b1920 |
| sabotage vigilance failure | 1 (vf12) | 0x5b1920 |
| sabotage AI structure pick | Get_Int(0, n−1) | 0x5b32c0 |
| any Ground_Activated apply with Causes_Despawn that returns true | 1 (vf12) | 0x37b580 |
| corruption AI pick | 1 Get_Float(0, total) when total > 0 | 0x52b160 |
| consumed corruption prerequisites | 1 Get_Float(0, 4) per candidate | 0x52b870 |
| income gambling evaluation | 2 | 0x708430 |
MP note: for a human owner the sabotage apply returns before drawing on every machine except the owner's (step 1
of §2.3), so a human's multi-option sabotage pick would desync in MP; irrelevant in SP. [H]

### 6.4 Story and Lua surface touched
- Story events: BUY_BLACK_MARKET 0x38 (dialog, item Name), GALACTIC_SABOTAGE 0x39 (structure type name, command
  path and reward 0x81), PLANET_DESTROYED 0x17 (hack), LAND_ON 2 + DEPLOY 0xC (every successful apply),
  OPEN_CORRUPTION 0x3C (dialog, not read here [L]), CORRUPTION_* (L1_S4 §4.5).
- Story rewards: GIVE_BLACK_MARKET 0x7E, SABOTAGE_STRUCTURE 0x81, LOCK_UNIT 0x4D (same list as the hack lock),
  REMOVE_CORRUPTION (0x3f6770 with the reward's seconds).
- Lua: `Set_Black_Market_Tutorial` (P +0x448), `Set_Sabotage_Tutorial` (P +0x449), `Activate_Ability`, `Land_Units`,
  `Is_Ability_Ready`, `Lock_Tech`, `Is_Corrupted`.

---

## 7. Proposed engine shape (names only)

> **Status 2026-10-30:** The proposed functions are implemented as `GalacticSim` members (`engine/galactic/include/alamo/galactic/campaign.hpp:678-774`, API overview in `engine/galactic/include/alamo/galactic/special_systems.hpp:7-37`, code in `engine/galactic/src/special_systems.cpp`). Some names differ from this list (`bm_offers`, `corruption_choices`, `hero_depart`, `mining_income`), and command kind 6 (corruption) is executed by apps/sim `CampaignBattles`, not `GalacticSim::execute`.

- Data: `BlackMarketItemDef` + `BlackMarketItemTable` (file order, by name CRC); `BlackMarketAbilityDef`,
  `GalacticSabotageAbilityDef`, `HackSuperWeaponAbilityDef`, `CorruptionAbilityDef`, `PlanetIncomeBonusAbilityDef`,
  `PlanetIncomeGamblingAbilityDef`; GameConstants fields `hack_super_weapon_{required_type,cost,particle}`,
  `sabotage_particle`, `activated_*_ability_names`, `default_hero_respawn_time`; planet-type `vigilance`,
  `black_market_modifier`, `is_mining_colony`; object-type `sabotage_cost`, `tutorial_sabotage_cost`,
  `destroy_when_used_as_corruption_prereq`.
- Player state: `bm_purchased` (ordered), `bm_tutorial`, `sabotage_tutorial`, `corruptions_completed[8]`,
  `locked_types` (shared with Lua_Lock_Tech).
- Planet state: `sabotage_count`, `credit_halt_entries`, `income_breakdown` entries with `source_tag`,
  `respawn_queue`.
- Functions: `ground_activated_trigger(unit, planet)`, `schedule_hero_respawn(hero, planet, seconds)`,
  `bm_build_offers`, `bm_execute_purchase`, `bm_allows_unit_ability`, `bm_allows_special_ability`,
  `sabotage_options`, `sabotage_cost`, `sabotage_execute_structure`, `hack_find_super_weapon`, `hack_execute`,
  `planet_destroy` (shared with Death Star), `corruption_build_choices`, `corruption_requirements`,
  `corruption_ai_pick`, `corruption_execute` (existing L1_S4 path), `income_bonus_apply/remove`,
  `income_gambling_evaluate`, `economy_breakdown_by_tag`.
- Command: `SpecialAbilityEvent` (id 13) kinds 1, 4, 5, 6, 7 with the payloads in §1.4, §2.4, §3.3, §5.4, §6.1.

## 8. Open items (no in-game probes run; prepare under tools/probe_mod/ if wanted)
1. Exponent of the sabotage cost scaling (D +0x350 assumed) and save of D +0x350 / the halt list.
2. Double departure on AI black-market / corruption applies (§6.1).
3. Writer of P's price-modifier list (+0x270) used by m(P); `Tech_Level_Adjustment` reader.
4. Default values of special-ability `Min/Max_Respawn_Time` (+0x54/+0x58) and BM item `Tutorial_*` fields.
5. Ordering key of 0x3fefc0 (respawn planet choice); D +0x2C9; D +0x2E4 (success speech gate); record +0x48 string.
6. Whether the sync CRC player term covers purchased items, locked types and corruption counters.
7. Gambling service interval source (vf3) — assumed `Evaluation_Interval_In_Secs`.
8. Mission-path increment of the corruption counters (battle end 0x28bf10).

---

## 9. Story summary "Abandon", auto-resolve "Withdraw", tactical retreat dialog "Autoresolve"

GUI helper used below: 0x1c30d0(dialog, control name, b) finds the control (case-insensitive, 0x1e1ef0) and
calls its vf10: `GUIGadgetClass::vf10` 0x1e3f30 stores b at gadget +0x79 and, when b = 0, also clears +0x78 (both
default 1 in the gadget ctor 0x1e3cb0). It is used to switch a control off [M: enable vs. show; either way the
control cannot be clicked].

### 9.1 Story summary dialog Abandon: dead button [H]
- `IDD_STORY_SUMMARY_DIALOG` is built by 0x107280 with callback 0x107bb0. At creation it switches
  `IDC_SUMMARY_DIALOG_ABANDON` off (0x1c30d0(…, 0)), with no condition. This is the only code reference to the
  string (strings_xrefs), so nothing ever switches it back on.
- The callback handles only: close (`IDC_SUMMARY_DIALOG_CLOSE_BUTTON`, and event 0 = dialog close), the
  `IDC_RADIO_COMPLETED` / `IDC_RADIO_ACTIVE` filter (sets 0x140a12220, refills the list), and goal-list selection
  (event 0xC: description/reward text). **There is no Abandon handler.**
- No command, story event, Lua function or plot state change is associated with abandoning: no "abandon" string
  in the Lua API, the story code or the shipped story XML/Lua. The button exists only in `GUIDIALOGS.RC`
  ("TEXT_BUTTON_ABANDON", id 1289).
- **Port:** show the button permanently disabled. Drop the PortExtension 0x5341 stand-in, or keep it as an
  explicit no-op outside the retail command set. Nothing can be abandoned and nothing is refused.

### 9.2 Auto-resolve dialog Withdraw → event 49 → `Set_Retreating` [H]
- The dialogs are `IDD_AUTO_RESOLVE_DIALOG` / `_LITE` (setup 0xa7fb0, callback 0xa8b80) and their counterpart
  (setup 0x82fe0, callback 0x83df0). Clicking `IDC_WITHDRAW_BUTTON` creates a `WithdrawlEventClass`
  (ctor 0x4aded0, type 0x31 = 49) and queues it with OutgoingEventQueue::Add.
- **Payload:** none. Pack (vf5 0x4adf20) and unpack (vf6 0x4adf00) only call the ScheduledEvent base, so the wire
  record is the header alone (6-bit type, player id, 26-bit execute frame; COMMANDS §1.2). The withdrawing player
  is the header player id (event +0x24). **Correction to COMMANDS §2.5 row 49:** the payload is H, not L; the
  manager at 0x140b31400 is the AutoResolve object.
- **Execute** (vf4 0x4adf40): if the player exists, call AutoResolve vf5 (+0x28) = `Set_Retreating(player)`
  0x60cf00. Set_Retreating returns 0x80670003 when the resolve has not begun (+0x09 = 0) and 0x8067000A when a
  retreat is already set (+0x0A = 1). Otherwise it sets +0x0A = 1 and +0x10 = player and returns 0. The executor
  ignores the result, so a second or early withdraw is silently dropped. The rest follows
  BATTLE_TRANSITIONS §4.4/§4.6 (retreat attrition, vf6 → 0x670002, vf9).
- **Enable rule:** the dialog setup reads the current conflict record (GOM 0x2ac5a0: players at +0x04 and
  +0x08). If the local player is one of the two sides, the planet and force texts are filled and Withdraw keeps
  its default (on). Otherwise the texts are blanked and Withdraw is switched off (0x1c30d0(…, 0)).
- `IDC_WITHDRAW_TO_COMBO` only writes a UI global (0x140a61c14). Nothing reads it, so the destination choice is
  not sent and has no effect. `IDC_ADVANCED_RESOLVE_INFO` only swaps the dialog between full and lite.

### 9.3 Tactical retreat dialog (`IDD_RETREAT_DIALOG`, open 0xdc410, callback 0xdc5d0)
- **"Autoresolve" (`IDC_AUTORESOLVE`) sends no command.** The GUI callback drives the sim directly on the
  clicking machine [H]:
  1. Gate: mode +0x30 object (the tactical retreat coordinator) vf +0x48(local player) must return 0. That is
     the same "can retreat" reason function used by the retreat request below, so the button does nothing while
     retreat is refused or already running [H for the call, M for the meaning].
  2. Space mode (mode vf +0xE0 = 2): AutoResolve vf1 (init space). Land (= 1): vf2 (init land). Then vf8 (gather
     combatants from the tactical mode, "from tactical"), then vf4 Begin(vf27() [L: argument meaning]).
  3. If Begin returned 0: vf6(force = 1). If that returns 0x670002 (done, with a retreating player), let r =
     vf21() (the retreating player): if coordinator vf +0x48(r) = 0, call coordinator vf +0x70(r) [M: start that
     player's tactical retreat] then vf7 reset; otherwise vf9 (early withdrawal), then vf7 reset. For any other
     result: vf7 reset.
  4. Close the dialog.
  This confirms BATTLE_TRANSITIONS §4.5 (vf1/vf2 → vf8 → vf4 → vf6(force)) and adds the retreat tail.
  **Determinism:** it bypasses the command stream. In MP that is a desync (an engine limitation). In SP and
  replay it is a local-player action, so the port should wrap it in a port command so that replays reproduce it.
- **"Retreat" (`IDC_RETREAT`)**: 0x30d5d0 (land) / 0x30e1b0 (space) → 0x340920(coordinator, local player,
  request = 1) [H]:
  - result 1: player is not local, or id −1;
  - otherwise the coordinator vf +0x48 reason code, if non-zero;
  - result 2: coordinator +0x28 > 0;
  - result 5: retreat already flagged (+0x7E, +0x1C ≠ −1 or +0x18 ≠ 0);
  - otherwise queue a **RetreatEvent (event 22)** via 0x532620(event, player, 0) and return 0.
  A non-zero code plays a faction refusal SFX: land +0x2200, or +0x2210 / +0x2220 / +0x2230 for codes 3 / 4 / 6;
  space +0x2130…+0x2160. The bit sent by 0x532620 is [M] (COMMANDS §2.3 row 22: 1 = request).

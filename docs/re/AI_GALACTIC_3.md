# Galactic and tactical AI, part 3: tactical marshallers, heroes, population, MagicPlan, perception DLL

Clean-room behavioural spec from the Ghidra 12.2 decompilation of the 64-bit Steam `StarWarsG.exe` (FoC v1.121, image base 0x140000000; all addresses are RVAs), disassembly of the same exe and of `PerceptionFunctionG.dll` (same install), constants read from both binaries, and the shipped FoC XML / Lua. No decompiled code is reproduced.

Tags: **[H]** read directly from code or data; **[M]** one inferred link (a field meaning, a callee's purpose); **[L]** plausible, not verified.

Read first: AI_GALACTIC.md, AI_GALACTIC_2.md (marshaller framework, notation **PP**, **rec[i]**, **slot**, cost(T), btime(T), power(T)), AI_GALACTIC_TOKENS.md, AI_TACTICAL.md, AI_TACTICAL_PERCEPTION.md. This file does not repeat them.

Extra notation:
- **mode** = PP+0x198: 0 galactic, 1 land, 2 space (copied from the AI player's mode at PP Init) [H].
- **pop(T, m)** = 0x373500: m = 2 → `Space_Override_Population_Value` (+0x2128) if ≥ 0; m = 1 → `Land_Override_Population_Value` (+0x212C) if ≥ 0; otherwise (and for m = 0) `Population_Value` (+0x2124) [H].
- **pool** = the reinforcement pool list built by the Reinforcement marshaller (global vector 0x140b3a690, 0x18-byte entries {object, type, count}) [H].
- **free store** = the AI player's mode free store (AI player → +0x50 → +0x38; AIFreeStoreClass vt 0x878f80, Land 0x8b6b68, Space 0x8b6a38) [H].

---

## Status (most-needed first)

| § | Topic | Status | Main consumer |
|---|---|---|---|
| 1 | Reinforcement and TacticalBuild marshallers (tactical PotentialPlan) | **complete** [H]; one field meaning [M] | A1 tactical AI (Produce_Force), A2 solver |
| 2 | Hero AI and plan attachment | **complete** for the solver [H]; release rules outside the solver partly [M] | A2 / A1 Hero marshaller |
| 3 | Population caps (galactic and tactical) | **complete** [H]; one rounding field meaning [M] | A2 Production / Contrast, A1 Reinforce, galactic sim |
| 4 | MagicPlan spawning (HIJACK_UNIT) | **complete** [H]; "no charge" [M] | galactic sim hook (spawn), A2 build tasks |
| 5 | Production-market planet order | **answered** [H code site, M list direction] | A2 galactic_build |
| 6 | Terrain data the AI reads | **complete** for galactic [H]; land terrain id source [M] | A2 TerrainType / Contrast |
| 7 | Perception DLL failure semantics; synced draws per pass | **DLL complete** [H]; Lua-side consequences [M]; actual counts need a probe | A1, A2, L1 (EvaluatePerception), S2 equations |
| 8 | Other A2 open items the binary answers | K = 1.0, health threshold, Flush_Category / Flush_All, budget tick period, PAYG reclaim, Production travel estimate, source-planet lists | A2 |
| 9 | Corrections to earlier documents | list | all |
| 10 | Open items and in-game probes | list | Gabriel |

---

## 1. Reinforcement and TacticalBuild marshallers

### 1.1 Objects and vtables [H]

The marshaller list builder 0x6c0e60 pushes static objects. Their first qword is the vtable.

| Marshaller | object | vt | Score | Commit | Setup A | Setup B / C / Finalize | IsPermanentVeto |
|---|---|---|---|---|---|---|---|
| Reinforcement | 0x140a44990 | 0x8bf1b8 | 0x7276a0 | 0x727b30 | 0x727940 | none | **false** |
| TacticalBuild | 0x140a44a20 | 0x8bf290 | 0x727d30 | 0x728000 | 0x727fb0 | none | **true** |

Order (AI_GALACTIC_2 §1.1): land = FreeStore, Plan, Reinforcement, TechTree, Hero, Contrast, TacticalBuild; space = FreeStore, TechTree, Plan, Reinforcement, Hero, Contrast, TacticalBuild. In both lists **Plan runs before Reinforcement**, so rec.teamSlot (+0x10) is already set when Reinforcement scores.

### 1.2 Tactical solver facts that the two marshallers rely on [H]

- Every iteration resets, per candidate, rec+0 (score) = 0, rec+4 (free unit) = −1, rec+8 (**pool index**) = −1, rec+0x14 (contrast entry) = −1 before any Score (solver 0x6c3da0). So FreeStore (first in the list) recomputes rec+4 each iteration, and Reinforcement may then clear it.
- Reset 0x6c3080 fills PP+0x30/+0x34/+0x38 only in galactic. In tactical they stay 0, PP+0x40 (gross) and PP+0x44 (net) are reset to 0, and the committed-reinforcement-pop counter 0x140b3a52c is reset to 0. The Budget marshaller is not in the tactical lists, so **tactical net cost PP+0x44 counts only units bought through TacticalBuild**.
- PP+0x104 is a solve counter incremented at the start of every solve, so it is ≥ 1 during Setup.
- PP+0x120 (int per selection) is the selection's pool index or −1. In galactic the solver pushes −1 per selection; in tactical the Reinforcement Commit pushes it. PP+0xB8 (int per selection) is the selection's slot.

**Tactical FreeStore differences** (Setup A 0x7287a0, Score 0x728560; galactic rules in AI_GALACTIC_2 §1.4) [H]:
- Distance key per eligible unit: Σ over TF stages of (stage reachability cost to the unit, reachability mode **7**)² + 2-D distance² (unit − stage). If at least one stage cost was finite, divide by the number of stages. If none was finite, the key is instead the 2-D distance² from the unit to the goal target's position (galactic keeps the undivided sum).
- No key is computed (the unit keeps key 0, i.e. it scores as the nearest) when the goal has no target object, or when the unit is in the "in transit" state (0x3a9760 true). This holds in galactic too [H read; M meaning of 0x3a9760].
- Score = (maxKey ≠ 0 ? 1 − key/maxKey : 0) + 2 if unit +0x3A2 bit 2; **no +2 galactic bonus**. Range 0..1 (3 for the flagged unit).

### 1.3 Reinforcement Setup A 0x727940: build the pool [H]

Runs only when all hold: mode ≠ 0; not MagicPlan; the player has population room for one unit (0x2b03e0 with no type, §3.4). The pool vector is cleared by Reset.

Source of the pool:
- **Campaign battle** (the current mode has a parent galactic mode): the parent store's per-player reinforcement record (parent GOM +0x498 map keyed by player id; record +0x20 = object-id list, +0x28 = count). This is the list of the player's persistent units in orbit (AI_TACTICAL §7.4, BATTLE_TRANSITIONS reinforcement split). Done by 0x727490.
- **Skirmish** (no parent): the player's tactical reinforcement table, player +0x2C8 (type pointers), count +0x2D0 (bought land units, AI_TACTICAL §7.1).

For each slot i of the source, in order, one entry is appended (empty first, so pool indices equal source indices). The entry is filled only if the free store has not reserved slot i (free store +0x88[i] is null; vf+0xE8 0x4d3cf0) and:
- campaign: the id is not 0x3FFFFF, the object exists in the parent store and is owned by this player → entry = {object, its type};
- skirmish: the type pointer is non-null → entry = {null, type}.

**count(E)** for entry type E: E's `Company_Units` count n (+0x960); n = 0 → 1; if the first member type has `Create_Team` (+0x67) → ceil(n / E.`Max_Squad_Size` (+0x1E44)) with integer ceiling, and `Max_Squad_Size` 0 → 1; otherwise n. This is the number of selections one pool entry can serve.

Free store reservations: free store vf+0xE0 (0x4d3040) reserves slot i for an owner (succeeds if free or already that owner); +0x88 is the reservation vector [H]. Who reserves (plan start or Reinforce) was not traced [M: the TF's Reinforce / plan activation].

### 1.4 Reinforcement Score 0x7276a0 [H]

For candidate type T, with f = rec+4 (the free unit FreeStore found this iteration):

1. **Not applicable → 0** when MagicPlan, or T has behaviour 0x2A (TACTICAL_UNDER_CONSTRUCTION), or AllowFreeStoreUnits is false.
2. **No pool usable** — any of: player +0x39C is 0 (reinforcements disabled; the story reward DISABLE/ENABLE_REINFORCEMENTS writes it, 0x45eca0; constructor sets 1); the pool is empty; remaining pop (cap − used, §3.3) < pop(T, mode) + committed pop (0x140b3a52c). Then return **veto 1e18** if f = −1, else **0**.
3. Otherwise scan pool entries j in order for the first match. Entry type E matches T when:
   - E has no `Company_Units`: T == E;
   - else with M = E's first member type: T == M, or (M has `Create_Team` or T is a named hero (+0x13)) and T == E.
   
   A matching entry is **skipped** if an earlier selection k already uses it (PP+0x120[k] == j) and that selection's slot belongs to a different TF than this candidate's rec.teamSlot. Same TF, or unused → accept.
4. On the first accepted j:
   - if f ≠ −1 and the free unit's distance key < **300.0** (strict) → return **0** (keep the nearby free unit; rec+8 stays −1);
   - otherwise rec+4 := −1 (drop the free unit, so FreeStore's commit will not consume it), rec+8 := j, return **2.0**.
5. No accepted entry → veto 1e18 if f = −1, else 0.

The key is a squared distance (plus squared reach cost), so 300 ≈ 17.3 world units: in practice a reinforcement beats any free unit that is not already at the stage [M for the practical reading].

### 1.5 Reinforcement Commit 0x727b30 [H]

- Push rec+8 (pool index or −1) to PP+0x120.
- If j ≠ −1: if entry.count still equals count(entry type) (first use), committed pop += pop(entry type, mode) (the company type, charged once per entry). Then entry.count −= 1; at 0 the entry's object and type are cleared, so it no longer matches.
- Effect: a company entry with k members serves k selections of its member type in one TF and is charged its population once.

### 1.6 How the task force uses the result [H]

- 0x6c0520(PP, TFDef) returns the TF's pool-index list: the distinct non-negative PP+0x120 values (bitset de-duplication, first-occurrence order over selections) whose selection slot belongs to that TF. In a campaign battle the bitset size is the parent record count; in skirmish player +0x2D0.
- `LandTaskForceClass.Reinforce` (0x6ed5f0 → campaign 0x6ed9f0 / skirmish 0x6ee010; AI_TACTICAL §7.4) consumes this list. The campaign variant **does nothing and returns no value** when the parent record's count differs from the size of the free store's reservation vector (+0x88) [H] (a guard against a changed pool).
- Units chosen through the pool are therefore not "produced": the TF's Produce_Force task list does not cover them [M: TF+0x60 list holds the to-be-produced types].

### 1.7 TacticalBuild Setup A 0x727fb0: the open-pad list [H]

Clears the global pad vector 0x140b3a6a8 and fills it through 0x4d3830 from the free store's visible-pad table (free store +0xE8, a linked list; entry +0x10 = pad object id, +0x20 = reservation). A pad is listed, in table order, when: the entry is not reserved (+0x20 == 0); the pad object exists in the current store; it has the build-pad behaviour (slot 0x29); it is empty and unblocked (0x42e400, L1_S4_REMAINING §3.6); its cooldown has elapsed (0x42daf0).

### 1.8 TacticalBuild Score 0x727d30 (return values from the disassembly) [H]

1. **Not applicable → 0** when T lacks behaviour 0x2A and (AllowFreeStoreUnits or MagicPlan). Normal units are therefore priced only by build plans (AllowFreeStoreUnits = false, not magic); pad structures (0x2A) by every plan.
2. Cost c:
   - T without 0x2A: the tactical production price 0x496d80 on the player's tactical build manager (player +0x3A0): the type's tactical build cost × (1 − the player's summed price-reduction modifiers) × (in a campaign battle) the faction's corruption price multiplier 0x33e470 [H call chain, M names]; 1e18 for a null type.
   - T with 0x2A: the pad price 0x3744e0 (`Tactical_Build_Cost_Multiplayer` in skirmish, else `_Campaign` falling back to multiplayer, × planet factor; L1_S4_REMAINING §3.6).
3. **Veto** if c ≤ 0, or c + PP+0x44 > player credits (+0x70) (equal is allowed).
4. T without 0x2A:
   - veto unless 0x288b80(player, T): not `Cinematic_Object_Only` (+0x21), tactical cost > 0, named heroes only when skirmish option bit 0x140b154bc & 1, special weapons (+0x1C) only with bit & 4, flags +0x2A / +0x29 clear, affiliated with the faction, every `Tactical_Build_Prerequisites` type (+0xA18) owned, no hostile owner on the +0xA70 chain, and the `Build_Limit_*` fields (+0x888 lifetime, +0x88C allies lifetime, +0x880 current, +0x884 allies current) not reached;
   - then **1.0** if any object in the free store's builder list (free store +0x120) can build T now (0x497140: T's `Tactical_Production_Queue` (+0xA30) is 2 or 3, the builder is friendly, the builder type lists T for its owner's faction, build limits incl. in-progress counts, price and build time both finite and > 0, prerequisites owned); else **veto**. [H; M that +0x120 is "builder objects"]
5. T with 0x2A: veto unless the goal has a target object (goal +0x58); then **1.0** if any pad in the open-pad list accepts T for the player's faction (pad type's buildable list, 0x372c10 / 0x374420), else **veto**. Score does not look at distance.
6. Every veto is **permanent** (IsPermanentVeto true): the candidate is erased for the rest of this solve, including credit vetoes.

### 1.9 TacticalBuild Commit 0x728000 [H]

- If not MagicPlan and rec+4 == −1 and rec+8 == −1 (neither free unit nor pool unit): PP+0x44 += c (same formula as Score).
- rec+4 := −1 (no effect: FreeStore already committed).
- T without 0x2A: push null to PP+0x138.
- T with 0x2A: over the open-pad list, take the pad that accepts T with the smallest 2-D distance² to the goal target position (strict <, first wins). Push it to PP+0x150, remove it from the list (one structure per pad per solve) and push T to PP+0x138. No pad → push null to PP+0x138.

### 1.10 Resulting tactical selection rules (summary)

| Plan kind | What a candidate needs | Typical scores |
|---|---|---|
| AllowFreeStoreUnits (attack, defend, escort…) | a free unit of T, or a matching pool entry; else Reinforcement vetoes (non-permanent) | free unit 0..1 (+2 flagged); pool unit 2.0; TechTree 0.4/level; Plan 1 − n/max; Contrast ×2; Hero |
| Build plans (AllowFreeStoreUnits = false, not magic) | buildable now by some builder and affordable cumulatively (PP+0x44); else permanently erased | TacticalBuild 1.0 + TechTree + Plan + Contrast |
| Pad structures (0x2A, any plan) | an open pad that accepts T, a goal target, affordable | TacticalBuild 1.0 |
| MagicPlan | neither marshaller applies | Plan, TechTree, Contrast, Hero |

**RNG:** neither marshaller draws. The tactical solve draws only in Plan Setup C (team start) and in Contrast for IgnoreTarget plans (AI_GALACTIC_2 §1.5) [H].

### 1.11 What Produce_Force does with tactical build tasks [H branch, M state names]

`TaskForce.Produce_Force` (0x5fe240) creates one AIBuildTask (0x524ee0) per entry of the TF's to-produce list. In the BUILD_TASK_LIST state (0x648980), outside galactic mode:
- **Skirmish and AllowFreeStoreUnits = false:** the task takes the next global tactical queue id (0x140b3995c); for each builder in free store +0x120, in order, 0x496a20(build manager, T, builder, id) tries to start production. On the first success it registers the unit with the PP (0x6c1e80), listens to player signals 0x22 and 3, increments the counter and waits. No builder accepts → FAILED (Production_Failed).
- **Otherwise** (campaign battle, or AllowFreeStoreUnits = true): if AllowFreeStoreUnits and the free store can supply the unit (0x4d2bd0) → wait for the free-store allocation; else **FAILED**.
- So **campaign battles never buy units through the AI**: units come from the free store, the pool (Reinforce) or MagicPlan.

---

## 2. Hero AI and plan attachment

### 2.1 Which candidates become hero records (Hero Setup C 0x726710) [H]

For each candidate T, the record fields are reset first: +0x38 = 0, +0x3C = 1e18, +0x40 = 0, +0x44 = −1, +0x48 = 0, +0x4C = +0x50 = 0, +0x54 = 0, +0x58 = null. Let H = T's company/squadron representative (first `Company_Units` member, else first `Squadron_Units` member, else T; 0x6c00c0). T becomes a **hero record** (+0x54 = 1) only when all hold:
1. H is a named hero (+0x13), a generic hero (+0x14), a space special weapon (`Is_Special_Weapon_In_Space` +0x1E) or has behaviour 0x37 for mode 1.
2. Allowances: (any TF allows hero attach, 0x140b3a526) or H is neither named nor generic hero; and (any TF allows special-weapon attach, 0x140b3a527) or H is neither +0x1E nor behaviour 0x37.
3. A unit is found: the first eligible free-store unit whose type resolves to T, else the first pool entry of type T (its object). With no unit, a campaign battle (the mode has a parent) skips T; galactic and skirmish continue with the type.
4. A Lua script exists: the unit's per-object script (+0x2D8, via its container 0x6c0020) or the type's `Lua_Script` instance (+0x1AF8, loaded only with `Execute_Script_On_Type`). No script → not a hero record.
   - **Precision 2026-11-12 (`A2M_R2_RE_B.md` §3) [H]:** the object script must still be **live**. The type instance is
     read from H (the representative), not from T. The `Lua_Script` tag itself is not consulted.
   - Galactic hero objects whose scripts `ScriptExit()` outside their tactical mode (Home_One, Sundered_Heart,
     Millennium_Falcon, Vader, Yoda, …) have no script after their first galactic pump. They are not hero records,
     and AI plans pick them as plain units, even in plans that allow hero attach.

Then, on that script: `PlayerObject` = the AI player; `MinPlanAttachCost` → +0x4C and `MaxPlanAttachCost` → +0x50 (numbers only); `Evaluate_Attack_Ability(target or nil, goal type name)` → +0x38; `Get_Escort_Ability_Weights(...)` → +0x58 (must be a WeightedTypeList object). Shipped hero scripts (GameObject/*.lua, `require("HeroPlanAttach")`) define these; e.g. DarthVader.lua sets Min 5000 / Max 0.

### 2.2 Hero Setup B 0x726410: extra hero candidates [H]

Only when 0x140b3a526 or 0x140b3a527:
- free-store units whose representative is named hero / special weapon / behaviour 0x37, filtered by the two allowances;
- **pool entries** (§1.3) whose representative is a named hero without `Stay_In_Transport_During_Ground_Battle` (+0x2D) (the entry type is added);
- with hero attach allowed: `Field_Com_Rebel_Team` / `Field_Com_Empire_Team` (not in space) and `Fleet_Com_Rebel_Team` / `Fleet_Com_Empire_Team` (not in land), each only if affiliated with the player's faction.

### 2.3 Hero Score 0x726f40 [H]

Let u = the free unit object of rec+4 (null if none or the store sentinel).
- Not a hero record → **0**.
- Hero record and (no team, or not MagicPlan):
  - allowance check as §2.1 item 2 on H; failing it → **0** (the hero is then a plain unit for the other marshallers);
  - if u is attached to something (u +0x2B0 → +0xF8 non-null) → **1e18**;
  - otherwise return s = rec+0x3C / rec+0x40 + rec+0x38 when all hold: (the current mode is not galactic, **or** (Min = 0 or Min ≤ gross PP+0x40) and (Max = 0 or gross ≤ Max)); rec+0x40 > 0; s ≥ −100. Else **1e18** (non-permanent veto).
- Hero record, MagicPlan with a team, and u null: **1e18** if any active MagicPlan of this player (the AI player's mode context goal list) has a TF that already holds T (0x5fdfc0), or T is already in this PP's selection; else **0**.

(AI_GALACTIC_2 §1.4 called the first condition "auto-resolve busy"; it is "mode ≠ galactic".)

### 2.4 Hero Commit 0x7271f0: the escort accumulation is dead code [H]

For each hero record with weights, the commit calls the WeightedTypeList lookup 0x68dbd0(weights, newly selected type) (exact type → its weight; otherwise the mean weight of entries whose category masks overlap the type; else 0), but **discards the returned double**. The code then tests a local weight that is always 0.0 and skips the update of +0x3C / +0x40 / +0x44 / +0x48. Ghidra removes the block as unreachable; the disassembly confirms the callee's result in xmm0 is never read and the compared register (xmm6, callee-saved) is constant 0.

Consequences:
1. rec+0x40 stays 0 for every hero record, so **a hero record never gets a finite Hero score in a non-magic plan**. "Hero attach" by cost window never happens in the shipped game. [H code; M gameplay reading, probe H1]
2. Heroes join AI plans only:
   - as plain units in plans where **every** TF is `DenyHeroAttach` (then 0x140b3a526 = 0, named/generic heroes are not records, and they are picked through `LandHero` / `SpaceHero` category teams, e.g. LandHeroes, RaidPlan, Patrol-type plans that deny attach);
   - as non-records (no script found, or a campaign battle with no unit on hand);
   - through MagicPlans with a team (§2.3).
3. 53 of the 134 shipped plan files use `DenyHeroAttach`. In the other plans, hero records are vetoed every iteration but are not erased (the veto is non-permanent).
4. **Port rule:** run Setup B/C as specified (the Lua calls have side effects: `PlayerObject`, script globals), score as specified, and do **not** implement the accumulation. Behaviour then matches the shipped binary.

### 2.5 Outside the solver [H data, M rules]

- Galactic idle heroes are moved by the free-store Lua: `GalacticFreeStore.On_Unit_Service` moves every hero each service (`MoveUnit` → `GalacticHeroFreeStore.Find_Custom_Target`: per-hero `CustomUnitPlacement` perception, e.g. `Is_Home_Planet` for Palpatine / Tarkin / Mon Mothma, then space or ground preference) through `FreeStore.Move_Object`.
- Hero object scripts run their own `HeroService` loop (HeroPlanAttach.lua `main`) and tactical state machines (DarthVader.lua exits outside land mode).
- Release: plan end returns units to the free store (AI_GALACTIC §1.9). A plan-level "release hero" rule separate from this was not found [open item 2].

---

## 3. Population caps

### 3.1 Values [H]

- pop(T, m) as in the notation; `Space/Land_Override_Population_Value` default −1 and forced to 0 for types with TACTICAL_SUPER_WEAPON (OBJECT_TYPES §9.4).
- GameConstants (instance 0x140b15800): `Max_Ground_Forces_On_Planet` +0x10C8 (ctor default 14, shipped 10), `Allow_Reinforcement_Percentage_Normalized` +0x10CC (ctor 0.4, shipped 0.0), `AI_Uses_Galactic_Pop_Cap` +0x11EBC (ctor false, **absent from shipped XML → false**).
- Faction: `Space_Tactical_Unit_Cap` +0x2CD0 (shipped 20/25/40), `Land_Skirmish_Unit_Cap_By_Player_Count` +0x2CD8 (shipped curve 0→10, 1→10, 8→5), `Base_Population_Capacity` +0x2EE8 (not in shipped XML; ctor default not read [L: 0]).
- Type `Additional_Population_Capacity` +0x2120: planets 3–5 (PLANETS.XML), starbases, and the land markers `Reinforcement_Point_PlusN_Cap` (N = 1..5, 10) and one 100.

### 3.2 Tactical used population 0x2aba70(pop manager, player) [H]

Sum, over the player's object list in the current store (pop manager +0x548, per-player list), of the float at object +0xF0 → +0x198 (each object's population share [M: team members alive / team size]). Result = (int)sum, plus 1 when sum − floor(sum) > `Allow_Reinforcement_Percentage_Normalized`. With the shipped 0.0, **any fractional share rounds up**.

### 3.3 Caps 0x2ac5c0(pop manager, player) [H]

| Game | Land | Space | Galactic |
|---|---|---|---|
| campaign battle | the battle's **defender** (land queue head record +8): `Max_Ground_Forces_On_Planet`; any other player: min(that, player +0x404) | faction `Space_Tactical_Unit_Cap` | see §3.5 |
| skirmish | min(player +0x404, `Land_Skirmish_Unit_Cap_By_Player_Count` evaluated at the player count [M: linear interpolation of the curve]) | faction `Space_Tactical_Unit_Cap` | – |

- Player +0x404 is refreshed by 0x289010 (on object add/remove in the current mode) with the capacity 0x2ac700 of §3.5 computed in that mode: `Base_Population_Capacity` + Σ `Additional_Population_Capacity` of the player's alive objects (plus, in skirmish, those of allied players). In a land battle this is the capacity of the reinforcement-point markers the player holds. So **the attacker's land cap grows with captured reinforcement points; the defender always has the full planet cap** [H; M gameplay reading].
- Remaining = cap − max(used, 0) (0x2acb10).
- "Room for T" 0x2b03e0(pop manager, player, T): (int)(pop(T, mode) + 0.5) ≤ cap − used; with no T the test uses 1.0.

### 3.4 Where the AI checks it [H]

- Reinforcement Setup A (room for one) and Score (remaining ≥ pop(T) + committed), §1.3–1.4.
- `TF.Reinforce` spawn loop (AI_TACTICAL §7.4): last pending unit first; skip a unit whose pop(T, 1) exceeds the remaining cap (the remaining value is read once).
- Production marshaller (galactic, only when `AI_Uses_Galactic_Pop_Cap`): AI_GALACTIC_2 §1.4.
- Contrast pop allowance (0x72b3e0 / 0x72b6a0): uses pop(T, 0) and the same flag; with the flag off the allowance always permits.

### 3.5 Galactic population [H]

- **Applies to a player only if the player is human (+0x62) or `AI_Uses_Galactic_Pop_Cap` is set.** In vanilla FoC the AI has no galactic cap. The A2 stand-in "no galactic pop cap" is correct for the AI with shipped data; mods that set the flag need §3.5–3.6.
- Capacity 0x2ac700 (galactic mode): `Base_Population_Capacity` + Σ over the player's alive objects (+0x3A0 & 0x42 clear) of their type's `Additional_Population_Capacity`, plus, for a planet, the `Additional_Population_Capacity` of the type returned by 0x3f8b40 (the planet's current starbase type, planet data +0x140: once per owned planet, whoever owns the starbase, whatever its level; A2GAL_READINGS §2.2 [H]). For a non-human, objects whose nearest planet (the object or its +0x2B0 parents) is **not revealed** are skipped: 0x3fa420 is `Is_Planet_Revealed` (GALACTIC_REVEAL_CORRUPTION §1.1; local reveal state, no player argument, always true in multiplayer); objects with no planet ancestor (fleets in transit) are never skipped (A2GAL_READINGS §2.2–2.3 [H]; this line formerly read 0x3fa420 as "surface not usable", which was wrong).
- Used 0x2ac320 (production legality): round(Σ `Population_Value` over the player's top-level objects + 0.5), skipping objects inside a container whose own pop is > 0 and (non-human) objects at planets that are not revealed (0x3fa420, as above), plus, when asked, the pop of items queued at the player's production behaviours (0x401cf0). A type is producible when pop(T, 0) ≤ max(capacity − used, 0). Galactic starbases are planet types, not objects, so their `Population_Value` is never "used".
- AI remaining pop for the solver (0x5f5080; PP Reset puts it in 0x140b3a530 when the flag is on): galactic only; capacity − Σ pop of the AI's galactic free-store units (skipping units at planets that are not revealed, 0x3fa420) − Σ PP+0x1A8 over the budget's goal structs (the population committed by instantiated plans) [H; M "committed"].
- Port (refix t05, 2026-10-31): the port keeps a galactic starbase as the planet's starbase **object**, so 0x3f8b40 is `GalacticSim::starbase_type(planet)` = that object's type. Level, owner and object are set and cleared together, and a starbase object removed outside the sim (a direct store destroy) is cleared to level 0 at the next galactic update, so the port never holds a starbase level without a starbase type [port decision; the original cannot reach that state because its starbase is not an object].

---

## 4. MagicPlan: how the AI spawns units

### 4.1 Plan time (recap with corrections) [H]

- FreeStore is off for MagicPlans; Budget, Production (non-magic branch) and TacticalBuild do not charge them; Reinforcement does not apply.
- Galactic Production Score for a magic plan: veto when not spawnable (AI_GALACTIC_2 §1.4 list), else **one synced float in [0, 1] per candidate per solver iteration**. Tactical lists have no Production marshaller, so a tactical magic solve draws nothing for this.
- Shipped magic plans (8): GenerateMagicSpaceDefense(Easy/Hard), GenerateMagicGroundDefense(Easy/Hard), GenerateMagicGroundStructure, AI_Plan_ExpansionGeneric_GenerateMagicCashDrop. **All set `MagicPlanStealing = false`.** The cash drop has an empty `TaskForceRequired` TF and only calls `Give_Money(6000)`.

### 4.2 Charge [H read, M conclusion]

BUILD_TASK_LIST's magic branch charges the goal budget (Allocate 0x691950) once, guarded by +0xD0, **only if the task already has its AIPlanetBuildTask pointer (+0xC8)**. That pointer is created only by the galactic non-magic branch, so for a fresh magic task the charge is skipped: **magic units cost nothing beyond the plan's AdditionalCost** [M]. The task then enters state 0x17 (HIJACK_UNIT).

### 4.3 HIJACK_UNIT 0x648370 [H]

Task fields: +0x88 type T, +0x98 destination object, +0xA0 task force.
1. **Destination.** In a land or space battle with a parent galactic mode, the destination becomes the battle planet: the parent's space or land conflict-queue head (BATTLE_TRANSITIONS §1.1, record +0 = planet id) looked up in the parent store. Otherwise +0x98.
2. **Stealing.** If MagicPlanStealing, or T is a named hero: in the galactic store, walk the TF owner's object list and **kill silently** (Kill 0x391310 with flag 1) the first object of exactly type T that is not already at the destination (a hero is taken even from the destination) [H; M for the exact parent-chain test]. Only the first match.
3. **Cancel pending respawns.** For every galactic planet, remove pending delayed-spawn entries of type T from the planet's spawn list (planet data +0x240, the list serviced by 0x3fc750: due frame, type, player; "Successfully spawned %s at %s!") [H].
4. **Spawn.** obj = 0x400b40(destination's production behaviour, T, destination, owner) (the planet production "create object" used by finished queue items, GALACTIC_ECONOMY §2.5). Null → **FAILED (7)** → `Production_Failed`.
5. **Non-structures** (T not 0x5F / 0x60 / 0x5B / 0x5C):
   - created on a planet surface (parent with planet behaviour) and not a `Galactic_Fleet`: FAILED if the planet is blockaded (0x3fa370); else FleetManagement launch (op 4) then form fleet (op 0) by direct execution (as AI_GALACTIC_2 §5.4); the new fleet's +0x94 flag is cleared;
   - a created `Galactic_Fleet`: only the +0x94 clear.
   - **Galactic mode** (or no parent): 0x6498f0 hands the unit to the task (free store, FINISHED).
   - **Tactical with parent:** take the fleet's first member if a fleet was formed; add the unit to the player's **galactic** free store (galactic AI context → +0x50 → +0x38, vf+0x98); append its id to the parent's per-player reinforcement record (+0x20 list, +0x28 count, grown by max(10, size/10)); register the new pool index with the PP (0x6bf790) so `TF.Reinforce` can drop it. State 8.
6. **Structures / generic types:** created at the planet; state 8.

No RNG is used at run time [H for this function; L for 0x400b40 internals].

### 4.4 Port hook

The galactic sim needs a spawn operation equivalent to the planet production "create object" (same placement as a finished queue item, owner = AI player, no cost), plus "kill silently", "cancel pending spawns of a type on every planet", and "append to the battle's reinforcement record". The A2 stand-in (magic tasks fail → Production_Failed) diverges whenever a magic goal is proposed.

> **Status 2026-10-30:** `engine/ai/src/galactic_build.cpp:62-67` now handles magic tasks by calling `sim.magic_spawn` (steal, cancel pending respawns, spawn at the destination as a finished queue item), failing the build only when the spawn is refused; the "magic tasks fail" stand-in is gone.

---

## 5. Production-market planet order [H code site, M list direction]

- The AIPlanetBuildQueue market (0x53cd60) is called from **ProductionBehaviorClass::Service** (vtable 0x868be0, slot 6 = 0x402840): for the planet being serviced it runs the market for queue 0 (ground/structures) then queue 1 (space), and only then advances that planet's own production queues (head done, PAYG installments, refunds).
- ProductionBehavior is serviced inside the store's serviced-object loop (OBJECT_RUNTIME §5.3 step 2: behaviours in attach order), so **planets run their markets in serviced-list order, newest-registered first**. Planets are registered when created at campaign load, so the order is the **reverse of planet creation order** [M: list direction from OBJECT_RUNTIME].
- The A2 assumption "reverse planet-row order (GOM newest-first)" is right if the port's planet rows are in creation order. Within one frame, the planet serviced first wins bid ties (AI_GALACTIC_2 §2.3 step 4).
- Each queue keeps its own last-run frame and runs when more than 30 frames have passed (AI_GALACTIC_2 §2.3). Whether the queues of different planets stay phase-aligned was not traced [L].
- Correction for GALACTIC_ECONOMY §5.1: the step after the object loop (0x3fc750, "planet production service") is the **delayed-spawn list** (planet data +0x240), not the build queues. Build queues advance in ProductionBehavior::Service during the loop.

---

## 6. Terrain data the AI reads

### 6.1 PP terrain id PP+0x19C (PP Init 0x6c0d00) [H]

- Galactic, goal target with a planet object: the target planet type's `Terrain` string (+0x848) converted through the terrain enum (0x140b310d8, built by 0x5e8770; names Temperate, Arctic, Desert, Forest, Swamp, Volcanic, Urban…).
- Land: the land mode object's +8 field (0x5f55e0 returns the mode object of the AI player's mode) [M: the battle's terrain id].
- Space, or galactic without a target planet: −1.

### 6.2 Terrain effectiveness table 0x5f22c0(T, terrainId) [H]

- Loaded by 0x5f3160 from `Data\XML\AITerrainEffectiveness.xml`: outer key = terrain enum id (element name), inner key = `MovementClass` enum id (0x140b31028; element name), value = double.
- Lookup uses T's `MovementClass` (+0x1688), or the first `Company_Units` member's if T is a company. Missing terrain or class → **1.0**.
- **Shipped FoC data is 1.0 for all 7 terrains × 7 classes**, so the factor is always 1.0 in vanilla.

### 6.3 Uses [H]

- **TerrainType marshaller** (galactic list only, 0x727c40): with a goal target that has a planet object, **veto** if the planet type restricts T: `Planet_Restricted_Unit_Categories` (+0x808) overlaps T's category mask (+0x1648), or T is in `Planet_Restricted_Unit_Types` (+0x810) (0x373490). Otherwise **0** when PP+0x19C = −1, else the effectiveness value (vanilla **+1.0 for every candidate** on a target planet with a known terrain).
- **Contrast Setup C:** each (entry, weight) pair weight × effectiveness when PP+0x19C ≠ −1 (AI_GALACTIC_2 §1.4); vanilla ×1.0. In land battles this applies too (PP+0x19C from §6.1).
- **Tokens:** no galactic perception token reads terrain. The tactical `TimeLastSeenUnnormalized` skips land cells with terrain flags & 0x22 (AI_TACTICAL_PERCEPTION §2).

Port: A2's "TerrainType 0" differs from the original by a uniform +1.0 per candidate on targets with a terrain (no effect on argmax or the 50 % rule) and by the restriction veto (which does matter). Implement the veto and the +effectiveness.

> **Status 2026-10-30:** The TerrainType marshaller is implemented: `TerrainM` (`engine/ai/src/galactic_pp.cpp:759-770`) vetoes planet-restricted types via `planet_restricts` and otherwise returns 0 when the terrain id is -1, else the terrain effectiveness.

---

## 7. Perception equations: failure semantics and RNG

### 7.1 `PerceptionFunctionClass::Evaluate` (PerceptionFunctionG.dll 0x17650) [H]

- First calls the exe's pre-evaluation callback (Init_Perception_DLL argument 4 = 0x4e0160 → 0x5f11a0; §7.3), then executes the compiled node list in order on a stack of doubles. Node classes: Constant, PerceptualLookup, Addition, Subtraction, Multiply, Division, Equal, NotEqual, Less, LessOrEqual, Greater, GreaterOrEqual, Unary minus, Clamp, RandomNumber.
- **The first node that returns false aborts the evaluation: Evaluate returns false and does not write the output.** Later nodes (including lookups and `#`) are not evaluated.
- After the last node, success requires exactly one value on the stack; then the output is written and true returned. Any other stack size → false, output untouched.
- No operator swallows a failure. Node failure cases:
  - every operator: stack underflow → false;
  - PerceptualLookup: pops its bound parameter values, calls the exe evaluate callback (0x4dd340); callback false → false (the leaf failure of AI_GALACTIC_TOKENS: unregistered path, unbound root, Self-only leaf, …);
  - **Division a / b:** a == 0 → result 0 (success, even when b == 0); else b == 0 → **false** (the first time per process the DLL shows a modal MessageBox "Divide By Zero error while evaluating perceptual function!", guarded by a one-shot flag); else a / b;
  - **Clamp(x, lo, hi):** lo > hi → **false**; else lo if lo > x, else min(hi, x) [H arithmetic; M argument naming];
  - comparisons push 1.0 / 0.0; Multiply has no zero short-circuit (both operands are always evaluated before it, postfix order).
- Equation lookups and `Function_` references therefore always evaluate both operands of `*`; a failing leaf anywhere fails the whole equation, which is the S2 rule.

### 7.2 Exe-side consequences [H unless tagged]

- Proposal, template, budget and FindTarget sites: as AI_GALACTIC_TOKENS §5 (skip candidate; template result keeps its pre-set 0; budget weight keeps its pre-loaded previous value). These are now confirmed: the DLL never writes the output on failure.
- **Lua `EvaluatePerception`** (0x699430): on failure it raises the script error helper with "Evaluate_Perception: Error evaluating perception function %s." and **returns no value**; the helper does not unwind (SCRIPT_GAPS §0), so the script receives **nil**. Same for the argument errors and "players with no AI"; "AI not yet initialized" is a warning, also no value.
- In `GalacticAttackAllowed` (PGTaskForce.lua) with no human: `num_land_enemy_controlled` and `num_space_enemy_controlled` become nil; on first use they are stored with `GlobalValue.Set(…, nil)` and later used in arithmetic (`… - recent_…`), which is a Lua runtime error inside the plan thread [H script text; M that the thread then dies and the plan ends without the sleep]. Probe P3.

### 7.3 The `#` operator does not use the synced RNG [H]

- RandomNumber node (DLL 0x178f0): pops b (right) and a (left), converts both to float, calls the exe callback 0x5f1150 and pushes the result as double.
- 0x5f1150: lo = min(a, b), hi = max(a, b); state(0x140a441ac) = state × 1103515245 + 3039 (32-bit wrap); v = (state >> 10) & 0x7FFF; result = (float)v / 32767.0f × (hi − lo) + lo (single precision).
- **Before every equation evaluation** the callback 0x5f11a0 reseeds that state: seed = game seed (0x140b154c0, the same value that seeds the synced LCG 0x140a13e24 at game start) + (player id × 131 + 0x123456FB) + CRC-32(equation name) + CRC-32(target name string, target vf+0x50) (absent parts count 0; CRC-32 = 0x215a30, standard reflected table at 0x140a14d20, init and final inversion) [H; M that vf+0x50 is the target's name].
- Consequences:
  - `#` never moves the synced RNG and costs **0 synced draws**;
  - the same (game seed, player, equation, target) yields the **same `#` values every evaluation for the whole game** (e.g. `Game.Age > (20 # 40)` is a fixed threshold per player and target; `0#1 > 0.5` is a fixed coin per target);
  - a `Function_X` reference evaluated inside an equation re-enters Evaluate and reseeds the shared state, so `#` nodes after it in the outer equation continue from the inner seed [M];
  - only tactical equations use `#` in shipped data (AI_Equations_ExpansionGeneric_LandSkirmish, BasicLandEquations, OffensiveSpaceEquations; 7 uses). No galactic equation uses it.
- **This corrects AI_GALACTIC §1.13, AI_TACTICAL §12 and the S2/A2 implementation** (`perception_equations.hpp`: "'#' draws SimRng::range_float from the synced RNG"; `lua/players.hpp`).

### 7.4 Synced draws in one galactic proposal pass (static count) [H formula, probe for numbers]

In cursor order, per AI player:
1. Find_Plan_For_Goal: 1 float per candidate with desire > 0 that passes slot 9 and has ≥ 1 candidate plan (AI_GALACTIC §1.13).
2. Each PP solve run during the pass (AI_GALACTIC_2 §1.5.4):
   - Plan Setup C: per candidate with ≥ 2 teams, rejection sampling on mask = next power of two − 1 (≥ 1 draw; expected < 2); 0 for 0 or 1 team;
   - Contrast, IgnoreTarget plans only: 1 float per surviving candidate per iteration, in candidate order, until the solve ends;
   - Production, MagicPlans only (galactic): 1 float per surviving candidate per iteration.
3. Equations: **0** (§7.3). `Script_AnyFreeLandSlots` (desire `Allow_Blind_Ground_Production`): one weighted pick per `FindTarget.Reachable_Target` call it makes.
4. Hero, Reinforcement, TacticalBuild, TerrainType, Survival, Budget, FreeStore, TechTree: 0.

Static analysis cannot give the per-frame total: it depends on the candidate population, how many plans are solved per proposal and data-dependent rejection loops. Probe P1.

---

## 8. Other A2 open items answered by the binary

| Item (PHASE4 A2 open list) | Original | Tag |
|---|---|---|
| Level factor K (0x140b16c8c) | GameConstants +0x148C, ctor **1.0**, no XML tag parses it (the shipped tag `AITechLevelProductionTimeWeight` is not in the exe's strings) → factor = 1 + (base level − required level) × **1.0** in both the ETA (0x6639f0) and the market bid (0x695c90). A2 uses 0 | H |
| Free-store health threshold 0x140b27338 | GameConstants +0x11B38 = `Health_Low_Percent_Threshold`, shipped **0.33**: a unit is eligible when its health fraction ≥ 0.33 | H |
| Flush_Category 0x612cb0 | finds the category (budget +0x40 map) and sets its **available A (+0x44) = 0**; nothing else | H |
| Flush_All_Resources 0x6bcdf0 | goal struct alloc (+0x2C) −= min(goal cost, alloc) (0x614840), then req (+0x28) = 0 (0x612d60) | H |
| Budget tick period T (0x4b2010) | GameConstants +0x180 = `Fiscal_Cycle_Time_In_Secs` | H |
| PAYG reclaim r (0x5f7d40(plan, amount)) | over the AI free store's build-task list, tasks of this plan whose production has started (task +0x78 ≠ −1) with remaining cost in (0, amount]; sorted by creation time ascending, ties by AI_Combat_Power descending; taken greedily while cost ≤ the remaining amount; each taken task gets +0xD9 = 1; r = the sum. The comparator computes both ages from two separate `timeGetTime()` calls (wall clock), an original determinism hazard; port: sort by task +0x2C ascending, then power descending | H mechanism; M meanings |
| Production Setup C "travel estimate" 0x664210 | **not a travel time**: the mean, over the target reachability planet list for the level (§8 next row) of planets with a usable surface, a production queue and able to build T (0x2804d0), of btime(T, planet) + that planet's queue time (AIPlanetBuildQueue 0x53bb20, or the production behaviour's remaining time); no planet → 1e18 | H |
| Source-planet list 0x665160(reach, level) | levels 0..5; cached per level, recomputed when forced or when more than 10 × FPS frames (10 s) old; the planets returned by the reachability flood 0x3f12e0 from the reachability object's player, with level flags (5 = any; 1 or 5; > 1; > 2; 4 = extra mode), in flood output order, mapped to the reachability's planet table | H; M flag meanings |
| Market order | §5 | H/M |
| Galactic pop cap | §3.5 (none for the AI in vanilla) | H |
| TerrainType | §6 | H |
| MagicPlan spawn | §4 | H |
| Hero marshaller | §2 (attach never succeeds; records vetoed) | H |
| Type flags +0x67 / +0x29 / +0x21 | +0x67 = `Create_Team`, +0x21 = `Cinematic_Object_Only`; +0x29 = "is a squadron member" (set on `Squadron_Units` members by 0x377500, like +0x2A for company members) → STRATEGIC_GAPS §1.2 | H |

---

## 9. Corrections to earlier documents

1. **AI_GALACTIC §1.13, AI_TACTICAL §12, S2/A2/L1 code:** the perception `#` operator uses its own LCG reseeded before every equation evaluation; it never draws from the synced stream (§7.3).
2. **AI_GALACTIC_2 §1.4 Hero Score:** the "auto-resolve busy" condition is "current mode ≠ galactic"; and the Commit's escort accumulation is dead code (§2.4), so hero attach never succeeds.
3. **AI_GALACTIC_2 §2.3 MagicPlan:** the budget charge needs the planet build task pointer, so magic tasks are not charged [M]; HIJACK also cancels pending respawns of the type and, in tactical, feeds the battle's reinforcement record (§4.3).
4. **AI_GALACTIC_2 §1.4 Production Setup C:** 0x664210 is a mean (build time + queue time), not travel (§8).
5. **A2 port:** K = 1.0, not 0 (§8).
6. **GALACTIC_ECONOMY §5.1:** 0x3fc750 is the delayed-spawn service; build queues and the AI market run in ProductionBehavior::Service during the object loop (§5).
7. **AI_GALACTIC_TOKENS open item 1 / AI_GALACTIC_2 open item 1:** closed: failure leaves the output untouched and propagates (§7.1).
8. **§3.5 here, AI_GALACTIC_2 §2.1 ("surface usable"), AI_GALACTIC §2.8 ("restricted planet"):** 0x3fa420 is `Is_Planet_Revealed`, not a surface test (A2GAL_READINGS §2.3, 2026-10-31; all three corrected in place).

---

## 10. Open items and in-game probes

Open (static analysis did not settle):
1. Land PP+0x19C source (land mode +8) meaning [M].
2. Hero release rules outside plan end; who reserves free-store pool slots (+0x88) and pads (+0xE8 entry +0x20).
3. Free store +0x120 = builder objects [M]. (Type flag +0x29: answered, squadron member — STRATEGIC_GAPS §1.2. Survival
   0x588a90, buildability 0x2804d0, the 0x375270 veto ("Super" category) and company transports: STRATEGIC_GAPS §1.1,
   §1.3, §1.5, §1.7.)
4. Lua thread fate after a runtime error in a plan (affects `GalacticAttackAllowed` without a human).
5. `Land_Skirmish_Unit_Cap_By_Player_Count` evaluation (interpolation vs step) and `Base_Population_Capacity` default.
6. Object +0xF0 → +0x198 population share meaning.

In-game probes (prepare under `tools/probe_mod/` later; **do not launch the game**):

| Id | Question | Setup | Observe |
|---|---|---|---|
| P1 | Synced draws per galactic proposal pass (AI_GALACTIC_2 open item 8) | AI-vs-AI galactic sandbox, no human. Lua cannot read the synced LCG state, so this needs a debugger hit counter (Gabriel attaches) on the synced draw helpers 0x1ffb40 / 0x532480 / 0x5324d0, bucketed per frame | draws per frame on proposal frames vs other frames; compare with §7.4 |
| P2 | `#` determinism | Land skirmish vs AI; a probe Lua plan calls `EvaluatePerception("Unit_Needs_To_Be_Bombed", ai_player, same_target)` every few seconds and prints the result | the value is constant while its non-`#` inputs are constant, survives save/load, and differs between targets |
| P3 | `EvaluatePerception` returning nil in `GalacticAttackAllowed` with no human | Galactic AI-vs-AI; a story Lua that calls `EvaluatePerception("Num_Enemy_Land_Territories", ai_player)` and prints `type(result)` | expect `nil`; then watch whether ConquerOpponentPlan threads die after their first conflict (plan count drops, no `DifficultyBasedMinPause` sleep) |
| H1 | Hero attach never succeeds | Galactic vs AI with an affordable AI hero (Vader) idle in the free store and several conquest plans (no `DenyHeroAttach`) | Vader never joins an AI conquest TF through plan instantiation; he only moves by the free-store placement (home/space preference). **Refined 2026-11-12 (`A2M_R2_RE_B.md` §3):** Vader's script exits in galactic, so he is a plain unit and may be picked like any unit. Use a hero whose script survives galactic (BobaFettPlan: Boba Fett, Tyber Zann) to test the veto. The retail Q2 traces already show HOME_ONE and SUNDERED_HEART in a Rebel Conquer_To_Reconnect TF (a2m r1). |
| R1 | Reinforcement score and the 300 rule | Land skirmish, AI with bought reinforcements and one idle unit of the same type near its base | the AI drops reinforcements for attack plans instead of walking the idle unit, unless that unit is at the stage |
| R2 | Campaign attacker land cap | Campaign land battle as defender vs AI attacker; count AI units on the map before and after the AI captures reinforcement points | AI unit count ≤ min(10, Σ captured point capacity); defender limited by 10 |
| M1 | MagicPlan spawn and cost | Galactic vs AI on Hard with a weakly defended AI planet (triggers `Needs_Magic_*_Defense`) | units appear at the planet without production, AI credits unchanged except AdditionalCost; a dead AI hero's pending respawn is unaffected (no magic plan lists heroes) |
| K1 | Level factor K = 1 | Galactic AI with two planets of different base levels able to build the same unit | the AI builds at the planet with the lower (queue + build + travel) × (1 + Δlevel) bid; with equal queues it prefers the lower-level-surplus planet |

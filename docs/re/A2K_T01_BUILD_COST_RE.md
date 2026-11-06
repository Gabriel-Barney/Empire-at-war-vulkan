# A2k t01: galactic build cost, build time and the AI's cost view (RE lane 3, 2026-11-06)

Source: the decompiled retail exe (Ghidra output), the FoC XML (`extracted/foc_data_text.zip`) and the retail probe
`tools/probe_mod/a2f_budget/out/20261106-074145` (Heart of the Maw, Rebel human, Medium; `A2J_T02_T04_PROBE_RESULTS.md` §2).
This file is behaviour only. Confidence: [H] read in the code, or the code and the probe agree to the frame; [M] inferred;
[L] hypothesis.

**Summary.**
- **Broadside @Corellia costs 1350 because the Emperor is at Corellia** [H]. `Emperor_Palpatine` has
  `Emperor_Reduce_Production_Price` (Applicable_Unit_Categories `All`, 0.25, not Galaxy_Wide). In HotM the story spawns
  him, with Tarkin, Veers, Boba Fett and the Accuser, at Corellia on day 1 (GALAXY_MAP_PRESENTATION §7.2). Retail
  keeps this discount in a per-type modifier table on the planet. The port has one unused per-planet float.
- **Tartan @Kuat takes 9 s because the base-vs-tech table index is d − 1, not d** [H]. Here d = starbase level − Tech_Level.
  Tartan has Tech_Level 0 and Kuat has starbase level 3, so the time is 15 × `_2` (0.75) × the AI's Normal multiplier
  0.8 = 9.0 s. The port uses d as the index, so it reads `_3` (0.55) and gets 6.6 s. The factor also applies **only to
  space-queue items**, never to ground units.
- **The AI's plan cost is always the base `Build_Cost_Credits`** [H]. This covers the PP net cost and the goal cost
  +0x78. The **market bid** uses the planet's price and time. The **installments** charge the planet price stored on
  the queue entry. So in retail the goal costs 3050 but spends 2600, and 505 is left in the Offensive pool. The probe
  shows exactly that.

---

## 1. Credit cost of a unit at a planet

### 1.1 Order of operations [H]

`Get_Cost(type, planet)` is **0x3711c0**:
1. **No planet** (`planet == null`, or the planet has no behaviour 6): return type `Build_Cost_Credits` (+0x86c) as it is.
   There is no rounding and no modifier. This is `cost(T, none)` everywhere in the AI.
2. **With a planet**, raw = **0x400240**(type, planet) =
   `m_planet_cost × float(Build_Cost_Credits) × m_corr_price × m_techupg`, float32, multiplied left to right:
   - `m_planet_cost` = **0x559c10**(planet +0xd0, planet, type), see §1.2. It is 1.0 when the planet has no production
     behaviour (+0xd0 = 0);
   - `m_corr_price` = 0x33e3e0(faction, planet corrupted 0x3a0910): `Corruption_Galactic_Production_Price_Multipliers`
     [corrupted ? 1 : 0] (faction +0x2ff0). It applies only if the owner faction's `Benefits_From_Corruption`
     (+0x10b) is 0, else 1.0;
   - `m_techupg` = 0x281960(player). It applies only when the type's `Build_Advances_Tech_Level` (+0x894) ≠ 0,
     else 1.0.
3. If raw > 0, raw = max(1, raw). Cost = 0x22f9c0(raw), round half away from zero, int.

At enqueue, **0x3fff70** stores this int, as a float, in the entry's cost field (+0x28). It stores the build time
(§2) in +0x10, arms the timer and sets +0x1c to that time. A pay-as-you-go player (every AI) is then charged per frame
`a = (prev_remaining − remaining) / T × C` (**0x498ec0**, entry +0x1c, +0x10, +0x28). So the total charge over a build is
exactly the stored C, at a flat rate of C / (30 T) per frame [H]. **The price is frozen at enqueue.** Nothing
recomputes +0x28 or +0x10 later, and a queued entry that becomes head (0x4033c0) only re-arms its timer from +0x10.

### 1.2 The planet price modifier 0x559c10 [H]

Inputs: the planet production behaviour B (planet +0xd0), the planet P, the type T.
1. **Player-wide table:** the owner player of P (0x3956c0 → player list), then 0x2817e0(player, T). This is a hash
   map at player +0x238..+0x260, keyed by the type pointer (hash `((ptr ^ 0xdeadbeef) & 0x7fffffff)` Park–Miller
   step, masked). Expired entries are purged first (0x497bd0).
2. **Planet-local table:** the same kind of hash map at B +0x88..+0xb8, keyed by T. Expired entries are purged. If
   the record is now empty (0x282450), it is erased.
3. Merge both records (0x536330) into one ordered map, **keyed by stacking category** (int). Each category holds a
   list of 0x30-byte modifier entries {value float +0, expiry frame +4, name hash +8, source handle +0x10}. The
   records' optional lower bound (+0x10, flag +0x18) is merged as max, and the optional upper bound (+0x14, flag +0x19)
   as min.
4. bonus = Σ over categories, in ascending key order, of (the **largest** value in that category). It is a float fold
   with `Plus<float>` (the first category initialises it). So abilities in the **same** stacking category do not
   stack (max), and abilities in different categories add.
5. If the lower-bound flag is set, bonus = max(bonus, lower). If the upper-bound flag is set and upper ≤ bonus and
   upper ≠ bonus, bonus = upper. A new planet-local record (0x559950) is created with upper = 0x3f7fffef (≈0.999999)
   and its flag set. Then, if bonus ≥ 1.0, bonus = 0.999999.
6. Return **1 − bonus**.

The build-time modifier 0x55a010 is the same algorithm on the time tables: player 0x281890, planet B +0x48..+0x78.

### 1.3 Who writes the tables: `Reduce_Production_Price_Ability` [H for the code, M for the activation trigger]

`ReduceProductionPriceAbilityClass` (vftable set in 0x70a280; name getter 0x70a900):
- Its fields are `Price_Reduction_Percentage` +0x178 (float, must be < 1.0, else set to 1.0 with an error, 0x70af70),
  `Stacking_Category` +0x17c (int) and `Galaxy_Wide` +0x180 (bool). The only allowed `Activation_Style` is
  `Galactic_Automatic` (3); anything else is forced to 3 with an error.
- Apply (vf 0x70a340 → 0x70a3e0 / 0x70a640):
  - **Galaxy_Wide** and the holder is a unit (holder +0x335 ≠ −1) → **0x70a640**: the table record is the player's
    0x280cc0(owner player, T), get-or-create on player +0x238. It logs "%s has APPLIED its %s effect for object type
    %s to player %S.";
  - otherwise → **0x70a3e0**: the record is the **planet-local** one 0x559950(planet +0xd0, T), get-or-create on
    B +0x88. This needs the planet to have a production behaviour and the holder's planet data not to be in the
    destroyed state (+0xb8 → +0x2c8 = 0).
  - For every applicable type T, from 0x48d070 (`Applicable_Unit_Categories` / `_Types` minus `Excluded_Unit_Types`;
    `All` = every category [M]), with T's base cost > 0: 0x38c850(record, &pct, &Stacking_Category, source, name,
    −1.0). This finds or creates the stacking-category bucket and appends {pct, expiry = −1 (never; −1 as uint is
    never < now), name hash, source}.
- Remove: vf +0xf0 / +0xf8 through 0x70aa20 / 0x70a990, by source [M; not traced entry by entry].
- **When** Apply / Remove run for a hero (on arrival or creation at a planet, on leaving): from the
  Galactic_Automatic ability framework (0x48c780 base) [M]. The probe below fits "applied while the holder is at the
  planet, from the moment it is created there".
- `PLANETS.XML` planet abilities (for example Corellia's `Corellia_Corellian_Corvette_Production_Boost`, 0.20, stacking
  category 9) use the same class. They write the same planet table, from the planet object itself [M].

### 1.4 Why Broadside @Corellia is 1350 [H]

- Data:
  - `Emperor_Palpatine` (NAMEDHEROUNITS.XML): `Emperor_Reduce_Production_Price`, `All`, 0.25, Stacking_Category 0,
    not Galaxy_Wide;
  - `Sandbox_Heart_Of_The_Maw_Rebel` loads `Story_Plots_Empire_AI_Heroes_EXP.xml`. Its `Empire_AI_Heroes_02` spawns
    `Emperor_Palpatine_Team` at "Coruscant" (prereq `Universal_Story_Start`);
  - Coruscant is not on the HotM map, so the reward uses the first planet the Empire owns, which is Corellia
    (GALAXY_MAP_PRESENTATION §7.1–7.2: all five day-1 heroes at Corellia).
- So at Corellia, m_planet_cost = 1 − 0.25 = 0.75 for every Empire type. **Broadside: 0.75 × 1800 = 1350.**
- Probe check (Empire credits, `samples.tsv`):
  - frames 104..119: −6.527 / frame. That is the Turbolaser's 4000 / 1440 = 2.778 plus 1350 / 360 = 3.750
    (Broadside, T = 12 s, §2.4);
  - 182 → 244: −11.157 / frame. 4.630 of that is Tartan's 1250 / 270 (no discount at Kuat);
  - 430 → 492 matches the ends at 410 (Tartan) and 463 (Broadside) to 1%.
  - Charged: Broadside 3.75 × 361 = 1354 (≈1350, one extra frame of rounding), Tartan 1250, Turbolaser @Tatooine 4000
    (no discount; the Emperor is not there).
- **Offensive pool:** 3105.34 at 101; 2606.26 at 182 (= 3105.34 − 80 × 3.75 − 43 × 4.63); **505.33** at 492, when both
  ships are done. That is 3105.34 − 1350 − 1250. The installments are debited from the goal's Offensive budget at the
  planet price [H].
- The probe's `cost` column (1800 / 1250) is Lua `Get_Build_Cost` on the type wrapper. That is the base cost (§1.1 step
  1), not the charged price [M: the wrapper calls 0x3711c0 with no planet].
- Also discounted at Corellia while the Emperor is there:
  - every other Empire build there (Stormtrooper 60 → 45);
  - Veers adds Heavy_Scout / Heavy_Assault / Anti_Aircraft 0.25, in the **same** category 0. So it does **not** stack
    with the Emperor (max = 0.25);
  - Tarkin's `E_Ground_Research_Facility` is also category 0 (max = 0.25).

## 2. Build time of a unit at a planet

### 2.1 Order of operations, 0x400370 [H]

`Get_Build_Time(type, planet)` is 0x371230. With no planet it returns float(uint32(float(`Build_Time_Seconds`
+0x890) × `Production_Speed_Factor` (GameConstants +0x120))), the "T0". With a planet it calls 0x400370:
1. Owner player +0x3c0 (cheat) → 5.0.
2. T0 = 0x371230(type, null).
3. m_planet = 0x55a010(planet +0xd0, planet, type) (§1.2 on the time tables), 1.0 without a production behaviour.
4. **m_tech** applies only when **all** of these hold:
   - (cost < 1, or (`Required_Ground_Base_Level` +0xf0c = 0 and not behaviour 0x5f));
   - 0x282400(type): cost ≥ 1 and (`Required_Star_Base_Level` +0xf10 ≠ 0, or behaviour 0x5c with `Base_Level` +0x8a0
     = 1);
   - the planet data pack (+0xb8) exists and its starbase level L (+0x148) is in 1..5.

   In effect m_tech applies **only to space-queue items** (ships and starbases), never to ground units or ground
   structures. If it applies, let d = L − `Tech_Level` (+0x89c):

   | d | factor | GameConstants | vanilla |
   |---|---|---|---|
   | ≤ 0 | 1.0 | – | 1.0 |
   | 1 | `Production_Speed_Mod_Base_Vs_Tech_0` | +0x15d4 (0x140b16dd4) | 1.00 |
   | 2 | `_1` | +0x15d8 | 0.90 |
   | 3 | `_2` | +0x15dc | 0.75 |
   | 4 | `_3` | +0x15e0 | 0.55 |
   | ≥ 5 | `_4` | +0x15e4 | 0.45 |

   The GameConstants base is 0x140b15800: `Max_Build_Queue` +0x11778 is at 0x140b26f78 and `Production_Speed_Factor`
   +0x120 at 0x140b15920. The vanilla XML comment says the same: "difference (base level 1-5 minus tech level 0-5 - 1)".
   **GALACTIC_ECONOMY §2.3 had this off by one** (d = 1 → `_1`); corrected there.
5. n = max(1, number of structures of the type's `Required_Special_Structures` on the planet (0x4b8170)), only if
   `Build_Time_Reduced_By_Multiple_Factories` (+0xc4).
6. m_ai = difficulty `Galactic_Build_Time_Multiplier` (adjustment +0x28) for an AI-controlled (0x2814d0), non-human
   (+0x62 = 0) owner, else 1. Normal_Default is 0.8.
7. m_corr = 0x33e410(faction, corrupted): `Corruption_Galactic_Production_Time_Multipliers`[c], only if the faction
   does not benefit from corruption.
8. **T = (m_planet × T0 × m_tech × m_ai × m_corr) / float(n)**, float32, left to right.

The timer is int(30 × T) frames (0x4ad3a0). The installment base is T itself (+0x10).

### 2.2 Why Tartan @Kuat is 9 s [H]

Inputs: Tartan has `Build_Time_Seconds` 15 and Tech_Level 0. Kuat has `Empire_Star_Base_3` (L = 3), so d = 3 and the
factor is `_2` = 0.75. With the AI Normal multiplier: 15 × 0.75 × 0.8 = **9.0 s** = 270 frames. Retail ran from 139 to
410 (271 frames, one frame of event lag as for every other item).

### 2.3 The four cases (AI Normal, HotM day 1, Emperor at Corellia)

| unit @ planet | L | d | m_tech | retail T | retail C | port T | port C |
|---|---|---|---|---|---|---|---|
| Broadside (tech 2) @Corellia | 2 | 0 | 1.0 | **12.0 s** | **1350** | 12.0 | 1800 |
| Broadside @Kuat | 3 | 1 | `_0` 1.0 | 12.0 | 1800 | 10.8 (`_1`) | 1800 |
| Tartan (tech 0) @Corellia | 2 | 2 | `_1` 0.90 | 10.8 | 938 (937.5 rounded) | 9.0 (`_2`) | 1250 |
| Tartan @Kuat | 3 | 3 | `_2` 0.75 | **9.0** | 1250 | 6.6 (`_3`) | 1250 |

Retail checks against the probe [H]:
- Broadside @Corellia ran 102 → 463 = 361 frames;
- ground units show m_tech is skipped:
  - Heavy_Scout @Kuat (tech 1, L 3, d 2) ran 612 → 1573 = 961 frames = 32 s = 40 × 0.8, **no** factor;
  - the port would give 24 s (40 × 0.75 × 0.8);
- Stormtroopers ran 361 frames = 15 × 0.8.

## 3. The AI's cost view and the planet choice

### 3.1 Plan cost (PP gross / net, goal +0x78) uses the base price [H]

- Budget marshaller Commit **0x7284f0**: unless MagicPlan, PP +0x40 += float(0x3711c0(T, **null**)) and PP +0x44 += the
  same, i.e. the base `Build_Cost_Credits`.
- FreeStore Commit **0x728fd0**: PP +0x44 −= 0x3711c0(T, null) for a free-store unit (galactic, PP +0x198 = 0).
- Re-solve **0x6c4a50** (at accept) → 0x6c3da0, then goal +0x7C = PP +0x48 (ETA) and goal +0x78 = PP +0x44 (net cost);
  init 0x6c0d00 does the same at proposal. So the **goal cost is the sum of base costs of the units to be produced**,
  whatever planet later builds them. For the D3 Offensive goal: 1800 + 1250 = **3050**, against a pool of 3105.
- The funds time of a build task (0x6490d0) is the budget's time to afford **0x3711c0(T, null)**, the base cost too.
- **Budget Score 0x728330** (the veto `available < c + PP +0x44`) [H]. The branch is the **opposite** of what
  AI_GALACTIC_2 §1.4 said:
  - when ETA (PP +0x48) = 0 **and** (no target, or T has **none** of the behaviours 0x60 / 0x5f / 0x5c / 0x5b, i.e.
    any ordinary unit): t = 0.5 × btime(T, none), c = cost(T, none);
  - when ETA = 0 and there is a target and T is a structure / generic type: c = cost(T, target planet),
    t = btime(T, target planet);
  - then t is clamped to the TF Time_Limit when 0 ≤ TL ≤ t, and available = PP +0x38 + t × PP +0x34;
  - when ETA ≠ 0: available = PP +0x38, c = cost(T, none).

  So for ships the Budget veto never sees a planet price. AI_GALACTIC_2 §1.4 is corrected.
- **Spending** goes through the queue entry, not the plan. The per-frame installment (§1.1) is paid through
  0x53b900 → the build task's goal budget (0x64adb0), with C = the planet price frozen at enqueue. So a goal whose
  units are built at a discounted planet **underspends its cost**. In D3 retail it spent 2600 of 3050, and 505 of the
  3105 pool was left over, so that money is free for the next goal (the ground units from 612) [H for the numbers, M
  for "this is what frees the ground units"].

### 3.2 Which planet the market picks [H for the formula, M for this case]

The bid of an unbound AIPlanetBuildTask at planet P is 0x695c90 (AI_GALACTIC_2 §2.3; lower wins; ties keep the
current choice):

`bid = (queueTime(P) + btime(T, P) + travel) × (1 + (L(P) − requiredLevel) × K) × (cost(T, P) / cost(T, none))`

- queueTime is 0x53bb20(queue, 0, 0), the queue's cached time (+0x50);
- btime and cost are §2 and §1 **with** the planet (so with m_tech, m_ai and the Emperor discount);
- travel is |pos(destination) − pos(P)| / (type +0x648 × faction +0x3b0 × 30), only when the task has a destination
  and speed > 0;
- K = 1.0 (GameConstants +0x148c; AI_GALACTIC_3 §8).

Retail bids with queueTime 0 and travel ignored:
- Broadside: Corellia 12.0 × 1 × 0.75 = **9.0**; Kuat 12.0 × 2 × 1 = 24.0. Broadside goes to Corellia, with the
  discount making it decisive, as seen at 102.
- Tartan: Corellia 10.8 × 1 × 0.7504 = 8.10 on an idle queue; Kuat 9.0 × 2 = 18.0.
- Tartan bid again at 139, with Broadside at the head of the Corellia queue (queueTime 12 s at most, 10.8 s by 139):
  Corellia ≈ (10.8 + 10.8) × 0.75 = 16.2 against Kuat 18.0, before travel.
  - So retail's Tartan @Kuat needs one of two things: a Corellia queueTime above about 13.2 s (for example the
    funding-time extension of the head for a PAYG owner, A2J_RE_BUILD_MARKET §S1), or a travel term that favours Kuat.
  - [M]: the formula is certain; the numbers for this case need the task's destination and the queue's +0x50.

Port bids with the same inputs:
- the port's prices have no discount and its times are off by one table row;
- Tartan: Corellia 9.0, Kuat 6.6 × 2 = 13.2. Broadside: Corellia 12.0, Kuat 10.8 × 2 = 21.6;
- if Tartan bids first and takes Corellia, Broadside sees Corellia ≈ 9 + 12 = 21 against Kuat 21.6;
- that is near a tie the travel term can flip, which fits the port's Tartan @Corellia / Broadside @Kuat [M].

With both fixes the port reproduces retail's per-unit values. Without the discount, Broadside @Corellia (12) still beats
Kuat (24), but the 450 credits saved and the Corellia preference for both ships both depend on the Emperor's table entry.

---

## 4. Port changes implied (file / function level, no code)

1. **Base-vs-tech index** [H]: `econ::tech_time_factor` (`engine/galactic/src/economy.cpp`).
   - Index the table with d − 1: d ≤ 0 → 1.0; d = 1..4 → table[d − 1]; d ≥ 5 → table[4].
   - Update `engine/galactic/tests/test_economy.cpp` (its (3,1) → 0.75 check becomes 0.90) and the comment
     `economy.hpp:56` ("_0 is never read" is wrong: `_0` is read for d = 1).
2. **m_tech applicability** [H]: `GalacticSim::build_time_for` (`engine/galactic/src/campaign.cpp`).
   - `tech_factor_applies` must be "space-queue item": cost ≥ 1, `Required_Ground_Base_Level` = 0, not behaviour 0x5f,
     and (`Required_Star_Base_Level` ≠ 0 or a level-1 starbase type).
   - The current test is `build_cost >= 1 && kind != GroundStructure`, which wrongly speeds up ground units at planets
     with a starbase.
3. **Per-type production modifier tables** [H for the arithmetic, M for activation].
   - Replace `PlanetState::build_cost_bonus` / `build_time_bonus` (`engine/galactic/include/alamo/galactic/campaign.hpp`,
     never written today) with per-type tables per planet and per player:
     - each table maps stacking category to a list of {value, expiry, source};
     - the lookup sums the per-category max over the merged player and planet records, with the bounds and the
       0.999999 cap of §1.2.
   - Feed the tables from `Reduce_Production_Price_Ability` (and `Reduce_Production_Time_Ability` for the time tables)
     on Galactic_Automatic holders:
     - hero units present at the planet (planet table, or the player table when `Galaxy_Wide`);
     - planet abilities in PLANETS.XML (planet table);
     - add on create / arrival, remove on leave / death.
   - `GalacticSim::build_cost_for` and `build_time_for` then use `1 − bonus(type)`. Affected: the galactic module
     (tables, lookups, save); the ability-activation hook in whichever module owns galactic hero abilities.
   - Price is frozen on the entry at enqueue, which the port already does (`campaign.cpp` around 696/712).
4. **AI Budget Score branch** [H]: `BudgetM::score` (`engine/ai/src/galactic_pp.cpp`, around line 381).
   - Swap the branch: use `cost_none` and `0.5 × btime_none` for ordinary units (or with no target).
   - Use `cost_at` / `btime_at` at the target planet only for structure / generic types (0x60/0x5f/0x5c/0x5b) with a
     target.
5. **No change needed** [H]:
   - PP net / gross cost (`cost_none` in `galactic_pp.cpp`) and the goal cost;
   - funds time (`galactic_build.cpp` `cost_none`);
   - the market bid formula (`galactic_build.cpp:427-428`). Once items 1–3 land it already uses `cost_at` / `btime_at`
     with the planet.

## 5. Open, and what would settle it

1. **Emperor position at frame 102** [M; H from the price arithmetic]. A one-line probe settles it:
   `Find_First_Object("Emperor_Palpatine")` → `Get_Planet_Location()` at frame 100, and a check that his table entry
   exists. The credit series of the 666 → 1027 Stormtrooper @Corellia build also tests it offline: 45 / 360 = 0.125
   per frame with the discount, 60 / 360 = 0.167 without (not separated here because three builds overlap).
2. **Activation and removal triggers** of Galactic_Automatic production abilities (arrival, departure, death, owner
   change; planet abilities on owner change) [M]. Settle by tracing the 0x48c780 framework callers of vf +0xf0 / +0xf8,
   or by a probe that moves the Emperor off Corellia and enqueues there.
3. **`All` category coverage** in 0x48d070 (does it include starbases, structures, tech upgrades?) [M]. Read 0x48d070.
4. **Tartan's planet in retail** (§3.2): needs the build task destination (+0x30) and the Corellia queue's +0x50 at 139.
   Settle with the port's market trace after fixes 1–3 against retail's 139 Begin, or a retail probe of the queue time.
5. Lua `Get_Build_Cost` on a type wrapper returning the base cost [M]. Its wrapper function was not opened.

# Galactic economy, production, fleet travel, ownership and AI perception (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by the RE agent, ahead of roadmap steps **S3 "Galactic economy & fleets"** and **S2 "Real
perception"**. Clean-room: prose, tables and arithmetic only. Nothing from the decompilation is reproduced.
Addresses are RVAs in StarWarsG.exe. Many saved function names are wrong; the address is authoritative.
XML values are vanilla FoC (`extracted/foc_data_text.zip`). GameConstants offsets are from the GameConstants
field table in `xml_field_tables.md`; the singleton is at 0x140b15800.

Builds on `GALACTIC.md`, `PHASE2_ANSWERS.md` (30 Hz tick, path finders), `OBJECT_RUNTIME.md` (tick order, object
stores, list orders) and `OBJECT_TYPES.md`. Those are not repeated here.

Confidence:
- **[H]** read in the code and cross-checked;
- **[M]** read in the code, but intent or a field meaning is inferred;
- **[L]** inferred; needs a reference trace (Q2: Gabriel runs the game with a probe mod under `tools/probe_mod/`).

---

## 0. Summary for the implementer

1. **Income is paid once per fiscal cycle, not per frame.** The cycle is `Fiscal_Cycle_Time_In_Secs` = 45 s,
   i.e. **1350 frames**. The payment runs in the galactic mode update (0x4b2ef0), after the object stores have
   been updated. [H]
2. **Per-planet income** = base × black-market multiplier + Force-alignment term + political-control term +
   trade-route term. It is then clamped by ability limits, zeroed while "credit production is halted", and has a
   credit siphon taken out. The **base** is the planet's runtime credit value. It starts from `Planet_Credit_Value`
   and becomes `Planet_Destroyed_Credit_Value` once the planet is destroyed. [H]
3. **FoC corruption income.** A faction with `Benefits_From_Corruption` collects, from every corrupted planet it
   does not own, that planet's income × `Corruption_Choice_Income_Percentage[corruption type]`. The owner is
   **not** charged for it. The faction also collects the piracy pool that enemy fleets leave on Piracy-corrupted
   planets. [H]
4. **Credits are a float32 per player** (player +0x70).
   - They can never go below 0; there is no debt.
   - They are capped at `max(1, planets owned) × Credit_Cap_Per_Planet` (40000). The cap is recomputed for every
     player whenever any planet changes owner, and the clamp is applied at once.
   - AI players' **positive** credit gains (income *and* refunds) are multiplied by their difficulty
     `Credit_Multiplier`.
   
   [H]
5. **Production** uses two queues per planet: ground and space.
   - Humans are limited to `Max_Build_Queue` entries per queue. It defaults to 5 and is not set in vanilla XML.
     AI players have no limit.
   - Only the head entry counts down. Its timer restarts when it becomes the head.
   - Humans pay the full cost when they queue an item (`Pay_As_You_Go` = false). **AI players always pay as you
     go**, frame by frame, through their budget.
   
   [H]
6. **Build time** = trunc(`Build_Time_Seconds` × `Production_Speed_Factor`), then × planet modifier ×
   base-level-versus-tech factor × AI difficulty `Galactic_Build_Time_Multiplier` × corruption time multiplier,
   then ÷ the number of required special structures present. Timers are in whole frames: trunc(30 × seconds).
   [H]
7. **Fleet hyperspace speed is a per-frame distance.**
   - Speed = (slowest `Hyperspace_Speed` among hyperspace-capable members that are not carried) × faction
     `Hyperspace_Speed_Factor` (0.07) × an unidentified mode multiplier M × the hop factor R.
   - R = max(the route's `HS_Speed_Factor`, 1.0, or `Corruption_Hyperspace_Bonus` 1.5 when the mover benefits
     from corruption and both ends are corrupted).
   - **A route slower than 1.0 never slows a fleet.**
   
   [H] (M is [L])
8. **Ownership is re-derived every frame** by the planet service (0x3fe1b0).
   - Precedence: ground-base owner → ground troops → starbase owner → owner of active production → orbital
     control (surface-inaccessible planets only, [L]) → the planet's initial leaning → the neutral player.
   - When the derived owner differs, the owner flips **immediately** with political control 0. Control then ramps
     towards its target at 1 unit per `Political_Control_Change_Time_Seconds` (7 s).
   
   [M]
9. **AI perception for the galaxy** (S2) is read through three perception-token paths. Each is evaluated on
   demand, with no caching:
   - `Target.EnemyForce.SpaceTotalUnnormalized` gives Node_Weight;
   - `Target.EnemyForce.HasSpaceForce` gives AI danger;
   - `Target.TargetPlayerID` gives the AI's perceived owner.
   
   The "enemy strength" term in Node_Weight is the `AI_Combat_Power` of the planet's **starbase**. The four
   tokens that PHASE2_ANSWERS left unresolved are now named (§6). [H] for the names, [L] for the token
   semantics.

---

## 1. Credits and income

### 1.1 Player credit store

| player field | meaning | conf |
|---|---|---|
| +0x70 | credits, float32 (Lua `Get_Credits`) | H |
| +0x74 | credit cap, float32; −1 = uncapped | H |
| +0x360 | AI system pointer (non-null for AI-controlled players) | H |
| +0x62 | Is_Human | H |

**Add_Credits (0x27f370)** takes (player, amount, notify-AI flag). Every credit change goes through it: income,
costs, refunds, story rewards, bounties and Lua `Give_Money`.
1. If amount > 0 **and** the player has an AI system, amount ×= the player's difficulty adjustment
   `Credit_Multiplier`. The adjustment object comes from 0x4b0500, +0x20. Vanilla `DifficultyAdjustments.xml`:
   Easy 0.5, Normal 1.0, Hard 1.2. If no adjustment is set, an internal all-1.0 adjustment is used. [H]
2. credits += amount (float).
3. If credits < 0, set credits to 0. The amount actually applied shrinks accordingly. **There is no debt.** [H]
4. If cap ≥ 0, and the current mode's virtual +0xF8 returns true (or there is no mode and the global flag at
   0x140b155c5 is clear), credits are clamped to the cap. [H] for the clamp, [L] for what the mode test means.
5. Then a player-stats event and a credits-changed notification are sent. With the notify flag set, the AI budget
   is told the applied amount (0x4b0310).

**Cap (0x288f40)**, recomputed per player:
- cap = max(1, number of planets the player owns, as float) × `Credit_Cap_Per_Planet`. The planet count comes from
  the category-3 object query filtered by owner. GameConstants +0x17c; vanilla 40000.
- credits = min(credits, cap).
- If the test 0x28afb0 on the game-mode manager (0x140b153e0) is true, cap = −1 (uncapped). This is probably a
  non-campaign mode. [L]
- Called on player setup (0x292050) and, **for every player in index order**, after every planet ownership change
  (0x3fb040). [H]

### 1.2 Fiscal cycle (galactic mode update 0x4b2ef0)

The mode keeps `last_cycle_frame` (+0x368), `next_cycle_frame` (+0x36c) and a cycle counter (+0x370).

On a galactic frame f where next_cycle_frame ≤ f (f = mode frame +0x10):
1. **For each player in player-list index order** (0x140a16fd0 vector):
   - I = Player_Income(player, consume = true) (§1.3).
   - If I ≠ 0, Add_Credits(player, I, notify = false).
   - If the player is AI-controlled and not human, and its galactic AI sub-object is active, the AI's income
     history is fed with I (0x614340; Lua debug string "Last Fiscal Cycle income proportion").
   
   [H]
2. last_cycle_frame = next_cycle_frame.
3. next_cycle_frame += round_half_away(30 × `Fiscal_Cycle_Time_In_Secs`). That is 1350 in vanilla (0x27c9a0 →
   0x22f9c0). [H]
4. Cycle counter += 1. This is the "galactic day" (§5). [H] counter, [M] meaning.
5. **Black-market reroll**, for each planet in planet-list order:
   - If the planet's type has `Planet_Black_Market_Influences` (type +0xbd), its black-market multiplier
     (data pack +0x2a0) is set:
     - if the planet is not destroyed (data pack +0x2c8 = 0): a uniform draw in [`Black_Market_Income_Mult_Min`,
       `Black_Market_Income_Mult_Max`] = [1.0, 3.0];
     - if it is destroyed: 1.0.
   - The draw uses the LCG at 0x140a13e24 (0x1ffbb0):
     - state ← state × 0x41C64E6D + 0xBDF (mod 2³²);
     - u = float((state >> 10) & 0x7FFF) / 32767.0f;
     - value = (max − min) × u + min, with min and max swapped if they were given reversed;
     - all float32.
   - One draw per qualifying planet.
   
   [H] for the arithmetic. [M] for whether 0x140a13e24 is the synced game RNG: it must be in the sim RNG for
   lockstep.
6. UI refresh, then an AI notification (kind 10) for every player.

Independently, when the update argument is a multiple of 30 (once per simulated second; the argument is the
frame, [M]), an AI notification of kind 8 is sent for every player. [H]

**First cycle.** No initialiser for +0x36c was found in the galactic mode constructor, so the field is 0 and the cycle
runs on mode frame 0. That frame-0 cycle runs **before the galaxy is built**, so it pays nobody, feeds no AI income
history and rerolls no black-market value (no planets yet); only steps 2-4 change anything. [H] from the Q2 traces
(2026-10-30 HotM runs, reader 2): at mode frame 0 there are no objects and the F fields read last 0 / next 1350 /
counter 1; credits stay at their start values (Empire 13,000) until the cycle on frame 1350 (seen at mode frame 1351,
+667.5); every black-market multiplier is 1.0 at mode frame 1. Port: `GalacticSim::start` ends in that state (last 0,
next = one period, counter 1), so the first payment is on frame 1350 (q2f t02, 2026-11-02).

**Saved state.** +0x368/+0x36c/+0x370 must be saved with the mode. Unverified [L].

### 1.3 Player_Income(player, consume) (0x4b16f0)

The result is 0 unless the player's faction is not `Is_Neutral` (+0x108), has `Can_Control_Planets` (+0x109) and
is `Is_Playable` (+0x20a8). [H]

Otherwise, walk all planets (category-3 query, planet-list order; see PHASE2_ANSWERS Q1 and OBJECT_RUNTIME §6).
Accumulate a float sum S starting at 0. For each planet P with data pack D:
1. **Owned:** if P's owner = player, S += Planet_Income(P, gross = false) (§1.4). [H]
2. **Corruption income:** else, if D's corruption type (+0x2f4) ≠ −1 and the player's faction has
   `Benefits_From_Corruption` (+0x10b):
   - pct = GameConstants corruption income table[type] (0x2c8140; the table at +0x11ed8, 8 entries, 0 out of range);
   - if pct > 0 and g = Planet_Income(P, gross = true) × pct > 0, then S += g.
   
   [H]
3. **Siphon:** if D's siphoning player (+0x200) = player:
   - Planet_Income(P, false) is evaluated first, which refreshes D +0x20c;
   - then S += D +0x20c.
   
   [H]
4. **Piracy pool:** if the player's faction benefits from corruption and D +0x2a4 > 0:
   - S += D +0x2a4;
   - when `consume` is set, D +0x2a4 = 0.
   - So the **first** such player in index order takes the whole pool. [H]

Steps 1–4 run in that order within each planet. All arithmetic is float32 accumulated in planet order. [H]

Vanilla `Corruption_Choice_Income_Percentage` (GameConstants +0x11ed0) = 0.5, 0.8, 0.4, 4.5, 0.5, 0.5, 0.8, 1.0.
It is indexed by corruption type 0..7: Intimidation, Piracy, Kidnapping, Racketeering, Corrupt_Militia, Bribery,
Black_Market, Bonded_Citizens (enum strings at 0x5e0590, Corruption_None = −1). [M] for the index↔name mapping;
Piracy = 1 is consistent with §3.4.

`Credits_Accumulation_Factor` (faction +0x3a8; Rebel 0.95, Empire 0.85) is **not** read on this path. [M]

### 1.4 Planet_Income(P, gross) (0x3f68b0)

Let b = the planet base value (0x3733c0):
- the type's `Planet_Credit_Value` (type +0x7c8) when P has no data pack;
- the type's `Planet_Destroyed_Credit_Value` (+0x7cc) when D +0x2c8 (destroyed) is set;
- otherwise the runtime base D +0x228 (int; save id 33). It is presumably initialised from `Planet_Credit_Value`
  and changed by income abilities [L].

Steps:
1. **Halted:** if the credit-halt list (D +0x288) contains any live entry, the result is 0. Entries expire by
   frame; the list is folded with a boolean OR. It is filled by `Can_Halt_Credit_Production` /
   `Duration_Of_Credit_Halt` abilities (0x5b2450). [H]
2. **Expire breakdown entries:** entries in the income breakdown list (D +0x268) whose expiry frame (entry +0x14)
   is < now are removed. [H]
3. **Refresh the four standard entries.** Each is written into the list keyed by (planet, kind). An existing entry
   is updated in place; a new one is appended at the tail. Some kinds remove their entry when the value is 0.
   - **Base** (0x3f4940): float(b) × black-market multiplier (D +0x2a0; 1.0 unless rerolled). [H]
   - **Force alignment** (0x3f4f90). Let s = the owner faction's `Force_Alignment` (faction +0x110), t = the
     planet's `The_Force` (type +0x1664) and k = `Force_Strength` (type +0x166c).
     - If s = t and s ≠ enum 1: value = float(b) × k × `MaxCreditIncomeAlignmentBonus` (+0x57c, 0.5).
     - If s ≠ t and s ≠ enum 1: value = −float(b) × k × `MaxCreditIncomeAlignmentPenalty` (+0x580, 0.5).
     - Otherwise 0, and the entry is removed.
     - Enum 1 is presumably "Neutral". All vanilla planets are `Neutral`, so the term is 0 unless
       `Force_Strength` ≠ 0.
     
     [H] for the arithmetic, [M] for the enum.
   - **Political control** (0x3f5470): only when the owner is not the first `Is_Neutral` faction's player.
     - c = the political control (D +0x68, float);
     - q = `Political_Income_Curve`(c), the GameConstants +0x128 curve object, evaluated through its virtual
       +0x10;
     - if q > 0: value = q × float(b) − float(b); otherwise 0.
     - Vanilla curve "0,0, 1,1, 5,1": the value is 0 at full control, **negative while control is ramping
       between 0 and 1** after a capture, and 0 at exactly c = 0.
     
     [H] for the formula, [M] that the curve interpolates piecewise-linearly.
   - **Trade routes** (0x3f6210): G = Σ `Credit_Gain_Factor` over P's links (data pack +0x58 array, in link
     order). A link counts when:
     - both endpoints' types match the route's two endpoints (route +0x30/+0x40);
     - the neighbour passes 0x39a2c0(neighbour, owner) (presumably "held by owner or ally" [M]);
     - and the factor > 0.
     
     value = float(b) × G. **No black-market multiplier** is applied here. Vanilla routes all have 0.5. [H]
4. **Sum** the whole breakdown list front to back as a float left fold. The four standard entries plus any
   ability entries (`Planet_Income_Bonus_Ability`, `Income_Stream_*` [L]) are all in the list. [H]
5. **Clamp:** if the min flag (D +0x280) is set, the sum is at least D +0x278. If the max flag (+0x281) is set, the
   sum is at most D +0x27c. These are set by abilities. [H]
6. **Siphon,** only when gross = false and the siphoning player D +0x200 ≠ −1:
   - If the planet is not corrupted and D +0x208 < now, the siphon is ended (0x3ff1f0).
   - If 0 ≤ D +0x204 ≤ 1: siphon = sum × D +0x204, stored in D +0x20c, and the result is sum − siphon.
   
   [H]

**Determinism.** The breakdown list order depends on history: entries removed at value 0 are re-appended at the
tail. A port must keep a per-planet ordered list with the same append and remove semantics to reproduce float
rounding exactly. With vanilla data every term is a multiple of 0.5 × integer, so the order rarely matters. [H]

**Per-planet values the UI shows** (0x4b1950, 0x2f2ad0) are diagnostics only. They are not used by the sim.

### 1.5 Other credit sources and sinks (pointers only)

| source | where | note |
|---|---|---|
| planet capture bonus | 0x3fb040 | the new owner (playable faction, planet not destroyed) receives `Planet_Capture_Bonus_Reward` (type +0x7d0); message `TEXT_PLANET_CHANGED_HANDS_BONUS`. [M] for the exact amount argument |
| bounty | 0x392600 | "Awarding a %d credit bounty…" |
| story reward CREDITS | 0x456970 | |
| Lua `Give_Money` | 0x603130 | |
| production cost and refund | §2 | |
| Underworld corruption actions | `Corruption_N_Credit_Cost` (type +0x2468…) | not traced |
| multiplayer `Distribute_Credit_Quantum` (1000) | 0x498cd0 | gifting between teammates |

**Taxes.** `Progressive_Taxation` (+0x150) and `Income_Redistribution` (+0x178) have **no reader on the galactic
income path**. The only reference to their neighbourhood is the field table. Treat them as unused. [M]
`Political_Income_Curve` is the only one of the three that is used.

**Maintenance.** `Fleet_Maintenance_Update_Delay_Seconds` (+0x110) and the perception tokens `Maintenance*` exist.
No per-unit upkeep deduction was found on the fiscal path. → Settled 2026-10-28: `Maintenance_Cost` is read only by
the AI tokens and the delay constant has no reader, so there is no upkeep [H] (STRATEGIC_GAPS §2.3).

---

## 2. Production queues

### 2.1 Layout

The planet's production behaviour object is at planet GameObject +0xd0. It holds two DynamicVectors of
`ObjectUnderConstructionClass*`:

| queue | vector at | data / count | entries that go there |
|---|---|---|---|
| 0 = ground | +0x08 | +0x10 / +0x18 | `Required_Ground_Base_Level` ≠ 0, or category flag 0x5f |
| 1 = space | +0x20 | +0x28 / +0x30 | everything else. It must have `Required_Star_Base_Level` ≠ 0, or be a level-1 base type (flag 0x5c with `Base_Level` = 1) |

Entries with `Build_Cost_Credits` < 1 are rejected. [H]

`ObjectUnderConstructionClass` (0x38 bytes):

| offset | field |
|---|---|
| +0x08 | type |
| +0x10 | build time T, seconds (float) |
| +0x14 | timer {start frame, duration frames} |
| +0x1c | remaining seconds when last advanced |
| +0x20 | player id |
| +0x24 | caller tag |
| +0x28 | cost C (float) |
| +0x2c | planet object id |
| +0x30 | created object |

[H]

### 2.2 Enqueue (0x3fff70, from the production event 0x5242c0)

1. Check buildability (0x2804d0: tech, prerequisites, affordability [M]).
2. **Human queue limit:** a human may enqueue only while the target queue holds fewer than `Max_Build_Queue`
   entries (GameConstants +0x11778). The constructor default is 5 (0x2c91c0 area) and vanilla XML does not set it.
   AI players are not limited. [H]
3. A unit-limit check (0x2ac320 on the galactic store; type, player). Presumably `Max_Instances` / unique units
   [L].
4. C = Get_Cost(type, planet) (§2.4). T = Get_Build_Time(type, planet) (§2.3).
5. Create the entry and start its timer at the current frame for T.
6. If the player does **not** pay as you go, Add_Credits(player, −C). Pay-as-you-go is decided by 0x289050: true
   for every non-human player, and `Pay_As_You_Go` (+0x320, vanilla false) for humans. [H]
7. Append the entry at the tail of the queue. [H]

### 2.3 Build time (0x400370)

- If the owner has the cheat/debug flag (player +0x3c0), T = 5.0. [M]
- Otherwise T = T₀ × m_planet × m_tech × m_ai × m_corr ÷ n, all float32, multiplied left to right in this order:
  `m_planet × T₀ × m_tech × m_ai × m_corr`, then divided by n.
  - **T₀** = float(uint32(float(`Build_Time_Seconds`) × `Production_Speed_Factor`)). The result is truncated
    (0x371230). Vanilla factor 1.0. [H]
  - **m_planet** = 1 − (planet build-time bonus) (0x55a010). The bonus is the sum, over the planet's modifier
    sources, of each source's largest bonus, clamped by optional limits. This comes from abilities such as
    `Tactical_Build_Time_Bonus_Percentage` and hero effects. [M]
  - **m_tech** applies only to unit-like types: cost ≥ 1, and the type is not a ground-structure type (0x282400 true
    and not ground). It also needs the planet's starbase level L (D +0x148) to be in 1..5.
    - d = L − `Tech_Level` (type +0x89c).
    - d ≤ 0 → 1.0; d = 1 → `Production_Speed_Mod_Base_Vs_Tech_1` (0.90); 2 → `_2` (0.75); 3 → `_3` (0.55);
      ≥ 4 → `_4` (0.45). `_0` is never read.
    - The **starbase** level is used even for ground units that pass the test. [H] for the arithmetic, [M] for the
      classification.
  - **m_ai** = the difficulty `Galactic_Build_Time_Multiplier` (+0x28) for AI-controlled non-human players (Easy
    1.5, Normal 0.8, Hard 0.6). Otherwise 1. [H]
  - **m_corr**: when the owner's faction does **not** benefit from corruption, it is
    `Corruption_Galactic_Production_Time_Multipliers`[planet corrupted ? 1 : 0] (faction +0x2fd8; Rebel and Empire
    "1.0, 0.95"). Otherwise 1. [H]
  - **n** = max(1, Σ over the type's `Required_Special_Structures` entries of the number of structures on the
    planet whose type equals that entry). It applies only if `Build_Time_Reduced_By_Multiple_Factories`. The
    structures are counted in D's two structure lists, +0x158 and +0x170 (0x4b8170). [H]
- **Frames:** the timer duration is int(30 × T), truncated toward zero (0x4ad3a0). Remaining seconds =
  max(0, start + duration − now) / 30. The entry is due when start + duration ≤ now (0x4ad350). [H]

### 2.4 Cost (0x3711c0 → 0x400240)

1. raw = m_planet_cost × float(`Build_Cost_Credits`) × m_corr_price × m_techupg.
   - m_planet_cost: 0x559c10, the analogue of m_planet for price [M].
   - m_corr_price: `Corruption_Galactic_Production_Price_Multipliers`[corrupted ? 1 : 0] (faction +0x2ff0, "1.0,
     0.9"), applied only if the faction does not benefit from corruption.
   - m_techupg: only for `Build_Advances_Tech_Level` ≠ 0 items. It is the player's tech-cost modifier, 1 + the
     fold of player +0x270 entries, clamped, with a minimum of 0.0001 (0x281960) [M].
2. If raw > 0, raw = max(1, raw).
3. Cost = round_half_away_from_zero(raw), as an int.

[H]

### 2.5 Queue service, per planet per frame (0x402840, the production behaviour's service)

The two queues are serviced in order: ground (0), then space (1). Only the **head** of each queue is touched.

1. **Starbase-type head without an owned starbase.** The head type has category flag 0x5e, 0x60, 4 or 0x61 (the
   starbase and upgrade families [M]) and the starbase owner (D +0x13c) ≠ the entry's player.
   - The head is removed with **no refund** and the next entry starts.
   - The service **returns for this frame**, so the other queue waits until the next frame.
   
   [H] for the behaviour, [M] for the flag meaning.
2. **Starbase-in-production with an enemy fleet in orbit.** The head type has flag 0x5c, the planet is not in an
   active battle (0x2b0470), and some orbiting fleet (D +0xc8 list) is visible, not held by the entry's player or
   an ally, and has a combat-capable member.
   - The head is removed with no refund, a tooltip is shown to the local owner, and the service returns.
   
   [H]
3. **Head done** (0x498f70): for an up-front payer the timer has expired; for a pay-as-you-go payer the remaining
   time is exactly 0.0.
   - **Ground cap stall:** in the ground queue only, when the count of ground forces on the planet (0x3f9530) ≥
     `Max_Ground_Forces_On_Planet` (+0x10c8, vanilla 10) and the two presence checks 0x3f9270 / 0x3f9220 return
     two different non-null players, the head waits.
     - This is "contested surface" [L]. Q2: fill a planet to 10 units and queue one more, with and without enemy
       troops present.
   - Otherwise:
     - pop the head;
     - if the type is not a tech upgrade, create the object (0x400b40). If creation fails and the type is none of
       the flags 0x5c/0x5b/0x60/0x5f, refund (§2.6);
     - tech upgrades instead **set** the player's tech level **to** `Build_Advances_Tech_Level`, clamped to the max, with
       the unlock sweep (0x288980; corrected 2026-10-28, REMAINING_GAPS §2 / §1.4);
     - count the build (0x2821b0);
     - store the paid cost on the new object (+0x32c), used for sell-back and score;
     - start the next entry.
   
   [H]
4. **Head not done.**
   - **Up-front payer:** the countdown advances. Remaining seconds are cached in +0x1c. UI speech fires at 60 s
     and 10 s boundaries (0x401e50). [H]
   - **Pay as you go:** the installment is a = (prev_remaining − remaining) / T × C (0x498ec0).
     - A player with an AI system (player +0x360, 0x2814d0) whose byte +0x62 is 0 pays through its AI budget,
       0x53b900(the planet's AIPlanetBuildQueue for that queue, a). The rule is in AI_GALACTIC_2 §3.4 "Installment"
       (q2f t01, 2026-11-02): while that queue has a current AI build task, the installment is paid from the task's
       goal budget and can be refused; otherwise it is always paid.
     - Any other PAYG player (a human with Pay_As_You_Go) needs credits ≥ a and then pays a.
     - On success the timer advances. On failure the timer is re-armed from now with the cached remaining time
       (0x499090), which **pauses** the build.

     [H] (0x402840 service branch, 0x53b900, 0x64adb0, 0x615700 read 2-11-2026).

The production service is reached through the galactic store's "planet production service" step (OBJECT_RUNTIME
§5.3 step 3, 0x3fc750). [M]

### 2.6 Refunds and cancel

- **Refund amount** (0x499000): an up-front payer gets the full C. A PAYG payer gets clamp(1 − remaining/T, 0, 1) ×
  C, which is what it paid. [H]
- Refunds go through Add_Credits(player, +amount, notify AI). **AI refunds are therefore multiplied by
  `Credit_Multiplier` again.** That is a quirk to preserve. [H]
- **Cancel by index** (0x403770), **cancel the last entry of a type** (0x4035d0) and **cancel the head**
  (0x403900):
  - each takes a refund flag;
  - removing index 0 starts the next entry;
  - the UI cancel (`CommandBarClass::Cancel_Production` 0x3cb2f0) passes refund = true.
  
  [H]
- **Re-evaluation** (0x402490 ground, 0x402660 space; argument = the current ground/star base level):
  - entries are walked **from the tail to the head**;
  - an entry is deleted when its required base level > the given level, or the type is no longer buildable at the
    planet (0x3706f0) or by the player (0x285860);
  - **there is no refund on this path**;
  - deleting index 0 starts the new head and **stops the walk**.
  
  Re-evaluation runs when a fleet leaves the planet (0x4358c0, with D +0x130 ground-base level and D +0x148
  starbase level) and on capture (space queue with level 0). [H] for the walk, [M] that the vector removal frees
  without a refund. Q2: queue a starship, then destroy the starbase or capture the planet, and log credits.

### 2.7 Tech gating and caps

- `Required_Ground_Base_Level` / `Required_Star_Base_Level` are checked against D +0x130 / D +0x148, at enqueue
  and on re-evaluation. [H]
- Player tech level versus `Tech_Level` sits in the buildability test 0x2804d0 → traced 2026-10-28, STRATEGIC_GAPS §1.3. [H]
- `Max_Ground_Forces_On_Planet` = 10 is the only galactic population rule found (§2.5 step 3).
  `Base_Population_Capacity` / `Population_Value` / `Space_Tactical_Unit_Cap` are tactical. [M]

### 2.8 On planet capture

The space queue is re-evaluated with starbase level 0, which deletes everything that needs a starbase, without a
refund. The starbase owner (D +0x13c) is reset to −1 (0x3f7280, 0x3f98a0). Ground-queue clearing on capture is
logged by 0x2bff60 ("Clearing all land based production objects…") and was not traced. [M]

---

## 3. Fleet movement over routes

### 3.1 Departure (0x431c80, from the move order handlers 0x39f380 / 0x39f580)

The fleet data pack is at fleet +0xc0: contents vector +0x10/+0x18, current planet +0x40, destination +0x50,
base speed +0x64, hop speed +0x68.

1. If the fleet is already in transit (+0x50 ≠ 0) or is not at a planet, refuse. [H]
2. **Carrier simulation.**
   - holds = Σ `Squadron_Capacity` (type +0x990) over the directly held members;
   - squadrons that fit are marked as carried;
   - non-hyperspace-capable squadrons that do not fit **split off into a new fleet** that stays at the origin
     ("Squadron … is staying behind and is joining new fleet");
   - fill order: non-hyperspace squadrons by cost first, then hyperspace-capable squadrons by speed.
   
   [M] for the split, [L] for the exact fill order. Q2: send a fleet with more fighters than holds and log which
   ones stay.
3. **Base speed** s = min `Hyperspace_Speed` (type +0x648) over members that are hyperspace-capable (type
   `Hyperspace` +0x24) and not carried. Team or container members are looked through to their contents. If none
   qualify, s = 0 and the fleet cannot jump. [H]
4. v₀ = s × faction `Hyperspace_Speed_Factor` (faction +0x3b0; 0.07 for every vanilla faction) × M, stored in
   +0x64.
   - M is a float at (object +0x2b8) → +0x20 → +0x230. The same factor appears in the AI's travel-time estimate
     (0x648e80). → Settled 2026-10-28: M = the mode's `Object_Max_Speed_Multiplier_Galactic` (FoC **5.0**, TR 11.34),
     rewritten every mode tick (STRATEGIC_GAPS §2.1) [H]. The §3.2 example speeds are therefore 5× too low for FoC.
     **[port] 2026-10-28:** the port uses M = 1.0 pending probe G1. **2026-10-29: M = 5.0 confirmed by the Q2 trace
     (retail Corellia -> Kuat = 26 ticks)**; the port flip (`kApplyModeSpeedMultiplier`) waits for Gabriel (STRATEGIC_GAPS §2.1).
     Fleet members that are transports count with their own Hyperspace_Speed; only fleet containers (squadrons) are
     looked through (STRATEGIC_GAPS §1.7).
> **Status 2026-10-30:** `engine/galactic/src/strategic.cpp:39` now has `kApplyModeSpeedMultiplier = true` and line 40 scales fleet speed by `rules_->constants.fleet_speed_multiplier`, so the port applies M (FoC 5.0); the "port uses M = 1.0" and "waits for Gabriel" statements above are out of date. The comment at `engine/galactic/include/alamo/galactic/economy.hpp:67` still says the flag is false.
5. Hop factor R (0x397470), for hop A→B:
   - start with 1.0;
   - if the mover's faction `Benefits_From_Corruption` and A and B are both corrupted, use
     `Corruption_Hyperspace_Bonus` (+0x1213c, 1.5);
   - if A and B are joined by a trade route **and** the route's `HS_Speed_Factor` ≥ the current value, use the
     route factor.
   - So R = max(base, route HS). Vanilla routes are 5, 6, 7 or 10.
   
   [H]
6. Hop speed v = v₀ × R, stored in +0x68. The fleet leaves the origin's orbit list and the origin's production is
   re-evaluated (§2.6). All players' visibility is updated. [H]
7. **Piracy** (FoC): when the origin's corruption type is 1 (Piracy), there is a player of the corrupting faction
   (0x2948d0), that player is not allied with the fleet owner, and the fleet is not stealthed (0x3a9760):
   - the origin's piracy pool (D +0x2a4) += Σ `Piracy_Value_Credits` (type +0x235c) over every object in the fleet,
     recursively;
   - the corrupting faction collects it at the next fiscal cycle (§1.3 step 4).
   
   [H]

### 3.2 Per-frame motion (fleet locomotor service 0x56a1e0, vf6)

Each frame, while not facing the target the fleet turns (the facing is recomputed). Then position += velocity,
where velocity = unit direction × v **per frame**.

- If the remaining distance after the step is still ≥ 2 × |velocity|, continue.
- Otherwise snap to the hop's arrival point. This is an orbit slot computed by 0x569d50 [M], not the planet
  centre. [H]

So a hop of length D takes k frames, where k is the first step at which D − k·v < 2v, computed with float32
component accumulation. Vanilla example: s = 1.5 on a route with HS = 5 gives v = 0.525 units/frame. A 60-unit hop
then takes ≈ 113 frames ≈ 3.8 s. With no route (R = 1) the same hop takes ≈ 20 s. [H] for the rule, [M] for the
arrival-point geometry.

**Hero modifiers.** None appear in the speed formula; heroes contribute only through their own
`Hyperspace_Speed` like any member. [H] for this function.

### 3.3 Arrival at a hop planet

The path is a vector of planet ids at locomotor +0xb8, with index +0x80. It comes from the A* of GALACTIC.md §3.

1. If the reached planet is **inaccessible** (D +0x2e0 set), and the mover is not human or the planet is not in a
   battle, the fleet **turns back** to the previous hop (0x569980) and stops there. [H]
2. Otherwise the fleet enters orbit (0x4338d0). This returns whether a conflict starts.
3. **Intermediate hop:**
   - no conflict → depart to the next hop (§3.1 steps 5–6, with R recomputed for the new pair);
   - conflict → the fleet **stops here** (it is intercepted), **unless the mover's faction benefits from
     corruption**: Underworld fleets run through and continue.
   
   [H]
4. **Final hop, or stopped:**
   - speed = 0 and the path is cleared;
   - an AI fleet with the "continue" flag (fleet +0x96) may proceed (0x434720);
   - otherwise the fleet is placed in orbit;
   - **merge:** if a friendly fleet already occupies the orbit slot returned by 0x3f7d70 / 0x3f9370, the arriving
     fleet is merged into it (0x433dd0) and a `merge_fleets` event fires;
   - nearby incoming friendly fleets are pulled in (0x569700) when a battle starts.
   
   [H] for merge and stop, [M] for the slot logic.

**Blockade and interdiction.** There is no separate blockade mechanic in movement:
- the path finder avoids dangerous planets (GALACTIC.md §3);
- arrival in a defended system triggers the conflict;
- `Blockade_Run_Attrition_Factor` (0.2) and `Prevents_Blockade_Run_Attrition` exist but were not traced.

[L] Q2: order a fleet through an enemy-held system with move mode 5 and log the losses.

### 3.4 Retreat

Galactic retreat is a tactical outcome: retreat coordinators 0x532b70 / 0x534bd0, `RetreatAutoResolve*Attrition`,
`*_Retreat_Allowed_Countdown_Seconds` = 30. It is not part of S3 movement. A retreating fleet returns to the
planet it came from (`Lua_Retreat` 0x603de0). Not traced further. [L]

---

## 4. Ownership changes

### 4.1 Derived owner, each frame (planet behaviour service 0x3fe1b0 → 0x3f6df0 → 0x3f6af0)

The owner is the first non-empty result in this order (0x3f3830):
1. the owner of the ground base, when the ground-base level (D +0x130) > 0 (owner D +0x124);
2. the owner of the first non-hidden ground unit on the surface (D +0xe0 list);
3. the starbase owner (D +0x13c), when the starbase level D +0x148 > 0;
4. the owner of an active production entry (0x401960);
5. orbital control by fleets in orbit (D +0xc8). Only when 0x375220 returns **false**: surface-inaccessible
   type, or a destroyed planet (data +0x2C8 = 1), see STRATEGIC_GAPS §1.3 after step 24 [H];
6. the planet's political leaning (0x3f5710, type data, 0..5);
7. the first `Is_Neutral` faction's player.

[M]

**Target political control** = clamp(Σ breakdown entries, 0, 5). Entries come from the sources above and from
`Political_Control_Gain` of trade routes to planets the same owner holds, where both planets have control ≥ 1
(route +0x48; vanilla 0). Note the hard-coded 5; `MaximumPoliticalControl` is not used here. [M]

### 4.2 Applying it (0x3fb040)

Data pack fields: control +0x68 (float), owner +0x6c, target control +0x70, target owner +0x74.

- **First call for a planet** (D +0x1c9 = 0): the owner and control are set directly. If the owner's faction
  benefits from corruption, the planet starts **corrupted with type 3**. [H]
- **When the target owner changes:** the owner is set at once, control is set to 0, and visibility is
  re-broadcast. [H]
- **Ramp** (0x4adab0, each call, with elapsed frames e):
  - Δ = e / (30 × m × `Political_Control_Change_Time_Seconds`) (+0x124, 7);
  - m = the product, over sources, of each source's smallest alignment factor;
  - alignment factor: (1 − k × `MaxInfluenceTransitionAlignmentBonus` +0x574) when the owner's alignment matches
    The_Force, (1 + k × `…Penalty` +0x578) when it is opposed;
  - control moves toward the target by Δ, clamped at the target;
  - if the owner differs from the target owner, control falls by Δ and the owner switches when it reaches 0.
  
  [H]
- When control drops from ≥ 1 to < 1, 0x403490 is called for both production queues. That it re-checks the head
  is inferred. [H] for the call, [M] for the meaning.

**On an owner change**, in order:
1. visibility for all players;
2. the store event;
3. a story event "POLITICAL CONTROL";
4. **the credit cap for every player** (§1.1);
5. the conflict list update;
6. the capture bonus to a playable new owner (§1.5);
7. the planet-changed-hands message;
8. corruption handling:
   - the old owner benefited and the new one does not → corruption is removed (0x4b7260);
   - the new owner benefits, the old did not, the planet was not corrupted and is not protected (D +0x2e2) →
     corruption is applied (0x4b72e0);
9. if the siphoner is the new owner, the siphon is cleared.

[H] for the order and calls, [M] for the effects of 0x4b7260 / 0x4b72e0.

**Forced transfers.**
- `Change_Owner` (0x3f7280; Lua and story):
  - clears the conflict;
  - clears the starbase (0x4bda40);
  - re-evaluates the space queue at level 0;
  - starbase owner = −1;
  - retreats the local player's surface units, or transfers or removes the others (0x4b7670 / 0x4b7860 [M]);
  - clears the political breakdown;
  - applies with control 1.0.
- **Planetary revolt** (0x3f98a0): forces every foreign player's units off the planet and hands it to the
  revolting player. [H]

**What transfers** (structures destroyed or converted) is decided by the tactical battle and by 0x4b7670 /
0x4b7860, not traced. [L] Q2: capture a planet with a ground base and a starbase through Lua `Change_Owner` and
by invasion, and list the objects before and after.

---

## 5. Galactic time

- **One logic tick = 1/30 s** (PHASE2_ANSWERS Q12). The galactic mode only ticks while it is the active mode, so
  galactic time freezes during tactical battles (OBJECT_RUNTIME §0.7). [M]
- **There is no separate "galactic day" clock.** The cycle counter (mode +0x370) advances once per fiscal cycle of
  1350 frames, and that is what the HUD day counter (`Text_Day_Counter`) and the scoring stat
  `Galactic_Cycles_Elapsed` (0x341ff0) show. [M]
- Things tied to the cycle: income, the black-market reroll, AI income history and the AI notification of kind 10.
  Things tied to whole seconds (frame % 30 = 0): the AI notification of kind 8. [H]
- Story `STORY_ELAPSED` and objective timeouts count seconds of mode time, not cycles (0x454980). [M]
- Every production, political control, siphon and halt timer counts mode frames (mode +0x10). [H]
- Strategic game speed (UI slider 0..4, global 0x140b153b8, saved in the options) does **not** appear in any
  formula here. It probably changes the wall-clock pacing only (PHASE2_ANSWERS Q12). [L] Q2: time 10 fiscal
  cycles at the slowest and fastest settings, in frames and in wall time.

### 5.1 Order of operations per galactic frame

Expanding OBJECT_RUNTIME §5.2 for the galactic mode (0x4b2ef0):
1. pre-step 0x4b2600 (not traced);
2. the common mode update (0x3639d0). Its primary-store update services objects **newest-registered first**. That
   covers, among others:
   - the planet service (0x3fe1b0: derived owner, political control, visuals);
   - the fleet locomotors (0x56a1e0: movement, arrival, merges);
   - then the planet production service for each planet (0x3fc750 → §2.5);
   - then delayed creation and the delete flush;
3. **fiscal cycle** (§1.2), if due;
4. the once-per-second AI notification.

So income on a cycle frame sees ownership and production results from the **same** frame. [M] for the inner
order, from OBJECT_RUNTIME; [H] that the cycle comes after the base update.

### 5.2 Determinism-relevant orders (S3)

| what | order | conf |
|---|---|---|
| players in the income loop, the cap recompute and the notifications | player-list index | H |
| planets in Player_Income and the black-market reroll | category-3 list (GOM +0x560), the same order as the path-finder nodes | H, M for the list direction |
| terms inside one planet's income | breakdown-list order (append-new, remove-on-zero) | H |
| links in the trade-route term | the planet's link-array order | H |
| production queues | ground before space; head only; re-evaluation tail→head | H |
| the black-market RNG | one draw per qualifying planet per cycle, in planet order | H |
| credits | float32; clamp at 0 and at the cap on every change | H |
| frames from seconds | timers truncate int(30 × s); the fiscal period rounds half away from zero | H |

---

## 6. AI perception for the galaxy (S2)

### 6.1 Evaluation machinery

- An AI player's galactic perception system is reached through 0x4b06d0(AI system, 0) → +0x38.
- Galactic code evaluates **perception token paths** against it with 0x4dd1d0(system, bindings, &out). The return
  value is an "ok" flag; the result is a double.
- The bindings are a `PerceptionContextClass` (0x3ef160) with:
  - **Target** bound to the planet's perception id (data pack +0x354);
  - **Self** bound to the AI player.
- A token path is a short vector of token ids. The ids are globals filled by the token registration 0x5b43d0.

[H]

Token globals that were unresolved in PHASE2_ANSWERS, now named from 0x5b43d0:

| global | token | conf |
|---|---|---|
| 0x140a43c58 | the Target variable (the first element of every path; it carries the planet id) | M |
| 0x140a43c7c | `EnemyForce` | H |
| 0x140a43cfc | `HasSpaceForce` | H |
| 0x140a43d08 | `GroundTotalUnnormalized` | H |
| 0x140a43d0c | `SpaceTotalUnnormalized` | H |
| 0x140a43dc4 | `TargetPlayerID` | H |

### 6.2 The four galactic consumers

| consumer | token path | how the result is used | conf |
|---|---|---|---|
| **Node_Weight** (0x693db0 → 0x5f2430, ground flag 0) | `Target.EnemyForce.SpaceTotalUnnormalized` | value = perception + (only if the planet is **not** held by the AI or an ally, 0x4ad9f0) the `AI_Combat_Power` of the planet's **starbase type** (D +0x140). The category mask is "all" | H |
| ground variant (0x5f2430, ground flag 1) | `Target.EnemyForce.GroundTotalUnnormalized` | + Σ `AI_Combat_Power` of the ground structures (D +0x158) and garrison types (0x4b8000) when not held | H |
| **AI danger** (`Is_Planet_Dangerous` 0x3f18c0) | `Target.EnemyForce.HasSpaceForce` | dangerous if the evaluation fails or the value ≠ 0.0. Only used when the planet has no starbase | H |
| **AI passability** (`Is_Planet_Passable_For_Player` 0x3f2510) | `Target.TargetPlayerID` | the value is read as a player id: failure → passable; no such player → flag 0; neutral faction → flag 0; otherwise flag 1 | H |
| A*-family cost (0x3f06d0, called by 0x3ef6a0) | `Target.EnemyForce.SpaceTotalUnnormalized`, then `GroundTotalUnnormalized` | adds the perception value to distance / HS. The caller is a second planet search, not the fleet A* of GALACTIC.md | L |

`AI_Combat_Power` (type +0xdf0, 0x370e60) is used as-is. In modes where the virtual at +0xE0 is true and the type
is a squadron (has `Max_Squad_Size`), it is scaled by the squad's fill fraction clamped to 0..1. [M]

This refines GALACTIC.md §4 / PHASE2_ANSWERS Q7. The "enemy strength when the AI does not hold the planet" term
there is the starbase's combat power, not the strength of the forces present.

### 6.3 Cadence

Nothing is cached. Each consumer evaluates at its own call site:
- Node_Weight: N evaluations per AI per all-pairs recompute (every 90 frames plus the initial run; PHASE2_ANSWERS
  Q11);
- danger and passability: once per `Edge_Cost` call for AI-controlled movers, i.e. per A* hop expansion.

All of them read world state as it stands at that point of the frame. A port may cache per (AI, planet, frame), but
a cached value must not survive a state change within the same frame. In practice, compute once per frame **before**
the AI services run. [H] for no caching, [M] for the safe caching rule.

### 6.4 Replacing the headless stand-ins (`engine/apps/headless`)

| headless stand-in | real source |
|---|---|
| `value[i]` (random, sometimes negative) | Node_Weight input of §6.2. Element i of the AI's galactic planet-target list is assumed to be finder index i (PHASE2_ANSWERS "not resolved") |
| `view.dangerous` | starbase rule first (GALACTIC.md §3), then `HasSpaceForce` ≠ 0 |
| `view.ai_perceived_non_neutral`, `view.ai_eval_failed` | `TargetPlayerID` resolved to a player and then to faction `Is_Neutral` |
| `view.held_by_ally`, `view.unowned_or_neutral` | real ownership (§4) and the relation table (PHASE2_ANSWERS Q5) |
| `view.corrupted`, `benefits_from_corruption` | data pack +0x2f4 ≠ −1; faction +0x10b |
| `view.revealed` | data pack +0x2c9 / +0x2e0 and the fog option (PHASE2_ANSWERS Q6) |

**What is still missing for S2:** the semantics of `EnemyForce.*`. That is the planet force evaluator
(`VPlanetForcePerceptualEvaluatorClass` and its Visible/Fogged wrappers in SUBSYSTEMS.md, computed in
`PerceptionFunctionG.dll`). It needs its own spec.

Interim model [L]:
- `SpaceTotalUnnormalized` = Σ `AI_Combat_Power` of space units in orbit owned by players hostile to Self, as last
  seen by Self under fog;
- `GroundTotalUnnormalized` is the same on the surface;
- `HasSpaceForce` = (`SpaceTotalUnnormalized` > 0);
- `TargetPlayerID` = the owner id as last seen by Self.

> **Status 2026-10-30:** `engine/ai/src/galactic_perception.cpp` now implements this interim EnemyForce model (`SpaceTotalUnnormalized`, `GroundTotalUnnormalized`, `HasSpaceForce`, `HasGroundForce`, `TargetPlayerID`) and the four consumer paths, and `engine/ai/src/galactic_ai_tokens.cpp` evaluates the token leaves, so "what is still missing for S2" and the interim model above are implemented; the semantics stay [L] until a Q2 perception probe settles them.

**Q2 probe for S2 (recommended).** Add a mod perceptual-equation file with three equations, one per token path
(`Variable_Target.EnemyForce.SpaceTotalUnnormalized`, `….HasSpaceForce`, `Variable_Target.TargetPlayerID`). From a
galactic Lua script, call `Evaluate_Perception(name, ai_player, planet)` (0x699430) for every planet each fiscal
cycle and log the results together with the orbit contents. This settles the token semantics, the fog behaviour
and real value ranges, and with them whether wrap-around in Node_Weight ever happens.

---

## 7. Open items and Q2 traces

| # | question | trace that settles it |
|---|---|---|
| 1 | Initial `next_cycle_frame`; save/load of +0x368..+0x370 | log credits per frame for the first 3 frames of a campaign, and across a save/load at mid-cycle |
| 2 | Is the black-market LCG (0x140a13e24) the synced game RNG? | MP desync test, or read the state before and after a cycle with a debugger |
| 3 | Fleet speed multiplier M (object +0x2b8 → +0x20 → +0x230) — **answered**: `Object_Max_Speed_Multiplier_Galactic` (STRATEGIC_GAPS §2.1); a hop timing probe would only confirm | time one hop of a known distance, speed and route |
| 4 | Upkeep or maintenance deduction — **answered: none** (STRATEGIC_GAPS §2.3) | — |
| 5 | Refund on re-evaluation, starbase loss and capture | queue items, then destroy or capture; log credits |
| 6 | Ground-cap stall condition | 10 units on a planet plus one queued, with and without enemies present |
| 7 | Carrier fill order and the stay-behind split | send a fleet with excess fighters |
| 8 | What transfers on capture (0x4b7670 / 0x4b7860; corruption apply/remove 0x4b72e0 / 0x4b7260) | invade a fully built planet; list objects before and after |
| 9 | Orbital-control capture (0x375220) and the surface-inaccessible planets | move an unopposed fleet to such a planet |
| 10 | Strategic game speed effect | time 10 cycles at slider 0 and 4 |
| 11 | `EnemyForce.*` / `TargetPlayerID` semantics | the S2 probe of §6.4 |
| 12 | Tech-level gate inside 0x2804d0 — **answered**: full rule order in STRATEGIC_GAPS §1.3 (tech = step 16, plain ≥) | — |

> **Status 2026-10-30:** Row 3: the port now applies M (`engine/galactic/src/strategic.cpp:39`, `kApplyModeSpeedMultiplier = true`).

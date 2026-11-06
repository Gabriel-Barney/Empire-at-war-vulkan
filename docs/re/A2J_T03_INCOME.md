# A2j t03 / t06: Heart of the Maw income (585 vs 667.5) and the starbase owner (2026-11-06)

Scenario: `Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel with no orders, `--story`, seed 1, `--workers 0`
(`alamo_headless --economy --galactic-ai --story --campaign Sandbox_Heart_Of_The_Maw_Rebel --human Rebel --ticks N --workers 0 --seed 1 --quiet`).
Port: branch `claude/a2j-laneB` on 5509c2a (a2i). Traces came from temporary `fprintf` patches in `engine/galactic`
(per-planet income breakdown at each payment, starbase state at the start / end of every `GalacticSim::update` and before
each planet's production hook, every owner change). The patches were removed afterwards.

Confidence tags: [H] read or measured directly; [M] inferred from several consistent measurements; [L] hypothesis.

## 1. t03: the Empire's income at the first fiscal payment

### 1.1 Result

**No income term deviates.** At 5509c2a the port pays the Empire **667.5 at frame 1350**, the same as retail
(a2f t07 probe: 14.8333 / s for 7 cycles) [H]. The 585 in `A2I_T03_TRAJECTORY.md` (§1 L2, §4.1, §4.4 item 3) was measured
at cd13efb (a2h). In that trajectory the Underworld took **Kashyyyk** at frames 876–878 (the same doc's D4, §5.1). Kashyyyk is worth
exactly 82.5 to the Empire, and 667.5 − 82.5 = 585 [H arithmetic, M that this was the cd13efb state: an export of cd13efb did not
build here with the reduced module list, so it was not re-run].

So the statement "both hold the same four planets until 1350" in A2I_T03 §4.4 item 3 does not hold for the a2h run. The D1 fix
(typed Contrast rows, in 5509c2a) removed the early Felucia / Kashyyyk swap. Now no planet changes owner before frame 1413.

### 1.2 Per-planet breakdown at frame 1350 (port, measured) [H]

GALACTIC_ECONOMY §1.4 terms. All black-market multipliers are 1.0, all control is 1.0, no halt, no siphon, and Force_Strength is 0 on
every planet, so the force term is 0:

| planet | base | trade (Credit_Gain_Factor 0.5 per owned neighbour) | ability | sum |
|---|---|---|---|---|
| Corellia | 105 | 105 (Corellia_Kuat, Corellia_Tatooine) | 210: `Empire_Mining_Income_Bonus`, Percentage 2 × base, Corellia is no mining colony | 420 |
| Kuat | 75 | 75 (Corellia_Kuat, Kashyyyk_Kuat) | | 150 |
| Kashyyyk | 30 | 15 (Kashyyyk_Kuat; Felucia_Kashyyyk and NalHutta_Kashyyyk lead to Underworld planets) | | 45 |
| Tatooine | 35 | 17.5 (Corellia_Tatooine) | | 52.5 |
| **Empire** | | | | **667.5** |

The same numbers explain the earlier port values:
- **Without Kashyyyk:** −(30 + 15) for the planet itself, −37.5 for Kuat's Kashyyyk link, so −82.5 → **585**.
- **E1 run, with NalHutta added:** +125 base, +62.5 for its Kashyyyk link, +15 for Kashyyyk's link to it, so +202.5 → **870** (A2I_T03 §1 L2).
- **A2G_T02_T04:** Felucia gained (+85; its only route goes to the lost Kashyyyk) and Kashyyyk lost (−82.5), so **+2.5** → 670.

The retail total (667.5) matches the port's total. Retail's per-planet split was not probed [M].

New test: `test_foc_hotm_rebel_income` (`engine/galactic/tests/test_economy.cpp`, FoC data). It runs an economy-only start of the
Rebel entry, then checks the first payment (667.5), the four planet incomes and the mining row (210).

### 1.3 Later cycles (port, 5509c2a) [H]

| payment frame | Empire | Underworld | Rebel | owner changes before it |
|---|---|---|---|---|
| 1350 | 667.5 | 2105.0 | 337.5 | none |
| 2700 | 702.5 | 1075.8 | 337.5 | NalHutta U→E 1412, Shola U→E 1901, NalHutta E→U 2133 |
| 4050 | 702.5 | 1100.3 | 337.5 | |
| 5400 | 702.5 | 1988.5 | 337.5 | |
| 6750 | 959.1 | 861.6 | 337.5 | NalHutta U→E 6665 |

- 702.5 = 667.5 + Shola's base 35. Shola has no route to an Empire planet.
- Retail's Empire stays at 667.5 for 7 cycles and holds its four planets through frame 7525.
- The port's Empire captures NalHutta at 1412 and Shola at 1901: the E1 Conquer_Pirate NalHutta / Shola of A2I_T03 §4.2.
- **This is a trajectory deviation (galactic AI), not an income term.** It belongs to the AI lanes.

### 1.4 Underworld, first payment: 2105 vs retail about 2150 [M]

- **Retail.** The a2f t07 probe reads an Underworld income rate of 47.8 / s at the first payment (A2F_T07 §1 item 5). That is
  2149.5 .. 2153.9 per 45 s.
- **Port.** 2105, made up of:
  - Felucia 85 + palace 595;
  - NalHutta 125 + palace 875;
  - Shola 35 + palace 245 + trade 17.5;
  - The_Maw 85 + trade 42.5.
  - The `Underworld_Palace_Income_Bonus` is 7 × base.
- **The gap of about 45** is exactly one corruption income share on Naboo:
  - Naboo's gross income is 90 (Rebel, no route to another Rebel planet).
  - Intimidation (type 0) has `Corruption_Choice_Income_Percentage` 0.5, so 90 × 0.5 = 45.
  - 2105 + 45 = 2150 = 47.78 / s.
- **The other target does not fit.** The second Corrupt_Planet target, MonCalimari, would give 112.5 × 0.8 (Piracy) = 90, or 112.5
  (Bonded_Citizens). Neither matches.
- **What the port does** (`Corrupt_Planet @Naboo`, plan at frame 116):
  - The saboteur team reaches Naboo and fires its Ground_Activated abilities at frame **993**.
  - `Saboteur_Galactic_Sabotage` is not accepted.
  - `Saboteur_Corruption` is accepted. Its AI pick then finds both Naboo choices unaffordable:
    - Intimidation costs 800 and needs Silri; Silri is present (1/1).
    - Bonded_Citizens needs 2 Piracy missions.
    - The Underworld holds **688** credits.
  - Nothing is applied ("not applied" in the AI log), and the plan ends in failure at 1293.
- **Interpretation [M].** Retail most likely corrupted Naboo before frame 1350. The galactic corruption code behaves as specified
  (charge credits, then units: GRC 4.3 / P4 5.5). The difference is the Underworld's credits at the saboteur's arrival, which is AI
  spending and timing.
- **Retail question:**
  - What are the Underworld's credits at the frame its saboteur reaches Naboo?
  - Which corruption type does Naboo get, and on which frame?
  - Does the DeploySaboteur plan reserve or wait for the corruption cost?

## 2. t06: `PlanetState::starbase_owner` = -1 while the starbase object lives

### 2.1 Cause [H code, M spec]

`GalacticSim::on_owner_change` (`campaign.cpp`, the derived-owner change, 0x3fb040 in GE 4.2) reset `starbase_owner` to −1 on
**every** owner change. It did so whether the starbase object survived or not. The comment cited GE 2.8.

The spec ties that reset to clearing the starbase:
- GE 2.8 names 0x3f7280 (`Change_Owner`) and 0x3f98a0 (planetary revolt).
- `Change_Owner` clears the starbase first (0x4bda40, GE 4.2 "Forced transfers").
- BATTLE_TRANSITIONS §3.3a: 0x4bda40 sets +0x138 / +0x13c to −1 **together** (handle and owner), along with type +0x140 and level +0x148.
- GE 4.2's list for the derived-owner change ("On an owner change", [H] order and calls) has no starbase step.

So a starbase that outlives a derived owner change keeps its owner in retail [M]. The port instead left a live starbase with
`starbase_owner` −1. That had two consequences:
- GE 4.1 step 3 (the starbase owner) stopped counting.
- The space retreat in `conflicts.cpp` no longer destroyed the loser's starbase (`starbase_owner == side`). Every other
  `starbase_owner` reader in galactic / conflicts was affected the same way.

**When it happens:** a derived owner change while a starbase lives. That means a ground unit (or structure) of another player on
a planet whose starbase survives. For example, a story / Lua spawn or a hero respawn under a foreign starbase, or the starbase
owner's own ground units leaving or returning.

### 2.2 Fix

`on_owner_change` now resets `starbase_owner` only when no starbase object is alive. That is a no-op in the normal case, because every
destroy path already clears it. A live starbase keeps its owner. `Change_Owner` (`story_ops.cpp`) is unchanged: it destroys the
starbase first and then resets the owner, as 0x3f7280 does. The space-queue re-evaluation at level 0 stays as it was (GE 2.8 [M],
see §3).

New test: `test_a2j_starbase_owner` (synthetic):
1. An Empire ground unit is spawned under the Underworld's starbase.
2. The planet goes to the Empire. The starbase and its owner stay the Underworld's.
3. The unit is destroyed, and GE 4.1 step 3 hands the planet back to the Underworld.
4. Destroying the starbase clears both the handle and the owner.

### 2.3 HotM [H]

None of these runs reproduce the state (live starbase, owner −1) at 5509c2a, before or after the fix:
- the story run (9500 and 20000 ticks, seeds 0–3);
- the HotM human script run (36000 ticks);
- the all-AI run (18000 ticks).

The checks ran at the start and end of each galactic frame and before every planet's production hook, where the AI market runs.
- **Planet 8 (NalHutta) near frame 6657.** In the story run its Underworld starbase is destroyed at 6571 by the destroy path
  (owner −1, handle cleared), and NalHutta changes owner (U → E) at 6665.
- **The a2i observation.** It was presumably made on an intermediate a2i trajectory in which such a capture happened while a
  starbase lived. That is the path fixed above [M].
- **Goldens.** The final CRCs of the three HotM runs above are identical before and after the fix, so no golden moves.

**AI workaround.** With the fix, `ps.starbase_owner` equals `store.owner(ps.starbase)` whenever the starbase lives. The one
exception is a direct `ObjectStore::set_owner` on the starbase object outside GalacticSim, and no such caller exists. The
`st.owner(starbase)` read in `engine/ai/src/galactic_ai_tokens.cpp` can therefore go back to `ps.starbase_owner` (D +0x74), or
stay as it is: both give the same result.

## 3. Open items

1. [M] The Underworld's first-cycle income: the Naboo corruption at frame 993 fails on credits (688 < 800). See §1.4 for the
   retail question. This is for the AI lanes and a probe.
2. [M] The Empire's NalHutta (1412) / Shola (1901) captures move the port's income off retail's flat 667.5 from cycle 2 onward. This is AI
   trajectory (A2I_T03 E1 / D4 family).
3. [L] GE 2.8 attributes the space-queue re-evaluation at level 0 on capture to 0x3f7280 / 0x3f98a0, as it does the starbase reset.
   Whether the derived-owner change 0x3fb040 also re-evaluates the space queue (or only calls 0x403490 when control drops below 1,
   GE 4.2) is not traced. **RE question:** does 0x3fb040 call 0x402490 / 0x402660 (the queue re-evaluation) on an owner change, and
   with which level?
4. Docs to correct (not edited here):
   - GE 2.8: "the starbase owner is reset" applies to 0x3f7280 / 0x3f98a0 (through 0x4bda40), not to 0x3fb040.
   - A2I_T03 §1 L2 and §4.4 item 3: the 585 is Kashyyyk's loss at 876–878.

# A2i t03: early-game trajectory, port vs retail (Heart of the Maw, Rebel human)

Scenario: `Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel with no orders, `--story`, seed 1, `--workers 0`.

- Port: HEAD cd13efb (a2h), traced from a scratch `git archive` export. No `engine/` file in the repo was changed.
- Retail:
  - Run `tools/probe_mod/a2f_budget/out/20261106-053820` (frames 0..7525). It has Empire story samples every frame early and every 30 frames later, plus MJ records.
  - Run `out/20261106-040725` covers the long horizon (ConquerPirate starts).

Confidence tags: [H] read or measured directly; [M] inferred from several consistent measurements; [L] hypothesis.

## 1. Summary

| # | Finding | Kind | Conf. |
|---|---|---|---|
| D1 | `galactic_ai_enemy_strength` adds starbase and ground-structure power only for the all-categories mask. Retail (0x5f2430) adds them to each typed Contrast row whose mask matches. Felucia's Capital row is therefore 0 in the port instead of about 12650, so the Empire can fund `Conquer_Pirate @Felucia` at frame 42. | port deviation | H spec, M causal |
| D2 | Because of D1, the Empire spends its whole Offensive pool on 16 slow, cheap ground units for Conquer_Pirate. Retail spends it on space ships, most likely Tartan + Broadside. | consequence of D1 | M |
| D3 | The Empire's Infrastructure spend in frames 0..1350 is about 1130 in the port and about 3400 in retail. The port's Magnepulse at Tatooine waits behind a ground unit in the Tatooine ground queue (ordered at 837, not about 103). Its Heavy Vehicle Factory goal is then dropped four times by the time limit. | port deviation, cause partly open | M / L |
| D4 | The Underworld's `Conquer_Pirate @Kashyyyk` turns positive only through markup from `Distant_System_Path_Bonus`. It is accepted at 323 and takes Kashyyyk at about 877. Retail shows no Underworld ConquerPirate before 54583. | port deviation, mechanism open | M (deviation), L (cause) |
| D5 | Underworld `Upgrade_Starbase`: proposed from frame 17 but loses Infrastructure to the two palaces. Port: none in 3000 frames. Retail: first plan at 2794. | open | L |
| L1 | The weighted pick between Build_Space_Forces plans (Acclamator vs Tartan + Broadside) | synced random draw | M |
| L2 | The E1 Empire income of 870 at frame 1351 (vs 585) comes from NalHutta, which E1 had already conquered. It is not a separate bug. The base 585 vs 667.5 gap is the t02 question and is not resolved here. (a2j, `A2J_T03_INCOME.md`: the 585 is the Kashyyyk loss to D4, 82.5 per cycle; no income term deviates.) | consequence / open | M |

## 2. Method

- **Export.** `x\` = `git archive HEAD`, plus the `a2g_trace_patch.py` traces, plus these scratch patches:
  - `ALAMO_A2I_GOALS` / `ALAMO_A2I_MAXF` / `ALAMO_A2I_SUBEQS`: per-candidate desire with sub-equation values (`A2I CAND`), accept-tail failures, and "no PP" failures.
  - `ALAMO_A2I_PP`: PP Contrast entries and the selection (`A2I PP`).
  - `ALAMO_A2I_MK`: `Apply_Markup` calls (`A2I MK`).
  - `ALAMO_A2I_SBTYPED=1`: experiment E1. For typed masks, add starbase and structure power when `category_mask & mask != 0`.
  - `ALAMO_A2H_EXTRA_EQS`: extra token equations (`X_*`).
- **Command:** `alamo_headless --economy --galactic-ai --story --campaign Sandbox_Heart_Of_The_Maw_Rebel --human Rebel --ticks N --workers 0 --quiet --ai-log runK.log 2> runK.err`. run3 / run5 are HEAD. run4 is E1. run6 traces markup. run7 traces Upgrade_Starbase.
- **Scratch location:** `C:\Users\Gabriel\AppData\Local\Temp\claude\2c69982e164d12db\5ea282a5-a87f-490b-8d9e-7c5319dc6e80\scratchpad\a2i_t03\`. This holds the patches (`tools\patch_*.py`), the run scripts and the logs. Reproduce by re-applying the patches to a fresh export.

Map facts used below:
- Empire (p1): Kashyyyk, Kuat, Corellia, Tatooine.
- Rebel (p0, human): Kamino, Yavin, Naboo, MonCalimari.
- Underworld (p5): NalHutta, The_Maw, Shola, Felucia.
- There are no pirate planets. `Conquer_Pirate` targets Enemy|Neutral, so for the Empire it means the Underworld's planets.

## 3. Q1: the Empire's `Conquer_Pirate @Felucia` at frame 42

### 3.1 Port values [H]

**Desire.** At f=2, `Should_Conquer_Pirate_Planet(Felucia)` = 5.9422.
- That is `Is_Good_Invasion_Target` = 8 × GenericPlanetValue 0.6178 + 3 × TradeRoutes 0.3333.
- No-force term 0: Felucia has a space force and a ground force.
- Opens_New_Front 0: Felucia's only neighbour is Kashyyyk, which the Empire owns.
- Is_AI_And_Should_Be_Ignored 0 (NMS is 0 since a2h).
- Markup 0.
- The goal's time limit is 90 s and its tolerance 1.3.

**Competing goals.**
- Build_Space_Forces desire 1.0: PP Acclamator, net 2500.
- Build_Ground_Forces desire 0.5: net 450.

**Conquer_Pirate PP solves:**
- avail 3105.7, net 2760, gross 21290.
- eta 232 against AllowedTime 736 × 1.3, so the time check does not bind.
- nsel 31.
- Free units used: Accuser, 2 Victory, 4 Tartan and the ground units.
- Builds: 13 Imperial_Anti_Infantry_Brigade and 3 Imperial_Heavy_Scout_Squad.
- Contrast entries: `[-1S 16050] [-1G 3296] [0S 1518] [1S 569] [3S 3163] [4S 1771] [10S 633] [6G 615] [7G 266] [10G 2530]`. There is **no Capital (5S) row and no Structure (9G) row**.

**Resolve.** Conquer_Pirate (5.94) is first in the greedy phase and takes the Offensive pool. The phase then stops at Build_Space_Forces, which can no longer be funded.

### 3.2 Retail [H unless noted]

**Plans.** No ConquerPirate PSTART for the Empire in 053820, through frame 7525.
- In 040725 the first ConquerPirate trace is near 17179, then PSTART at 17404.
- PLCU stays 4 and NMS 0 throughout.

**Credits.** Flat at 8000 until frame 102, then they fall by pay-as-you-go installments.

| frame | credits | Offensive pool | Infrastructure pool |
|---|---|---|---|
| 0 | 8000 | 3105 | 4000 |
| 241 | 6620 | 2112 | 3614 |
| 399 | 4858 | 788 | 3175 |
| 554 | 4145 | 505 | 2745 |
| 709 | 3678 | 470 | 2314 |
| 1019 | 2685 | 338 | 1453 |
| 1329 | 1775 | 288 | 592 |
| 1350 (after payment) | 2381 | | |

- **Offensive** drains at about 250 cr/s from about frame 103 to about 430, then almost stops. 2817 is spent by 1329.
- **Infrastructure** drains at a constant 430 per 155 frames (83 cr/s) from about 103 to 1329: about 3400 spent.
- **Space strength.** FRT and SST rise by exactly +2650 at frame 678; HST is unchanged.

**Most likely Offensive build [M]: Tartan + Broadside.**
- Cost 1250 + 1800 = 3050, about the 3105 pool.
- Pay-as-you-go rate 1250/12 s + 1800/12 s = 254 cr/s (Galactic_Build_Time_Multiplier 0.8 on 15 s).
- Power 1250 + 1400 = 2650, which equals the FRT step.
- An Acclamator (2500 at 104 cr/s, power 2500) fits neither the rate nor the power.

So retail funded Build_Space_Forces (and Infrastructure) at the start, not Conquer_Pirate.

### 3.3 D1: typed Contrast rows skip starbase and structure power [H spec, M causal]

**Spec.** A2F_RE_READS §3.1 describes 0x5f2430 as follows:
- mask = param_3, and 0 means all bits.
- Space: add the starbase type's AI_Combat_Power if `(starbase category mask & mask) != 0`.
- Ground: add the matching ground structures plus the garrison (0x4b8000).
- The galactic contrast list (0x5f1990) calls only 0x5f2430, so the typed rows get the mask too.
- The table row in §3.1 says "port's formula matches", which is wrong for typed masks.

**Port.** `engine/ai/src/galactic_ai_tokens.cpp`, `galactic_ai_enemy_strength` (line 127) returns before adding the starbase and structures whenever `mask != kAllCategories`:

```cpp
double s = galactic_ai_planet_force(sim, types, planet, self, true, space, mask);
if (mask != kAllCategories || static_cast<size_t>(planet) >= sim.planets().size()) return s;
```

This was introduced in 73a90b3 (A2 f t03, 2026-11-02).

**Effect at Felucia.** Underworld_Star_Base_2 has power 10000 and CategoryMask Capital. The Capital row should be 10000 × (1.15 + fp) × 1.1 ≈ 12650. The row weights are AntiCapital 2.0, Corvette 0.33, Frigate 0.5 and Capital 1. Against those weights the Empire's free ships score about 7950. Filling the row needs builds well above the 3106 Offensive pool, so the Budget marshaller vetoes the PP and Conquer_Pirate gets no plan.

**Experiment E1** (`ALAMO_A2I_SBTYPED=1`, run4) [H]:
- At frame 42 the Empire runs Build_Space_Forces (Acclamator, 2500) and Build_Ground_Forces (450) instead of Felucia.
- That matches retail's use of the two pools. The ship type differs (L1).

**Proposed port change.**
- `engine/ai/src/galactic_ai_tokens.cpp` `galactic_ai_enemy_strength`: for a typed mask, add `power(starbase)` when `types[starbase].category_mask & mask` is non-zero, and the same for each ground structure (and the garrison, if modelled).
- Correct the A2F_RE_READS §3.1 table row.
- Regenerate the galactic and skirmish goldens.

## 4. Frames 0..1350: which builds the Empire starts, port vs retail

### 4.1 Port HEAD (run3) [H]

Orders that reached a production queue:

| frame | order | budget |
|---|---|---|
| 93 | Imperial_Anti_Infantry_Brigade @Tatooine | Offensive (Conquer_Pirate) |
| 155 | E_Ground_Barracks @Kashyyyk | Infrastructure |
| 186 | Anti_Infantry_Brigade @Kuat, Stormtrooper_Squad @Corellia | Offensive / Build_Ground_Forces |
| 558 | Stormtrooper_Squad @Corellia | |
| 775 | Anti_Infantry_Brigade @Kuat | |
| 837 | Ground_Magnepulse_Cannon @Tatooine (task made at 76) | Infrastructure |
| 1209 | Light_Scout_Squad, Acclamator_Assault_Ship @Corellia | Build_Space_Forces after Conquer_Pirate ended at 1046 |

- Build tasks are made, but no order is placed, for:
  - 16 Conquer_Pirate ground units (all at frame 43);
  - `E_Ground_Heavy_Vehicle_Factory` @Tatooine (154, 721, 886, 1078, 1321);
  - `E_Ground_Light_Vehicle_Factory` @Kashyyyk (721).
- Pay-as-you-go drain is about 32 cr/s. The cheap ground units are slow to drain: an AIB costs 180 over 24 s, which is 7.5 cr/s.
- Spent by 1329:
  - Offensive pool 3099 → 1947: about 1150.
  - Infrastructure pool 4000 → 2870: about 1130.
- Credits at the 1351 payment: 6016, after a gain of 585.

### 4.2 Port E1 (run4) [H]

- At 43: Acclamator plus 4 ground units for ReserveForce, ordered at 93 (Acclamator @Corellia).
- At 613: Empire_Star_Base_2 @Kashyyyk, dropped at 790.
- Then Conquer_Pirate NalHutta (775) and Shola (1036). Both planets were emptied by the Underworld's own Kashyyyk attack (D4). Retail never shows these.
- Infrastructure still stalls: the HV and LV factories and the Barracks are retried and released every 80–160 frames. The Barracks is ordered only at 496 and the Magnepulse at 837.

### 4.3 Retail

- Offensive: about 3050 on two space ships early (Tartan + Broadside [M], §3.2).
- Infrastructure: one or more structures drain 83 cr/s steadily from about frame 103 to 1329, about 3400 in total [H for the rate, L for the items].
  - Ground_Magnepulse_Cannon (2000 over 48 s = 41.7 cr/s) can make up at most half of that rate.
  - The rest is unidentified: the probe does not log build orders.
- The first `BuildBaseComponentLightVehicleFactory` PSTART is at 1435.
- `BuildBaseComponentHeavyVehicleFactory` is still being retried at 6172 (instance n=5, unallocated 1000). Retail therefore also fails to build the HV factory for a long time [H].

### 4.4 Why the port starts fewer builds

1. **D1 → D2 [M].** The Offensive pool goes to 16 ground units that drain 7.5 cr/s each, and only one at a time per planet ground queue (AI_GALACTIC_2 §2.3: production starts only when the queue is idle). Retail spends the same pool on two ships at 254 cr/s.
2. **D3, the Tatooine ground queue [M for the mechanism, L for the root cause].**
   - Build_Weapon's task is made at 76, 34 frames after its plan (42).
   - By then a ground-unit task has claimed Tatooine's idle ground queue (AIB at 93, 720 frames).
   - The Magnepulse (a structure, bound to the goal target) can only start at 837.
   - The Heavy Vehicle Factory at Tatooine then queues behind both. `Build_Base_Component_Heavy_Vehicle_Factory` is dropped by the time limit (`why=time`, eta_el 119.5 / 100.6 / 144.0 / 185.9 s against AllowedTime 72) at 610, 772, 922 and 1168.
   - The same stall appears in E1, so it does not depend on D1.
   - Retail's 83 cr/s Infrastructure drain from about 103 shows that its first structure started almost at once.
   - Open questions:
     - Does retail's Build_Ground_Forces / Conquer_Pirate market bid avoid Tatooine? (This needs §2.3 step 4 bid inputs: queue time, travel, base level K, cost ratio.)
     - Does the BuildWeaponStructurePlan's task appear earlier in retail?
   - Proposed check (port):
     - Trace the `galactic_build.cpp` market (bids and the starting task per planet queue) for frames 40–200.
     - Compare the bid formula with AI_GALACTIC_2 §2.3 step 4, especially whether a ground-unit task bids on a queue that a pending structure task for that planet is waiting for.
   - Proposed probe (retail): log the Empire's planet queue heads every 30 frames in frames 0..1400.
3. **Income [open].** 585 vs 667.5 is not explained by the planet list: the Empire owns the same four planets in both games through 1350. See t02. (Corrected by a2j t03, `A2J_T03_INCOME.md`: in the port the Underworld took Kashyyyk at 876–878, so the port's Empire held three planets at the payment; 667.5 − 82.5 = 585.)

## 5. Q2: the Underworld's early `Conquer_Pirate @Kashyyyk` (D4)

### 5.1 Port values [H]

**Desire without markup:** −0.3067. The terms are:
- 8 × GPV 0.5867 = 4.69
- 3 × TradeRoutes 1.0 = 3
- 2 × ConnectsIsolatedPlanetsByForce 1 = 2
- −(5 + 5 × AnyCurrentThreats 1) × Opens_New_Front 1 = −10

**Markup from `Distant_System_Path_Bonus`** (PathBonusForDistantTargets.lua, `Apply_Markup` along `Find_Path(_FindStageArea, planet)` with value (GPV + Neglected)/2, one planet per `Sleep(1)`):

| frame | markup added | desire |
|---|---|---|
| 222 | Kashyyyk's own value | +0.2933 |
| 252 | Kuat path (+0.3244) | 0.6178 |
| 372 | Corellia path | 1.0122 |

**Plan and outcome:**
- Accepted at 323; plan at 410 with MainForce of 13 free units, net 0.
- With E1 the force is 21+1 units, including units from The_Maw and Shola; the 6325 Capital row is met.
- The Acclamator is lost at 867; Kashyyyk changes owner at 876–878.

### 5.2 Retail [H]

- No Underworld ConquerPirate PSTART until 54583 (040725).
- The Empire holds 4 planets through 7525 (053820), with no space losses before about 17179.
- The Underworld's budget spend matches the port closely (credits at 241: 5416 port vs 5667 retail; at 481: 3332 vs 3840; at 724: 1619 vs 1737). So the difference is in goal choice, not money.

### 5.3 Status [L]

The port's markup puts Kashyyyk above 0 within about 230 frames; retail's apparently does not. Candidates, unranked:
- the planets `Find_Path` / `_FindStageArea` return (path content and staging area);
- markup accumulation or decay;
- ConnectsIsolatedPlanetsByForce = 1;
- free-store eligibility of units across the two route components (The_Maw / Shola vs NalHutta / Felucia).

**Proposed retail probe:** from the story Lua, call `EvaluatePerception` for UW → Kashyyyk on Should_Conquer_Pirate_Planet, Is_Good_Invasion_Target, `Variable_Target.Markup`, ConnectsIsolatedPlanetsByForce and Opens_New_Front, every 30 frames in frames 0..600. Do the same for Empire → Felucia.

No port change is proposed until that probe runs.

## 6. Other items

- **D5 Underworld Upgrade_Starbase** [L]:
  - Accepted from frame 17 (desire 34, net 1600), but in resolve it loses Infrastructure to `Build_Underworld_Palace` ×2 (2000 each).
  - Port: no plan in 3000 frames. Retail: a plan at 2794 (040725 / 053820 plans.tsv).
  - This may be pure budget timing.
- **First spend.** The port's Empire makes build tasks at 43 and first orders at 93; retail starts spending at 103. That is within one market period (30 frames) of the port's orders, so not a deviation on its own [L].
- **L1** [M]: the plan choice inside Build_Space_Forces (Acclamator vs Tartan + Broadside) is a weighted synced random draw. A different draw is legitimate once the RNG streams diverge, and they do (D1 changes the draws from frame 42).

## 7. Proposed order of work

1. D1 fix (`galactic_ai_tokens.cpp`), then re-run this trace. Expect: no Felucia plan at 42 and Offensive spent on space.
2. Trace the D3 market on Tatooine (`engine/ai/src/galactic_build.cpp`) and compare it with AI_GALACTIC_2 §2.3.
3. Run the retail probes for D4 (perception at Kashyyyk / Felucia) and D3 (queue heads).

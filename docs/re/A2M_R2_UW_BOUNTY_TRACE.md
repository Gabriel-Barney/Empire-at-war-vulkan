# a2m r2: Underworld credit gains outside the fiscal cycle in retail HotM (trace finding)

Date: 2026-11-12. Source: the existing retail Q2 reference traces of the hotm_galactic parity scenario
(`tools/probe_mod/q2_traces/out/20261030-061228-, 20261108-170824-, 20261108-204711-, 20261108-183748-galactic-vanilla-lua`,
Underworld = player 6). No decompiled code was read; everything below comes from `trace.q2t` / `production.q2q` and the
shipped XML. Tags: [H] = exact match in every run, [M] = inferred from the trace, [L] = guess.

## 1. Battle bounty in AI-versus-AI galactic auto-resolve

Retail Underworld credits rise outside a fiscal-cycle frame exactly when an auto-resolved battle it fought removes enemy
objects. Both events before tick 9000 are identical in all four runs (the runs are deterministic that far):

| mode frame | battle | enemy (Rebel) objects removed that frame | sum of their bounty | Underworld credit change |
|---|---|---|---|---|
| 7162 | space, Underworld fleet vs Rebel fleet (both sides lost units) | MARAUDER_MISSILE_CRUISER (Corvette 50), CORELLIAN_CORVETTE (Corvette 50), 5 X-WING (Fighter 15), 9 Y-WING (Bomber 15) | 310 | +308.3 = 310 − one 1.67 PAYG installment paid the same frame |
| 7497 | land at Shola, Rebel invasion; the Underworld **lost** the battle and the planet (owner 6 → 0 at 7498) | 4 GALLOFREE_TRANSPORT (Override_Bounty_SP 50), 12 SQUAD_PLEX_SOLDIER (Infantry 5), 15 SQUAD_REBEL_TROOPER (Infantry 5), 5 T2B_TANK (Vehicle 50) | 585 | +585.0 |

Rules this gives:
- The Underworld receives a bounty for each enemy object destroyed in its galactic auto-resolve, winner or loser. [H]
- Bounty of one object = its `Override_Bounty_SP` when set, else the amount of the first `Default_Bounty_By_Category_SP`
  entry (GameConstants document order: Infantry 5, Vehicle 50, Air 50, Structure 150, Fighter 15, Bomber 15, Corvette 50,
  Frigate 250, Capital 500, SpaceHero 500, LandHero 500) that the object's `CategoryMask` contains. The same rule as
  `battle_summary.hpp` `bounty_of`. [H for the values above]
- It is counted per galactic object as retail holds them: each fighter of a squadron (the GALACTIC_SQUADRON holder gives
  0), each `Company_Units` member of a ground company and each company transport (`Company_Transport_Unit`, Override 50).
  [H for these two battles]
- Multiplier 1.0 and integer sum. This matches `Underworld_Racketeering_Corruption_Upgrade` (UPGRADEOBJECTS_UNDERWORLD.XML):
  `Bounty_On_Faction_Ability` `Underworld_Racketeering_Salvage_Effect`, `Bounty_Percentage_Amount` 1.0, targets Empire,
  Rebel, Hutts, Pirates, `Apply_To_All_Allies` No. The Underworld's `Automatic_Planetary_Corruption_Upgrade_Type` list
  (EXPANSION_FACTIONS.XML) puts this upgrade on Racketeering (type 3) planets, and every Underworld-owned planet starts
  with Racketeering corruption (GE 4.2). [M: which object carries the ability and when it is active are not traced]
- No credit for enemy **structures** removed by an ownership change (REBEL_GROUND_MINING_FACILITY at 1808 in run 061228,
  R_GROUND_BARRACKS / E_GROUND_BARRACKS in run 20261103-152619: no gain). [M]
- The Rebel and the Empire get nothing for destroyed Underworld objects in the same battles (no bounty ability). [H]
- Notify flag: bounty is notify 0 (A2K_T10_CREDIT_NOTIFY_RE Q3). [from that doc]

Ported 2026-11-12 (a2m r2, `engine/galactic/src/conflicts.cpp` `pay_auto_resolve_bounty`); the [port] choices for the
open points below are listed in `docs/PHASE4.md` "a2m r2". BATTLE_TRANSITIONS §3.5 is corrected.

Open for an RE task (docs only): the call site from the auto-resolve kill recording (BATTLE_TRANSITIONS §4.6, 0x60da50)
to the bounty function 0x392600; whether `Bounty_Percentage_Amount` abilities with the same `Stacking_Category` take the
maximum; whether the AI Credit_Multiplier applies (Medium = 1 cannot tell); BATTLE_TRANSITIONS §3.5 [M] "auto-resolve gives
no reward" contradicts this trace and should be corrected.

## 2. Production refunds when an Underworld plan ends

`production.q2q` shows retail removing an in-production Underworld queue entry early, with a credit rise the same frame:

| entry | queued | removed | Underworld credit change |
|---|---|---|---|
| UNDERWORLD_SABOTEUR_TEAM @ NalHutta | 1573 | 1583 (10 frames) | not resolved (below 50) |
| UNDERWORLD_SABOTEUR_TEAM @ Shola | 2839 | 2959 (120 frames) | +195.0 |
| UNDERWORLD_SABOTEUR_TEAM @ NalHutta | 4146 | 4253 (107 frames) | +171.7 |

The amounts equal the installments already paid (GE §2.6 PAYG cancel refund = paid fraction × cost, minus that frame's
other installments). [H amount; M that the trigger is the plan end] Other saboteur entries of ended plans (1697, 2844,
3087) were completed, so the cancel is conditional. This contradicts AI_GALACTIC_2 "Claiming the produced unit" ("task gone
→ the production finishes and goes to the free store"), which the port implements. Open for RE: which path removes the
entry with a refund (build-task end 0x64ad00, the PAYG reclaim flag +0xD9, the plan-end block destructor) and its
condition.

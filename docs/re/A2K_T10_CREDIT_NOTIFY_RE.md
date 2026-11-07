# a2k t10: Add_Credits "notify AI" path (0x4b0310 / 0x615b40)

Read 2026-11-07 from the decompiled output and from disassembly of the FoC executable (call sites, to recover the
notify flag where the decompiler dropped arguments). Clean-room: behaviour only. Addresses are RVAs.
Confidence tags as elsewhere in `docs/re/`: [H] read directly, [M] inferred, [L] hypothesis.

Background: GALACTIC_ECONOMY §1.1 (Add_Credits 0x27f370), AI_GALACTIC_2 §3 (budget layout, accrual, reweight),
AI_TACTICAL §1.3 (battle credit transfer).

---

## Q1. What 0x4b0310(aiSystem, amount) does

Called only from Add_Credits 0x27f370, and only when the notify flag is set **and** player +0x360 (AI system) is
non-null [H]. Steps [H]:

1. Look up the **galactic mode object**: 0x28a950(GMM, 0) walks the GMM mode stack (GMM +0x48, count +0x50) and returns
   the first mode whose type (vf +0xE0) is 0. This is "the galactic mode, if it exists anywhere in the stack", not the
   current mode. None (skirmish) → return, nothing happens.
2. Walk the AI system's per-mode context vector (+0x38 .. +0x40) in order; the first context whose mode
   (0x5f55e0 = 0x28a950(GMM, context +0x34)) is that galactic mode is chosen. None → return.
3. Context +0x40 (goal system) null → return. Otherwise call 0x615b40(goal system +0x190 = AIBudgetClass, amount).

Gates that are **not** there [H]: no sign or zero test on the amount, no is-human test (+0x62), no "context active"
test, no current-mode test. So it also runs while a campaign land/space battle is the current mode (the galactic mode
is still on the stack below it), and it always targets the **galactic** budget.

**Amount passed** [H]: the **applied** amount, i.e. Add_Credits' own working copy after its adjustments:
- positive and AI system present: a' = a × `Credit_Multiplier` (0x4b0500 +0x20);
- credits' = credits + a'; if credits' < 0: a' −= credits' (so a' = −old credits) and credits = 0;
- cap clamp (cap ≥ 0 and the mode test passes): if credits > cap, a' −= (credits − cap) and credits = cap.
So the passed value equals the actual change of player +0x70 (float arithmetic as written: subtract the overflow
from a', do not recompute new − old).

## Q2. What 0x615b40(budget, x) does

[H] For each category in the budget's category list (budget +0x30; the same hash-map node list, same order, that
0x614340 / 0x614590 walk; node +0x18 = category record):

    P(+0x4C) = x × w(+0x3C) + P        (single precision; multiply, then add)

That is the whole function. No other field changes: A (+0x44), R (+0x48), I (+0x50), applied weight (+0x38), goal
structs, and budget-wide +0x68 / +0x70 / +0x74 / +0x80 are untouched. Negative x is applied the same way (P drops,
can go negative). w is the normalised weight left by the last reweight 0x614590 (or Init 0x6137b0); the raw values
only sit in +0x3C during 0x614590 itself. Order does not affect the result (categories are independent).

## Q3. Callers of Add_Credits 0x27f370 and their notify flag

58 direct call / tail-jump sites in 47 functions (byte scan of the code section matches the call graph). Notify flag
read from the r8b setup at each site [H]; "what it is" is [H] where an existing doc names it, else [M]/[L].

**notify = 1 (the AI galactic budget P is updated):**

| caller | what | amount |
|---|---|---|
| 0x3fb040 (site 0x3fb875) | planet capture bonus to the new owner: `Planet_Capture_Bonus_Reward` (planet type +0x7D0), when the planet is not destroyed (+0x2c8), the owner's faction flag (+0x68 → +0x20a8) is set and 0x2825b0 is false | + |
| 0x456970 | story reward CREDITS | ± (story parameter) |
| 0x603130 | Lua `Give_Money` | ± (Lua number) |
| 0x4023d0 | galactic production refund (0x499000 amount), from cancel 0x403770 / 0x4035d0 / 0x403900 and the creation-failure refund in service 0x402840 | + |
| 0x496830, 0x4986a0 | tactical build manager teardown refunds | + |
| 0x498820 | tactical build cancel refund | + |
| 0x614a70 | battle-end return: credits := saved (+0x88), then Add_Credits(leftover − released(+0x8c)) (tail jump) | ± |
| 0x6c2d60 | tactical plan (PP mode +0x198 ≠ 0) charges its net cost PP +0x44 when resolved; stored at PP +0x1A4 [M meaning] | − |
| 0x5f6a50 | plan end returns PP +0x1A4 (goal +0x68 → PP) [M: 0 for galactic plans] | + |
| 0x434720 | blockade bribe: Σ `Cheap_Bribe_Cost`/`Expensive_Bribe_Cost` (type +0x2350/+0x2354) × AI adjustment +0x4C, only if ≤ credits [M meaning] | − |
| 0x498cd0 (2 sites) | multiplayer `Distribute_Credit_Quantum`: giver −q, each recipient +q/n | − / + |
| 0x2aa510 (2), 0x2aa8e0 (3) | linked-battle resolution (land / space) credit awards, (float)int from 0x3711c0 [M] | + |

**notify = 0 (budget not told):**

| caller | what | amount |
|---|---|---|
| 0x4b2ef0 | fiscal cycle income (then accrual 0x614340) | + |
| 0x3fff70 | galactic production enqueue charge (up-front) | − |
| 0x402840 | production service charge (non-AI PAYG installment / tech) | − |
| 0x53b900 | AI PAYG installment, scripted build (no task) | − |
| 0x64adb0 | AI PAYG installment via the build task's goal budget | − |
| 0x392600 (2) | bounty | + |
| 0x3fb040 (site 0x3fbeda) | `Corruption_N_Success_Credit_Bonus` (planet type +0x25e0/+0x2780/+0x2920 via 0x3716b0) | + |
| 0x3f9690 | corruption success credits | + |
| 0x52bf50 | corruption action cost | − |
| 0x5b13c0 | Remove_Corruption cost | − |
| 0x5b21a0 | sabotage cost | − |
| 0x618e30, 0x619160 | slicing failure / slicing cost | − |
| 0x61a4a0, 0x61ab60 | black market | − |
| 0x61b9f0 | Death Star hack | − |
| 0x616740 | hero neutralize (bounty-hunter path) cost | − |
| 0x6147d0 | `Release_Credits_For_Tactical` (tactical allowance during a battle) | + |
| 0x3c4460 (2) | raid fleet move (mover −, per-player +) [M] | − / + |
| 0x42dbd0, 0x496a20, 0x498170 | tactical build start / enqueue / PAYG service charges | − |
| 0x5248b0 | tactical sell | + |
| 0x387170 | repair service cost | − |
| 0x694ba0 (2), 0x694e90 | tactical credit streams | + |
| 0x3615d0 (4) | tactical (no parent) end-of-battle credit awards [M] | + |
| 0x6e5c50, 0x6e6240, 0x6ea030, 0x6ea620 | task force `Build` / `Build_All` (Lua) [L: refunds] | + |
| 0x5327a0, 0x621370 | unidentified [L] | + |
| 0x5b07c0 | unidentified [L] | − |

All spending charges (production, PAYG, corruption, Underworld, black market) and the fiscal income are notify 0.
Refunds, gifts, story / Lua / capture rewards and the battle-end return are notify 1.

## Q4. Interaction with the accrual 0x614340 and the reweight 0x614590

- **Reweight 0x614590** (end of each galactic goal-system proposal pass; galactic context only) [H]: A = P, R = I,
  then A −= a per goal. So a notified amount becomes **spendable** (A, which Add-goal 0x610d30 and Can-fund 0x6129c0
  use) at the next reweight, not at the notify itself. This is what lets the retail Rebel spend the capture bonus
  before the next fiscal cycle [M: matches the trace].
- **Accrual 0x614340** [H] starts every category with **P = A**, discarding any P change since the last reweight.
  Σ starts from +0x80 and adds the rebuilt P's; the second pass adds (credits − Σ) × w, with credits = player +0x70,
  which already contains every applied amount (notified or not).
  - Notified amount x with a reweight in between: x × w is in A → in P → in Σ; credits contain x; the drift term
    does not add it again.
  - Without a reweight in between: P = A drops it; Σ lacks it; the drift term adds x × w.
  - Either way the post-accrual P is the same; the notify only changes **when** the money is spendable (and so
    which goals get funded in between). No double count, **provided** the port's accrual still does P = A first and
    takes Σ from +0x80 plus the rebuilt P's.
- 0x615b40 never touches +0x80, so the release / installment adjustments of +0x80 (0x614880, 0x615700) are
  independent of it [H].
- **Battles** [M]: during a campaign battle, notify-1 calls made on the tactical side (tactical refunds, plan net-cost
  charge / return, bribes, Lua `Give_Money`, story credits) also move the **galactic** P, with the galactic weights,
  on top of the battle-start take (0x612b00: A and P −= take) and the battle-end return (0x614a70, notify 1). The
  galactic AI does not run during a battle, so this P reaches A at the first post-battle reweight; the next accrual
  resets it as above.

---

## Port change

1. Add_Credits(player, a, notify): after the multiplier / floor / cap adjustments, let Δ be the adjusted amount
   (a' minus the floor and cap overflows, computed as in Q1). If notify and the player has an AI system:
   - find the galactic mode in the mode stack (absent in skirmish → stop);
   - take the AI's context for that mode (first match in context order) and its goal system (absent → stop);
   - for every budget category, in list order: P = Δ × w + P (w = current normalised weight, float32, no FMA).
   Nothing else changes. No sign, zero or is-human test; it also runs while a battle is the current mode.
2. Pass notify exactly as in the Q3 tables: true for capture bonus, story CREDITS, Lua `Give_Money`, galactic and
   tactical production refunds, battle-end return, tactical plan net-cost charge / plan-end return, bribes,
   multiplayer gifting, linked-battle awards; false for fiscal income, all charges and installments, corruption /
   Underworld / black market, bounty, corruption success bonus, tactical income and the tactical allowance release.
3. Leave the accrual and the reweight as specified in AI_GALACTIC_2 §3.1–3.2 (P = A first; Σ from +0x80; drift on
   player credits). Do not also feed notified amounts into the accrual's income argument (that stays the raw fiscal
   income). The money becomes spendable through the reweight's A = P.

## Open items

- Confirm with a retail trace that the capture bonus share appears in A at the first reweight after the capture
  (expected: Rebel A per category jumps by 2300 × Easy/Normal/Hard multiplier × w).
- The battle-time galactic P effect (Q4 last point) is [M]; check with a campaign land battle that has tactical
  refunds or a `Give_Money` call.
- Identify 0x5327a0, 0x621370, 0x5b07c0, the 0x2aa510 / 0x2aa8e0 amount source (0x3711c0) and the positive amounts in
  the task-force `Build` functions (all notify 0 except the linked-battle awards, so they do not affect this change).
- PP +0x1A4 is assumed 0 for galactic plans (only the tactical branch of 0x6c2d60 writes it) [M]; if not, plan end
  of a galactic plan would also notify.
- Whether a human player can have an AI system (+0x360) is unchecked; if so, notify updates a budget nobody reads.

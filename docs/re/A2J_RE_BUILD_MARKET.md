# A2j: build market RE answers (Q1–Q4 of A2J_T01_BUILD_MARKET §4)

Source: StarWarsG.exe (FoC x64) decompile and disassembly, read 2026-11-06 for roadmap group a2j. Behaviour only, in
prose; RVAs are image-relative. Tags: [H] read directly in the code; [M] inferred from several consistent reads or
from the observed retail frames; [L] hypothesis.

Notation: FPS = the int at 0x140b0a340 (30). "Mode frame" = current mode +0x10 (mode pointer 0x140b15418), which is
the clock of the market gate. "Pending list" = the global vector at 0x140b39330 (begin), 0x140b39338 (end),
0x140b39340 (capacity end). PBT = AIPlanetBuildTask (0x58 bytes), BT = its owning AIBuildTask (PBT +0x38).

## 0. Summary

| # | Answer | Conf. |
|---|---|---|
| Q1 | The pending list is a contiguous vector. Every insert is an append at the back (0x53b7d0 from BUILD_TASK_LIST 0x648980; the step-2 release; signal 0xE). Step 3 scans from index 0 up and claims the first match, so the **oldest entry wins**. A ground task made at 43–62 is claimed before a structure task made at 76 whenever both have chosen the same planet queue. | H |
| Q2 | 0x6956f0 only writes the PBT's planet (+0x28) and sets its bid (+0x40) to −1. It does not touch any queue list. The PBT was appended to the pending list just before (0x53b7d0), and it stays there until step 3 of that planet queue's next run claims it. | H |
| Q3 | +0x7C is a byte of the **AIBuildTask**, not of the PBT: the "build it" flag (default 1; cleared while the free store is to supply the unit). It is set for bound tasks. Bound tasks still never bid, because the bid function 0x695c90 returns at once for bid < 0. A bound task is **never released**: step 2 always keeps a task whose bid is < 0, and fails it (Production_Failed) when it can no longer be built there. Nothing has to give it a planet again. | H |
| Q4 | Each AIPlanetBuildQueue's last-run frame starts at a **synced random integer in [1, 30]** (ctor 0x53b5c0: synced LCG draw 0x1ffb40(1, FPS)). The ctor runs twice per planet (queue 0, then 1) when the PRODUCTION behaviour attaches at galaxy build. ProductionBehavior::Service runs every frame. So each queue first runs its market at a frame in 32..61 and then every 31 frames, each with its own phase. The retail starts at 54 / 80 / 96 / 102 are such run frames. | H code, M fit to the retail frames |
| S1 | Steps 2 and 3 add **max**(fundsTime, btime) for a pay-as-you-go player (`maxss`), not min. | H |
| S2 | The step-5 slack is goal ETA (goal +0x7C) − (global frame − goal creation frame +0x70) / FPS − btime. It is not fundsTime − (NOW − task created) / FPS − btime. Candidates: PAYG player or fundsTime ≤ 0, and buildable here. | H |
| S3 | A refused bid (path, buildable, ground slots, funds) clears the task's choice when that choice is this planet. | H |

## 1. Q1: pending-list order and insertion

**Container** [H]. 0x140b39330 is a plain growable pointer vector, not a linked list. Every add copies the pointer to
the end slot and advances the end. When full, it goes through the vector grow-and-insert helper (0x45580, insert
position = end). Removal is a memmove of the tail down by one slot (0x76bd80); the market claim, 0x53c780 and 0x53b9b0
do this. The save reader (0x53c390) rebuilds it in saved order. Nothing inserts at the front.

**Who appends** [H]:
- BUILD_TASK_LIST enter 0x648980 (galactic, not MagicPlan): allocate the PBT (0x6954b0), init it (0x695720: type,
  destination, owner BT, queue index), then append it (0x53b7d0). This happens **before** the structure / +0x894 /
  free-store branch, so every galactic build task goes on the list, bound or not.
- Market step 2 release (inside 0x53cd60): 0x695a30 (planet = null, bid = 1e18), then append, then unlink from the
  queue's own list. Released tasks therefore go to the back and lose their place.
- Signal 0xE handler (0x53c4f0, planet owner changed): every own-list task with bid ≥ 0 is reset (0x695a30) and
  appended; bid < 0 tasks fail instead.
- 0x695a30 itself only resets the two fields; the caller appends.

**Step 3 scan** [H]. Index 0 up to the current size. The first PBT whose planet (+0x28) equals this queue's planet and
whose queue index (+0x54) equals this queue's index is claimed:
1. stamp PBT +0x4C (0x695a10: a timer = {mode frame, FPS × the float at 0x140b16900});
2. insert it into the queue's own list (+0x10);
3. add its time to the running queue time (+0x50): max(fundsTime, btime) for a PAYG player; for others btime only
   when fundsTime is 0;
4. erase it from the vector;
5. stop.

The claim does not check owner, state or buildability.

**Net effect** [H rule]. At any one run the oldest pending entry that has chosen this planet queue is claimed first.
A ground-unit task made at 43–62 is older than a structure task bound at 76. If both have this planet queue as their
choice at the same run, the **ground-unit task is claimed first** and the structure task waits at least one more
period (31 frames).

**Consequence for J4 / E2** [H]. Hypothesis J4(a) (newest first) is refuted. The port's oldest-first scan is right,
and experiment E2's newest-first scan reproduced the retail start for the wrong reason. Under the binary's rules the
retail Turbolaser start at about 102 needs this: at Tatooine queue 0's run at 102, no older ground task had Tatooine
as its choice. With Q4's phases that queue's previous run is at 71 (runs 40, 71, 102), before the Turbolaser task
exists (76). So a ground task holds Tatooine at 102 only if it got a Tatooine bid at 40 or 71, no other planet bid
lower afterwards, and no refusal cleared it (S3). Which of these failed in retail cannot be read from the code [L].
Two candidates: the Empire's ground tasks were made after 71 in retail, or another planet's queue outbid Tatooine.
Probe item 5 of A2J_T01 §4 (Galactic_Production_Begin_Event log) would settle it.

**Port change implied:** keep the oldest-first scan and the append-at-back on release (as now), and drop E2. Make the
claim's queue-time term max(fundsTime, btime) for PAYG players, not btime (S1). The Turbolaser-first start should come
from the per-queue phases of Q4, not from the scan order.

## 2. Q2: what binding (0x6956f0) does

[H] 0x6956f0(PBT, planet) does two things:
- it stores the planet in PBT +0x28;
- it sets the bid PBT +0x40 to −1.0 when the planet is non-null, or to 1e18 (unbound) when it is null.

It does not call 0x53b7d0 or any queue-list insert. The chain the question names belongs to BUILD_TASK_LIST, not to the
bind: 0x695720 initialises the PBT and 0x53b7d0 appends it to the **global pending list**. That happens just before the
bind, for every galactic task.

So a bound task:
1. is on the global pending list from creation until its target queue's next market run, where step 3 claims it if it
   is the oldest pending entry naming that planet queue (Q1);
2. is never a bidder in between (Q3);
3. moves into the queue's own list (+0x10) at the claim and leaves the pending list.

The planet comes from the goal's target (goal +0x58 vf+0x30). If the goal has no target object, the bind writes null
and bid 1e18, and the task then behaves as an ordinary bidding task [H for the code path; L that it occurs in vanilla].

**Port change implied:** keep bound tasks on the pending list until claimed (as now). At bind time set the task's bid to
−1 (not "bid planet only"), and treat that bid as the bound marker in steps 2 and 4. A structure task is claimable at
the first run of its target queue after it is made, behind any older pending task that chose that queue.

## 3. Q3: BT +0x7C, bound tasks and release

**+0x7C is on the AIBuildTask** [H]. The market reads it through PBT +0x38 (the owner BT): step 4 bids only when
BT +0x7C ≠ 0. It is a "this task is to be produced" flag:
- AIBuildTask ctor 0x6478c0 sets it to 1.
- BUILD_TASK_LIST sets it to 1 on every path that charges the goal budget: MagicPlan, structure/generic (before the
  bind), type +0x894 ≠ 0, and the "AllowFreeStoreUnits off or free store cannot supply" path.
- The free store clears it (0x6491f0: claim count BT +0xD4 += 1, flag = 0, from 0x4d2bd0 / 0x4d2c90) when it registers
  the task as waiting for a matching free-store unit.
- When that claim count drops back to 0 (0x649070), 0x64ab50(BT, 1) sets it again and charges the budget once (BT +0xD0
  guard).

So the flag keeps free-store-supplied tasks out of the market. **It is set for bound tasks.**

**Bound tasks do not bid anyway** [H]. The first test in 0x695c90 returns when PBT bid < 0, and the bind set it to −1.
The step-4 loop calls 0x695c90 for every pending PBT with BT +0x7C set, the same queue index and a BT player
(player +0x4C) equal to the planet owner's id (owner +4); the early return filters out the bound ones.

**Step 2 never releases a bound task** [H]. The keep test is "bid < 0, **or** (reachable 0x6955a0 and not finished
0x695780 and fundsTime ≥ queueTime)". A bid < 0 task is therefore always kept:
- PAYG player: if 0x2804d0(player, type, planet, 0, 1, …) passes, the queue time grows by max(fundsTime, btime) (S1).
  If it fails, a bound task gets `Production_Failed(plan, type)` (0x5fdd90) and its BT finishes (0x64ad00); an
  unbound task goes to the release path instead.
- Other players: the queue time grows by btime only when fundsTime is 0 (with the 1, 1 buildability flags). Otherwise
  nothing is added.

The release path (0x695a30, planet = null, append to pending) runs only for bid ≥ 0 tasks. The signal-0xE handler also
fails bound tasks instead of returning them. The only writers of PBT +0x40 are:
- ctor 1e18;
- bind −1 or 1e18;
- release 1e18;
- the bid function (new bid, or 1e18 when a refusal clears this planet's choice), which never runs for bid < 0.

So a bound task's planet is never nulled, and **nothing has to give it a planet again** [H].

A2J_T01 §2.2 assumed a release can happen ("after a release a bound task keeps its target"). Retail never reaches that
state. It also differs: a bound task whose fundsTime < queueTime stays in the list and adds max(fundsTime, btime),
where the port releases and re-claims it each run (§2.1, frames 155 and 186).

**Port change implied:**
- In step 2, keep every bid < 0 (bound) task without the reachable / funds test. For a PAYG owner add max(fundsTime,
  btime) when it is buildable here, and fail it with Production_Failed when it is not.
- Never release a bound task.
- In step 4, skip bid < 0 tasks and tasks whose BT "to be produced" flag is clear (free-store waits), not a separate
  `bound` flag.

## 4. Q4: market phase per queue

**Initial last-run frame** [H]. The AIPlanetBuildQueue ctor 0x53b5c0(queue index) does the following:
- clears the fields;
- sets current queue id +0x20 = −1;
- stores the index at +0x54;
- allocates the own task list (+0x10);
- sets the last-run frame **+0x30 = 0x1ffb40(&synced LCG 0x140a13e24, 1, (int)(FPS + 0.5))**, a synced integer in
  [1, 30].

0x1ffb40 with lo 1, hi 30 has range 29, mask 31. Each draw is (state >> 10) & 0x7FFF & 31, redrawn while > 29, plus 1.
That is one LCG step per try: one try with probability 30/32, about 1.07 tries on average.

**When the queues are made** [H]:
- Planet types in vanilla carry `SELECTABLE, PLANET, PRODUCTION` (all 64 planets in PLANETS.XML).
- ProductionBehaviorClass (vtable 0x868be0) Attach (slot 7, vtable +0x38 = 0x400f20) calls 0x390ca0, which creates the
  ProductionDataPack (planet +0xD0) once, through ctor 0x559680.
- 0x559680 builds queue 0 and then queue 1 (0x53b5c0 with edx = 0, 1).

So there are **two synced draws (about 2.13 LCG steps) per planet, at galaxy build, in planet creation order**, at the
point where that planet's PRODUCTION behaviour attaches (after SELECTABLE and PLANET in its list). Queues are never made
lazily: 0x559b40 only returns ProductionDataPack +0x38 / +0x40.

**Service every frame** [H]. ProductionBehavior answers "needs service" (slot 4, 0x17dc70) with 1 and its service
interval (slot 5, 0x18ceb0) is 1. Add_Behavior therefore registers it as a serviced behaviour with interval 1, and
object Service calls slot 6 (0x402840) every frame, subject to the usual behaviour disable counter and object
"limited" flag (OBJECT_RUNTIME §4.4 / §5.3). 0x402840 runs the market 0x53cd60 for queue 0 and then queue 1 when the
planet has its PLANET behaviour. Inside 0x53cd60:
- the BT-done check 0x53ba60 runs every call;
- the market body runs only if the planet's owner exists and is not neutral (0x4ada70), **and** (float)(mode frame −
  last run) > (float)FPS;
- it then sets last run = mode frame.

On an unowned or neutral planet the last-run frame does not advance. After a capture the queue runs at the first
service where 30 frames have passed, normally at once, so it takes a new phase from the capture frame [H code; M
effect].

**Resulting phases** [H rule; M fit]. A queue with initial value r ∈ [1, 30] runs at r + 31k for k ≥ 1, that is first
at 32..61. The retail starts fit this:

| retail start | runs of that queue | r |
|---|---|---|
| Underworld palace 54 | 54, 85, … | 23 |
| Underworld palace 80 | 49, 80, … | 18 |
| Crusader 96 | 34, 65, 96 | 3 |
| Empire Turbolaser 102 | 40, 71, 102 | 9 |

The per-frame credit changes follow one frame after the start (55, 81, 103; the Underworld's third change is at 98). The port's last run starts at 0, so every queue
runs at 31, 62, 93, … in lockstep. Which queue gets which value depends on the synced LCG state at the planet's attach;
the exact values cannot be read statically.

**Side effect on the synced stream** [H draw; M count]. Heart of the Maw has 12 planets, so the galaxy build makes
24 of these draws (about 25.6 LCG steps). The port does not make them today. That is about half of the tick-1 gap in
GALACTIC_RNG_DRAWS §3 (retail 130, port 84). The remaining gap there shrinks to about 20.

**Port change implied:**
- Give each planet's two build queues a last-run frame drawn from the synced RNG as Get_Int(1, 30) (0x1ffb40 rules,
  rejection on a 5-bit mask), queue 0 then queue 1. Draw it at the point of the galaxy build where the planet's
  PRODUCTION behaviour attaches, in planet creation order, interleaved with the other attach draws, not in a separate
  pass in `galactic_build.cpp`.
- Run a queue's market when mode frame − last run > 30, and do not advance last run while the planet is unowned or
  neutral.
- Save last run per queue.
- This changes the synced stream and every AI golden.

## 5. Side findings in the same function (spec corrections for AI_GALACTIC_2 §2.3)

- **S1, PAYG queue time is max** [H]. Step 2 (0x53cfd0) and step 3 (0x53d142) compute `maxss btime, fundsTime`: a PAYG
  task occupies the queue for the longer of its build time and its funding time. AI_GALACTIC_2 §2.3 step 2 says min,
  and the port (J7, `galactic_build.cpp` step 2) uses min. Players that pay up front add btime only when fundsTime is
  0, and nothing otherwise. **Port:** use max for PAYG in steps 2 and 3.
- **S2, step-5 pick** [H]. Candidates are the own-list tasks with (PAYG player or fundsTime ≤ 0) that are buildable here
  (0x2804d0 with 0, 1). Slack = goal ETA (goal +0x7C, from PP+0x48) − (global frame 0x140b0a320 − goal creation frame
  goal +0x70) / FPS − btime(T, planet). The least slack wins (strict <, first in list order). This is the same expression
  the queue-time estimate 0x53bb20 uses for its ordering. **Port:** replace `funds_time(b) − (now − b.created)/FPS`
  with the goal's ETA and creation frame, and add the PAYG-or-funded and buildable filters.
- **S3, refused bids clear the choice** [H]. In 0x695c90 every "no bid" exit (path failure, not buildable, company
  without a free ground slot, fundsTime < queueTime) sets planet = null and bid = 1e18 **if the task's current choice is
  this planet**. A planet that can no longer serve a task drops it at its next run, and the task waits for another
  planet's bid. **Port:** on each refusal in step 4, clear the task's choice when it equals this planet.
- **S4, the spec's "bid < 0 (cancelled)"** means bound (bid −1 from 0x6956f0), not cancelled (Q3).

## 6. Expected port behaviour after Q1–Q4 and S1–S3 [M]

Tatooine queue 0 gets its own phase from the draw. A structure task bound at 76 is claimed at that queue's first run
after 76 unless an older ground task has chosen Tatooine queue 0 by then. Once claimed it stays in the own list (never
released) and adds max(fundsTime, btime) at every run until it starts. Retail's 102 start for the Turbolaser matches
r = 9. Whether the port reproduces it depends on matching the synced stream at galaxy build (§4) and on where the
Empire's ground tasks are at 71 (Q1, probe item 5).

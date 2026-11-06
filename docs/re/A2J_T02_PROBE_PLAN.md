# A2 j t02 / t04: what the extended a2f_budget probe measures (2026-11-06)

Kit: `tools/probe_mod/a2f_budget/` (how to run it: README section 9). One 30-minute in-game run of
`Sandbox_Heart_Of_The_Maw_Rebel` (human Rebel, hands off, battles auto-resolved) serves:

- **t02 D4**: why the port's Underworld proposes `Conquer_Pirate @Kashyyyk` early and retail's does not
  (A2I_T03_TRAJECTORY.md section 5), and the same terms for the Empire's `Conquer_Pirate @Felucia` (section 3);
- **t02 D3**: what the Empire builds where in frames 0..1400 (section 4.4 point 2: the Tatooine ground queue);
- **t04**: the budget over 30 minutes, compared per fiscal cycle with a port run of the same scenario (A2I_T02_PROBE.md
  section 3 did 5 cycles).

Confidence tags as in the other A2 documents: [H] read or measured, [M] inferred, [L] hypothesis.

## 1. D4: perception terms (channel D)

Two plot scripts, one per pair, each set as the `Lua_Script` of its faction's own AI story plot (the mod copies of
`Story_Plots_Underworld_AI_Heroes.xml` and `Story_Plots_Empire_AI_Heroes_EXP.xml`; the shipped files have no
`Lua_Script`):

| Script | Pair | Plot file |
|---|---|---|
| `A2fb_PerceptU` | Underworld -> Kashyyyk | Story_Plots_Underworld_AI_Heroes.xml |
| `A2fb_PerceptE` | Empire -> Felucia | Story_Plots_Empire_AI_Heroes_EXP.xml |

Sample frames: 0, 30, .., 600, then every 300 frames until the run ends. Per sample, `EvaluatePerception(name, player,
FindPlanet(planet))`, in two records with the same head (frame, faction, target, the target's owner):

- `DT`, mod equations, one token each, written exactly as the token stands in the shipped equations: `MK`
  Variable_Target.Markup, `CIP` .ConnectsIsolatedPlanetsByForce, `TR` .TradeRoutes, `CHK` .Hints.Chokepoint, `PRI`
  .Hints.PriorityTarget, `HGF` / `HSF` .EnemyForce.HasGroundForce / HasSpaceForce, `HUM` .IsHumanControlled, `ICC`
  .IsConnectedToCorruption, `ICOR` .IsCorrupted, `ACT` Variable_Self.AnyCurrentThreats.
- `DS`, shipped equations: `SCP` Should_Conquer_Pirate_Planet (Conquer_Pirate's desire), `IGI` Is_Good_Invasion_Target,
  `ONF` Opens_New_Front, `IGN` Is_AI_And_Should_Be_Ignored, `GPV` GenericPlanetValue, `NEG`
  Is_Neglected_By_My_Opponent_Space, `SRR` System_Recon_Relevance, `ICM` / `ICE` Is_Connected_To_Me / _Enemy.

With these, retail's IGI can be rebuilt term by term (8 GPV + 3 TR + 2 CIP + MK + 5 CHK + PRI + 5 NEG + the no-force term
from HGF / HSF / SRR - 5 IGN - (5 + 5 ACT) ONF + the Underworld corruption term from ICC / ICM / ICOR) and compared with
the port's, frame by frame. The port's values at frames 240 / 270 / 390 (markup 0.293 / 0.618 / 1.012, SCP turning
positive at 270) are the ones the question is about.

Variable_Target.Markup is readable directly: it is a plain token of Is_Good_Invasion_Target, so a mod equation with
that one token reads it.

## 2. D3: production events (channel Q)

The Lua API has no read of a planet's build queue (docs/re/LUA_API.md: no queue query among the globals or the
GameObject / Player methods), so the asked 30-frame poll of the Empire's queue heads cannot be made from Lua [H].
Instead the kit hooks the three production events that the engine calls in GameScoring.lua
(`Galactic_Production_Begin_Event(planet, type)`, `_Canceled_Event(planet, type)`, `_End_Event(planet, object)`; the
names are exe strings, LUA_API.md). Records `QB` / `QC` / `QE` carry the frame, the planet's owner, the planet, the type,
its cost and the owner's credits when the event ran (`cr=`, `Get_Credits()`), for every player. The report prints every
AI player's events in 0..1500 and the first Begin per (player, planet, queue). This is lane A's D3 / D5 question: retail's
Empire credits stay at 8000 until frame 102, and the queues seem to start at different frames: the Underworld palaces at
54 / 80, a Crusader at 96. It also rebuilds each (planet, ground / space queue) from the events and prints the Empire's
heads every 30 frames in 0..1400. Whether `cr` is read before or after the engine's charge is not known: the story
channel's credit samples around the same frame tell. The ground / space split comes from the type's XML element
(`A2FB_QUEUE_TYPES.json`, made at build time from GameObjectFiles.xml).

Open [M]: whether Begin means "queued" or "started". A Begin while the same queue already has an unfinished item
settles it: the report marks such rows with `+n`.

A memory read of the queues is possible (the q2 kit's `production.q2q` reader follows planet +0xD0), but it is not
part of this kit.

## 3. Why every read is safe in retail

Run 20261106-040725 showed that a failed engine evaluation stops the calling script with no Lua error (A2I_T02_PROBE.md
section 1). So the rule is: evaluate only what retail already evaluates.

- **Shipped D4 equations.** Each is Should_Conquer_Pirate_Planet or one of its sub-functions (the selftest walks the
  `Function_X.Evaluate` closure). The goal system evaluates Should_Conquer_Pirate_Planet as Conquer_Pirate's desire, on
  these targets (Enemy | Neutral planets), every proposal pass, so the whole closure is evaluated in retail with a planet
  target [H data, M that the goal system reaches these targets]. GenericPlanetValue and
  Is_Neglected_By_My_Opponent_Space are also called from shipped Lua with a planet target (PathBonusForDistantTargets.lua).
- **Variable_Human.** Is_AI_And_Should_Be_Ignored reads Needs_More_Space_Force_Globally, so SCP, IGI and IGN need a human
  player. The kit writes `skip` for them when there is none (as for MJ). The selftest checks that exactly these three
  reach Variable_Human.
- **Token equations.** Each body is a token of those shipped equations, verbatim (same root, no parameters, as there).
  So it evaluates wherever they do (selftest check).
- **No RNG.** No galactic equation file contains the `#` operator (selftest), so no evaluation draws from the synced
  random stream.
- **The target argument.** `FindPlanet(name)` returns a planet game object, as `Find_Path` does in
  PathBonusForDistantTargets.lua. Parameter 3 accepts "AI target location or game object" (LUA_API.md).
- **Before the AI Init.** The value is nil with a warning, not a failure (AI_GALACTIC_3 section 8). Expected at frame 0.
- **Isolation.** One script per pair, and neither is the budget probe's story script. If a term does fail after all,
  only that pair's channel stops. Tokens are evaluated before shipped equations. The failing evaluation returns nil, the
  record is completed and written, then the script is not serviced again. So the last `DT` / `DS` record names the failed
  term (`key=nil` after frame 30). The selftest injects such a failure.
- **Q hook.** Every call is one that shipped GameScoring makes on the same arguments (`planet.Get_Type().Get_Name()`,
  `planet.Get_Owner()`, `Get_Name()`, `Get_Build_Cost()`, the `Get_Type == nil` test). Added: `Get_Faction_Name` and
  `Get_Credits` on a player, and `GetCurrentTime`. Other channels already use these in retail, and shipped AI Lua calls
  `Get_Credits`. No equation is evaluated there. Afterwards the shipped handler runs unchanged.
- **Unproven [M].** That an AI player's story plot services its `Lua_Script`. The T9 story step services every active
  plot's Lua (A2I_T01_FRAME_ORDER section 4). The `PerceptU` / `PerceptE` canaries show whether it did. If it did not,
  only the D channel is missing.

### 3.1 Safe, but not without effect: the leaf cache [H port, M retail]

Each token value is cached per token and bindings (Self, Target, Enemy, Human, parameters) and stamped with the frame it
was computed. A lookup hits while NOW ≤ stamp + L × 30 (AI_TACTICAL_PERCEPTION section 1.2; EnemyForce totals L = 15 s,
AI_GALACTIC_TOKENS). A Lua `EvaluatePerception` with the AI's own bindings (Self = that AI, Target = the planet) uses
the same entries. So a probe evaluation can refresh an entry earlier than the AI would have. The AI then reads a value
computed at the probe's frame instead of its own.

Measured in the port (5509c2a, 2000- and 54000-tick runs of the scenario, the exported tree with the a2j trace):

| Run | Result |
|---|---|
| extra equation file only, no evaluations | same CRC as the control |
| Is_Neglected_By_My_Opponent_Space only (`EnemyForce.SpaceTotalNBPTM`, `Has*Force`) | differs |
| SCP or IGI only (they contain NEG) | differs |
| the 11 token equations only | differs |
| ONF, IGN, GPV, SRR, ICM, ICE each alone | same CRC as the control |
| full trace, 54000 ticks | same ai-log to frame 1460, then two plans started in that frame swap order; final CRC 7cf652411ed4a322 vs 5d3a792a7601b351 |

Retail should behave the same, since its leaf nodes are registered once per evaluator class and so shared by every
equation [M]. Consequences:

- The D4 comparison is retail with channel D against the traced port run: same effect, samples at the same frames.
  Retail's plot service may land a frame or two after the nominal frame [M].
- The port's decisions in frames 0..1460 are unaffected, which covers the D4 window (0..600).
- An unperturbed t04 needs a second run built with `--no-percept`, compared with `a2j_port.py run --control`.
- Port parity: the port's single `cache_` keyed by (leaf name, bindings, parameters) matches this model. A difference in
  retail's key (the [M] in section 1.2) would show here as a port-only probe effect.

## 4. t04: the 30-minute run and the per-cycle comparison

The story script writes `MIN30` and shows "30 minutes recorded" at 1800 s. LONGDONE at 1500 s is kept. The free store
keeps 300 samples (40 minutes at 8 s). The other caps already fit 30 minutes: story 20000 records against about 7100
expected, plans 3000 per script instance.

Port side, `a2j_port.py`:

- It exports a commit (git archive) and applies `tools/analysis/a2g_trace_patch.py` plus `port_a2j_patch.py`. The second
  patch keeps the per-frame budget row to frame 10200, then every 30 frames; it also adds the `A2J PERC` lines and the
  token equations.
- It builds `alamo_headless` and runs the scenario: `--economy --galactic-ai --story --campaign
  Sandbox_Heart_Of_The_Maw_Rebel --human Rebel --seed 1 --workers 0 --ticks 54000`.
- The traces are not neutral: the perception trace shifts the AI's leaf-cache refreshes (section 3.1). The traced run
  pairs with a retail run with channel D, and `--control` (A2G BUD rows only) pairs with a retail `--no-percept` run.

`a2f_report.py <run> --port-trace port.err`, section 8, gives two rows per fiscal cycle (payments 1351 + 1350 k):

- the first retail sample after the payment;
- the last retail sample before the next payment.

Each row shows the OFF / INFRA / MAJOR pool P and income rate I, and the credits, retail | port. The port row is the
last one at or before the same frame. First payment, port: the Empire is paid 667.5 at frame 1350, as in retail. The
earlier 585 (lane B, a2j) came from the D4 Kashyyyk loss. Both port runs here are identical to frame 1460, so the first
cycle compares the same either way.

## 5. Port reference (D4)

`tools/probe_mod/a2f_budget/port_reference_a2j.tsv`, from commit 5509c2a (`origin/claude/a2i-followups-4`), 54000 ticks,
final CRC 7cf652411ed4a322.

- **Sampling.** Same frames as the retail kit. The port has no frame-0 row. Each pair is evaluated right after that AI
  player's own galactic AI service; retail's plot script runs later in the same tick (T9).
- **Empire -> Felucia [H].** SCP 5.942 from frame 30 (8 GPV 0.618 + 3 TR 0.333; ONF 0, CIP 0). It is 6.251 from frame
  360, when a markup of 0.309 appears, and later varies between 1.25 and 6.56.
- **Underworld -> Kashyyyk [H].**
  - Frame 30: SCP -0.307 (CIP 1, ONF 1, ACT 1, TR 1, GPV 0.587).
  - Markup 0.293 at frame 240, 0.618 at 270, 1.012 at 390. SCP is +0.311 from frame 270.
  - SCP is 15.7 from frame 1500 and about 17.0 by 3300, then falls back to between -4.3 and 2.0.
  - Kashyyyk stays Empire-owned through frame 54000 in this commit.

What retail shows for the same frames decides D4. If MK stays 0 in retail, the markup source (Distant_System_Path_Bonus
/ Find_Path) is where the port differs. If MK matches, look at CIP / ONF / ACT.

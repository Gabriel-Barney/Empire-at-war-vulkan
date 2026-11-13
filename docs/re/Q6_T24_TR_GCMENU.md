# q6 t24: Thrawn's Revenge GCMenu, planet hand-out order and victory hold (2026-11-13)

Question: with the owner-change victory rule (q6 t21 / t22, `Q6_T15_RETAIL_RUN.md` "ELIM C" and "q6 t25"), does TR's
GCMenu campaign end at once? Does its story hand AI planets out before the human's, and does it hold victory with
`ENABLE_VICTORY 0`?

Short answer: the GCMenu story hands out **no** planets and issues **no** `ENABLE_VICTORY`. Retail stays alive because
no planet ever changes owner in GCMenu, so the victory tests never run. The port stays alive under the same rule if
the first owner assignment of each planet tests nothing, and nothing in setup or the first ticks counts as an owner
change.

Tags: [H] binary (RVA in StarWarsG.exe, Steam 1.121, `re_reference/ghidra`), [D] mod data (TR 3.3, workshop 1125571106,
paths relative to `Data/`), [?] not confirmed.

## 1. Campaign definition [D]

`XML/Conquests/Campaigns_GCMenu.xml` (the only GCMenu campaign; `XML/CampaignFiles.xml` lists it):
- l.6-7: `GCMenu_Rebel`, set `GCMenu`. l.18-24: two planets, Balmorra and Carida, one trade route.
- l.26-27: `Home_Location` Rebel = Balmorra, Empire = Carida. **No `Starting_Forces` at all** (l.71-83 hold only
  credits / tech levels: Independent_Forces 300000, Rebel 0, Empire 0).
- l.29: `Starting_Active_Player` Rebel (the human). l.33: `AI_Player_Control Rebel, SandboxHuman`.
- l.63-69: `Human_Victory_Conditions` and `AI_Victory_Conditions` are both `Galactic_All_Planets_Controlled`.
  The AI list is non-empty, so every non-neutral AI player gets the implicit type 1 record (0x289a90).
- `XML/Factions.xml`: only `Neutral` (l.1976, `Is_Neutral Yes` l.1988) is neutral. Playable AI factions (type 1 gate
  `Is_Playable`) include Empire (l.700/715), Hutt_Cartels, Eriadu_Authority, Greater_Maldrood, Pentastar,
  Zsinj_Empire, Corporate_Sector, Hapes_Consortium, EmpireoftheHand, Imperial_Proteus and Yevetha.
- Retail: `Home_Location` alone does not give a planet. The q6 t15 ELIM A / C kit used the same layout (human with
  `Home_Location`, no `Starting_Forces`), and retail reported `owned=0` at START. [H, probe]
  So in GCMenu the human (Rebel) owns nothing, Empire owns nothing, and both planets keep the derived default owner
  (GALACTIC_ECONOMY §4.1: political leaning, else the first `Is_Neutral` player). [?] Which of the two applies to
  Balmorra / Carida was not traced. It does not matter, because the owner never changes.

## 2. Story and Lua [D]

`XML/Conquests/Story_Plots_GCMenu.xml` l.4-7: Lua `GC_MasterScript_GCMenu`, plots `Story_GCMenu.xml` and
`Player_Agnostic_Plot.xml`.
- `Story_GCMenu.xml`: `Trigger_Load_GC_Selector` (STORY_ELAPSED 0, l.4-7). Its only rewards are `LOAD_CAMPAIGN`
  (l.9-16), DISABLE_/ENABLE_EVENT, FORCE_CLICK_GUI and TRIGGER_AI. No VICTORY, ENABLE_VICTORY or ownership reward.
- `Player_Agnostic_Plot.xml`: 13 STORY_ELAPSED events (LOCK_CONTROLS, REVEAL_ALL_PLANETS, the Zoom_Zoom branch,
  FORCE_CLICK_GUI). The ownership-related templates (`Template_Switch_Sides` l.6266, `Template_Switch_Control`
  l.6281, `Template_Victory_Event` l.6297, `Template_Link_Tactical` l.6305, `Template_Company_Remove` l.6455) fire
  only on STORY_AI_NOTIFICATION from Lua. GCMenu's Lua never sends those notifications (below).
- `Scripts/Story/GC_MasterScript_GCMenu.lua`, `State_Load_GC_Selector` (l.90-226): loads menu tables and
  force-clicks the default buttons (difficulty, Prog_Text, faction_01, era_01, prog_map_04). Then it calls
  `StoryUtil.UnlockAllControls()` (l.221), and on OnUpdate it runs `crossplot:update()`. Every `Story_Event` in the
  file is `FORCE_CLICK_BUTTON_EVENT` (19), `ENABLE_BUTTON_EVENT` (5), `DISABLE_BUTTON_EVENT` (4) or
  `LOAD_CAMPAIGN_EVENT` (1, `Load_GC` l.1820-1860). Start GC therefore loads a **new** campaign (LOAD_CAMPAIGN);
  it does not change ownership in this one.
- No `Change_Owner` in `GC_MasterScript_GCMenu.lua`, the `GCMenu*.lua` tables, or the libraries it requires
  (`deepcore/crossplot/crossplot.lua`, `eawx-util/StoryUtil.lua`, `eawx-util/StringUtil.lua`,
  `eawx-events/GenericPopup.lua`). The two planets have no `Lua_Script` (`XML/Planets.xml`). With no units and 0
  credits, no AI player can build or move anything.
- `ENABLE_VICTORY` appears in TR only in tactical plots: `Conquests/InstantAction/Story_InstantAction_Tactical.xml`
  l.210 / 218, `Survival/Story_Survival_Tactical.xml` l.7, `Tactical/Story_Tactical_Universal_Land.xml` l.202 and
  `Tactical/Story_Tactical_Universal_Space.xml` l.201. No TR galactic plot holds victory.

**Order of planet hand-out: none.** GCMenu ends only through `LOAD_CAMPAIGN` (or the player quitting).

## 3. Retail mechanics [H]

**First owner assignment (0x3fb040, planet data pack lVar15 = param_2[0x17]).**
- If +0x1c9 == 0, the function sets control +0x68 = param_4 and owner +0x6c = param_3, sets +0x1c9 = 1, and
  re-broadcasts visibility.
- It then snapshots +0x68/+0x6c (0x84e60 into local_8a0 / local_89c). The ramp / target block runs next.
- At the test `if (local_89c == +0x6c) goto LAB_1403fbdda` the owner was already written before the snapshot. So
  the first call jumps past the "%s: now controlled by %s" log, TEXT_PLANET_CHANGED_HANDS, the capture bonus and the
  per-player victory loop (types 0/1/4/5/9 via 0x28f570). **Confirmed: the first assignment tests nothing.**
- The same test makes a call whose owner does not change a no-op for victory. This matches the t25 note that
  `Change_Owner` to the current owner ends nothing.
- The first call for every planet comes from the per-frame planet service 0x3fe1b0 (derived owner 0x3f6df0, then
  0x3fb040, Global.c around RVA 0x3fe1b0 tail). So every planet, neutral or not, has its first assignment in its
  first service. Every later change of the derived or Lua owner is a tested owner change. Callers with control 1.0:
  0x3f7280, 0x3f8060, 0x3f98a0 (Change_Owner / revolt paths).

**Victory hold (galactic too).**
- 0x28f570(GMM 0x140b153e0, …) forwards to `*(GMM+0x38)` = DAT_140b15418 (the current mode), monitor at +0x28, and
  calls 0x346460.
- The story reward `ENABLE_VICTORY` (0x456970) uses the same object: `DAT_140b15418[5]` (+0x28). P1 = 0 calls
  0x341fe0 (hold +0x5c += 1). P1 ≠ 0 calls 0x3440a0(…, 1) only while +0x5c > 0.
- So `ENABLE_VICTORY` acts on the galactic monitor when it is issued in galactic mode. Other hold sources:
  full-screen movie 0x60330 and TSW 0x42a200 (BATTLE_TRANSITIONS §3.1.3).
- 0x346460: with the hold ≥ 1, it does not evaluate. It calls 0x341f40, which appends the (type, player) pair to the
  deferred queue +0x60/+0x68/+0x70 (deduplicated), and returns **false**. The 0x3fb040 loop then tries the next
  type / player, and every one of them returns false too.
- When the last hold is released by ENABLE_VICTORY ≠ 0 (0x3440a0 with replay 1), the queued pairs are re-tested **in
  queue order, against the state at release time**. The first pass declares, and the queue is freed.
- The VICTORY reward and retreat release all holds through 0x343f80(…, 0), with **no replay** (queue kept, not
  re-run).

So a hold **delays** the implicit type 1. It is neither skipped nor fired at once. The test is re-run at release, and
it ends the campaign only if the human still owns nothing then. The end screen is not involved: nothing is declared
while the hold is up.

## 4. Conclusion

- **Retail.** GCMenu never ends by itself. No planet changes owner (no forces, no Change_Owner, no AI activity). The
  starting owners are first assignments and test nothing. Type 10 (`Cycles_Elapsed`) and type 8 are not in any list
  here. This matches t15's "3 min 20 s, did not end". [H] for the mechanism, [D] for the data.
- The t24 premise ("an AI planet handed out before the human's") does not occur in GCMenu. The hold question is
  moot: TR holds no galactic victory anywhere.
- **Port rule needed for t21 / t22** (all [H]):
  1. Run the victory tests (types 0, 1, 4, 5, 9; every player in PlayerList order; first pass wins) only inside
     `on_owner_change` and only when the owner really differs from the previous owner.
  2. The first owner assignment of every planet must not test. That includes campaign setup from `Starting_Forces`,
     planets left to the derived default owner (neutral or leaning), and the first planet service.
     - The port must not produce a "change" on tick 1, for example setup writes owner X and the first derivation then
       yields Neutral or Y.
     - Either mark each planet's first assignment as untested (retail's +0x1c9), or make setup and the first
       derivation agree.
  3. `Home_Location` gives no ownership (it only names the home / revealed planet).
  4. Implement the galactic hold the same way: ENABLE_VICTORY 0 / movie raise it. While it is up, every test queues
     (type, player) and returns false. The release (ENABLE_VICTORY ≠ 0, last level) re-tests the queue in order. The
     VICTORY reward releases without replay. TR GCMenu does not need this, but FoC galactic story plots that use
     ENABLE_VICTORY 0 do.
- [?] Not checked here: real TR GCs started from GCMenu (`LOAD_CAMPAIGN`). Their ownership comes from
  `Starting_Forces` (first assignment, untested). Any start-up `Change_Owner` in TR libraries
  (`eawx-util/ChangeOwnerUtilities.lua`, `deepcore/.../PlanetTransferBuilder.lua`, `eawx-lua-events/StoryEvents.lua`)
  would be a tested change, so it would end the campaign if it ran while the human owned nothing. A human with a
  `Starting_Forces` planet is not affected.

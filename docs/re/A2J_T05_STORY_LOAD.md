# A2j t05: story load pass leftovers (port change, 2026-11-06)

Follow-up to A2I_T01_FRAME_ORDER.md §6 "Related differences" and "Side effects". Tags as in the other `docs/re` pages:
[H] read from code, [M] one inferred link, [L] unverified. No new RE reading: everything below is the A2I_T01 spec put
into the port (`engine/apps/sim/src/story_session.cpp`, `StorySession::load_pass`), plus questions for the next RE task.

## What changed

Load pass order (end of `StorySession::start`, before tick 1, story clock at mode frame 0 / time 0, not paused):

1. **Starting-force CONSTRUCT** (A2I_T01 §2 L8, §3.3 [H for the call, M for the order]). For each starting-force object,
   in object creation order (`ObjectStore::all_objects`: the port creates `GalacticSetup::forces` in `<Starting_Forces>`
   entry order): STORY_CONSTRUCT (name = type name, owner = the object's owner, planet = its planet's type name), and for
   a starbase (category 0) or ground structure (category 1) STORY_CONSTRUCT_LEVEL with the level its creation left (the
   starbase's level; the k-th ground structure of a planet gives ground level k). Every galactic plot set in set order,
   the same unscoped route as a completed build. Fires are logged `via=load:construct`. No trigger line is written.
   The objects are captured when `start()` begins, before any plot script's `Base_Definitions` runs.
2. **Tech pass** (A2i t01, unchanged order): STORY_TECH_LEVEL per player in player-index order to its own plot sets,
   logged `via=load`. While this pass (and step 1) runs, every object a SPAWN_HERO / UNIQUE_UNIT reward creates raises
   STORY_CONSTRUCT (+ CONSTRUCT_LEVEL) **inside the reward call**, before the firing event's dependants are armed, as
   0x400b40 does (A2I_T01 §3.3 [H as stated there]). In HotM this is the Accuser, Home_One and the other tech heroes; no
   HotM plot has a CONSTRUCT event, so nothing fires.
3. **Load-time galactic stream.** After step 1 and after each player's tech dispatch, the galactic events the load
   rewards (and plot scripts) raised are drained, translated and routed at once (the T9 further-rounds code: trigger
   line `T 0 <index> ...`, at most `kMaxRounds` rounds), instead of at the start of T9 of tick 1. Example: a load-time
   SET_TECH_LEVEL reward's TechLevelChanged. [port] retail dispatches each notification inside the call that raises it;
   the port routes them after the step's whole Fire chain, as T9 does for rewards.

`ProductionCompleted` now goes through the same CONSTRUCT helper (`construct_notifications`); its output is unchanged.

Test: `alamo_sim_tests` `test_story_load_construct` (starting SB1 / Barracks / enemy Corvette CONSTRUCT and levels,
CONSTRUCT inside the UNIQUE_UNIT reward between the event and its dependant, the dependant's TechLevelChanged routed as
trigger 0 at frame 0, identical logs and CRCs for every worker count).

## Effect on runs

- Only plots with an armed CONSTRUCT / CONSTRUCT_LEVEL event that matches a starting-force or load-spawn type, or load
  rewards that raise galactic events, change. HotM (no CONSTRUCT events, load rewards are spawns only) and the
  Underworld campaign opening (its only CONSTRUCT event, `Build_a_Saboteur_00`, has a prereq) are unchanged [M: from the
  plot XML]. A non-matching dispatch changes no story state, so the story CRC only moves when something matches.
- A load event that used to be trigger `T 1 ...` is now `T 0 ...` in record 0 (story CRC: trigger count).
- The game host's presentation tap (`GameSession::tap_events`) reads the stream before T9; load-time events are now
  drained before the tap is installed, so they no longer reach it (no HotM case: the spawns raise no stream events).

## Open questions (not guessed; the port keeps its current choice)

1. **L8 order.** "per planet the `Starting_Forces`" (A2I_T01 §2): does 0x292050 walk planets in `Locations` order and
   create each planet's entries, or walk the `<Starting_Forces>` entries in file order? The port uses entry order.
2. **Company CONSTRUCT.** For a company (0x400f30: transport + `Company_Units` members), does 0x400b40 raise one
   CONSTRUCT with the company type (port), or one per created object (transport, members)? And does the +0x2018
   chained type raise its own?
3. **CONSTRUCT_LEVEL value.** Is it the planet's level right after that object's creation (port), and is the ground
   level the count of ground structures (the port's `ground_base_level` model) or a Base_Level of the structure?
4. **Spawns outside the load.** Does 0x400b40 raise CONSTRUCT for story / Lua / AI (HIJACK_UNIT) spawns at run time too?
   A2I_T01 §2 L8 says "for every object it creates", but `GalacticSim::spawn_unit` documents "no story event". The port
   raises it only inside the load pass; if retail raises it always, the T9 reward spawns, Lua `Spawn_Unit` and the AI
   magic spawn need it too (galactic lane for the last two).
5. **Owner filter of CONSTRUCT.** STORY.md §5 says object = planet; the port filters on the creating player [M]. At
   load the planet owner and the creator are the same for starting forces, but not for a spawn on a planet the player
   does not own.

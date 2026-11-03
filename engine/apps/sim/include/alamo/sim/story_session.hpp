// S4 "Story & campaign events": the campaign side of the story system in the G0 host.
//
// A StorySession belongs to one game session of a SimHost (SimHost::start_story). It owns
//   * one engine/lua StoryModel per plot set (TheStoryMode::Load_Plots loads one plot set per
//     faction, docs/re/MAIN_LOOP.md 5.2): the plots file named by the campaign's
//     <Faction>_Story_Name tag, loaded in player-list order;
//   * one LuaVm per plot that has a <Lua_Script> (the original's LuaScriptClass = one lua_State per
//     script, SUBSYSTEMS.md "lua_script"): Lua globals of two factions' story scripts never share a
//     state. Every VM of the session is created, serviced and destroyed on the sim thread;
//   * the game-side trigger stream: the GalacticSim's S4 events (galactic_events.hpp, drained once
//     per tick) plus host events (tactical mode pushed / popped), stamped with the global frame and
//     their index in the stream, translated to story terms (planet / type / faction names).
//
// Load order at start_story (after Start_Game + galaxy + starting forces; MAIN_LOOP 5.2 loads the plots
// between the galaxy build and the player setup, which is not observable: loading fires nothing):
//   for each plot set in player-list order: StoryModel::load_plots; then for each plot with a
//   <Lua_Script>, in plot order: new LuaVm (host = galactic primary store, object types, the session's
//   GameRandom, the host's FileProvider), attach_story, require(<script>), Base_Definitions(),
//   Create_Thread("main") (PGBase.lua / PGStateMachine.lua: main = Process_States).
//
// T9 story service (SimHost::tick_once, after T7 players / T8 global Lua; docs/re/STORY.md 4.1):
//   0. [port] the original dispatches game notifications synchronously when they happen; here the
//      galactic stream raised during the tick (and host events) is translated and dispatched at the
//      start of T9 (StoryModel::dispatch: active plots, armed + unfired + enabled events, per-type
//      match rules), then the player-scoped polls stand in for missing sources (STORY_TECH_LEVEL with
//      the plot set's tech level every T9, STORY_ACCUMULATE when its credits changed);
//   1. STORY_ELAPSED (story time = galactic mode seconds, paused = galactic pause counter);
//   2. per plot in the original's table order: the plot script's Lua service when its ServiceRate
//      (default 0.2 s) has elapsed since LastService (runs for suspended plots too), then for active
//      plots inactivity timers, the STORY_FLAG poll and Timeout expiry;
//   3. OBJECTIVE_TIMEOUT. (UNIT_PROXIMITY, CHECK_DESTROYED, FOG reveals, GENERIC battle_end_closed:
//      no source yet.)
//   Rewards the host applies (SET_TECH_LEVEL, CREDITS) run synchronously inside Fire (on_reward); the
//   galactic events they raise are dispatched in further rounds of the same T9 (at most kMaxRounds).
//   Story_Event_Trigger(name) reaches the plot's script synchronously at Fire (StoryModel::set_script_vm).
//   Story flags are one table for the session (STORY.md §0 [H]): every galactic plot set and the tactical plot set share
//   the first set's table (StoryModel::share_flags), so a mission's SET_FLAG ..._WIN reaches the campaign's STORY_FLAG.
//
// Notification mapping (STORY.md 5; planets are named by their TYPE name):
//   PlanetOwnerChanged   STORY_CONQUER (name = planet type, owner = new owner) + STORY_CONQUER_COUNT
//   ProductionCompleted  STORY_CONSTRUCT (name = built type, owner = producer [M]) + STORY_CONSTRUCT_LEVEL
//                        for a starbase (category 0, planet starbase level) or ground structure (1, ground
//                        base level)
//   FleetArrived         STORY_ENTER + STORY_MOVE, stopping arrivals only [port] (units = fleet contents)
//   FleetBounced         STORY_FLEET_BOUNCED
//   UnitsLanded          STORY_LAND_ON + STORY_DEPLOY (L1/S4 wave 1; units = the landed units: the last
//                        `value` surface units of the player [port])
//   ObjectDestroyed      STORY_DESTROY (name = type); a named hero (Is_Named_Hero, or a Company_Units member
//                        that is one) also STORY_DEFEAT_HERO once per hero type name (wave 1)
//   CorruptionChanged    STORY_CORRUPTION_CHANGED
//   PlanetDestroyed      [port poll, wave 1] a planet's destroyed flag went up: STORY_PLANET_DESTROYED
//   (poll)               STORY_BEGIN_ERA with STORY_TECH_LEVEL (same evaluator, BeginEra class)
// Fleet / landing unit lists given to the evaluators also hold the Company_Units member types of each unit
// [inferred: the original's fleet tests are recursive through fleets / transports; galactic teams carry
// no member objects] (the T log lines keep the unit types only).
// UI notifications (wave 1, notify_ui; the P2 HUD and later the galactic action dialogs call them):
// SELECT_UNIT, SELECT_PLANET, ZOOM_INTO/OUT_PLANET, CLICK_GUI, SPEECH_DONE, MOVIE_DONE, GENERIC,
// MISSION_LOST, BUY_BLACK_MARKET, GALACTIC_SABOTAGE, OPEN_CORRUPTION; queued and routed at the next T9
// after the galactic stream and the host events, one "T ... Ui<Kind>" line each.
//   BattleWon / BattleLost / BattleRetreat (S10)  STORY_WIN_BATTLES / STORY_LOSE_BATTLES / STORY_RETREAT,
//                        player-scoped to the winner / loser / retreating player (Param2 location and Param3
//                        composition of WIN/LOSE_BATTLES not modelled)
//                        (wave 2a) with Param2 location = the event's value (1 land / 2 space) + "base" and
//                        Param3 composition from the battle snapshot (begin_battle) when it is the same planet
//   BattleLost (land)    (wave 2a) also STORY_INVASION_BOUNCED to the loser's plot sets when the loser was the
//                        invader (the snapshot's attacker; without one: not the planet's owner at the previous
//                        service) [inferred], Enter family: planet, owner = loser, units = the invader's types
//
// L1/S4 wave 2a - BATTLES AND THE TACTICAL STORY (the original keeps one story manager per game mode, STORY.md 0):
//   begin_battle (battle host, at the tactical push): snapshot of the battle (planet, kind, attacker / defender,
//     per-player unit categories and type names, base present) for the later WIN/LOSE_BATTLES / INVASION_BOUNCED
//     routing; STORY_SPACE_TACTICAL / STORY_LAND_TACTICAL dispatched at once to the participants' galactic plot
//     sets (STORY.md 4.1: these types are never queued; Param2 planet, StartTactical [inferred]); then the
//     tactical plot set is loaded (one StoryModel + one LuaVm per scripted plot, on the battle's store and
//     ObjectBridge): explicit file, else Param1 of the first STORY_*_TACTICAL event that fired, else the
//     Param7 plots of a LINK_TACTICAL reward recorded for that planet and kind (linked_tactical()).
//   service() also runs the tactical T9 while a battle is attached, after the galactic one: queued
//     notify_tactical() notifications (routed to the tactical plot set: "the running mode's story manager"),
//     STORY_ELAPSED, per plot Lua + timers + FLAG, UNIT_PROXIMITY poll (local-player objects), CHECK_DESTROYED
//     poll, OBJECTIVE_TIMEOUT (STORY.md 4.1 steps 2-5). Tactical story time = (float)(frame - begin frame) x
//     (float)seconds_per_frame, StoryModel::t9_seconds [port].
//   end_battle (battle host, before the pop): STORY_VICTORY (Param1 = winner faction names) to the tactical
//     plot set, then the tactical VMs and plot set are released.
// Not raised yet: FOG_*_REVEAL (fog of war); LOAD_TACTICAL_MAP and the notify_tactical kinds the battle host does
// not feed. Wave 2b (SCRIPT_GAPS.md §4): DIFFICULTY_LEVEL is never dispatched (as in 1.121);
// CORRUPTION_TACTICAL_COMPLETE / FAILED at end_battle of a pending corruption mission (set_corruption_mission);
// FILTER_* compare factions (StoryModel::faction_of); the original vf3 rules of the wave-2a [inferred] types. SPEECH_DONE's 60 s
// Timeout still runs [L] when no speech end is reported (the game host reports AudioSystem speech ends). Galactic notifications raised while a
// tactical mode runs are routed at once (the galactic sim emits nothing then).
//
// Lua bridge families attached to every story VM of the session (L1 stage 3):
//   * LuaPlayerWorld (players.hpp, when engine/lua is built with the players family): backed by the
//     session's GalacticSim (credits, tech, factions; Find_Player, PlayerWrapper, Suspend_AI, ...);
//   * ObjectBridge (object_bridge.hpp): orders go through the host's CommandPipeline::execute_direct
//     (set_command_pipeline; the galactic mode routes them to GalacticSim::handle_command), the
//     enemy test is the player world's relation table, script-only object state lives here;
//     Change_Owner = GalacticSim::change_owner; containment (Get_Parent_Object, Get_Planet_Location,
//     Contains_Hero, ...) = GalacticSim::container_of / contents_of; Despawn / Destroy_Contained_Objects
//     = GalacticSim::destroy_object (wave 1); Reinforce_Unit / Spawn_From_Reinforcement_Pool use the
//     bridge's direct-spawn stand-ins (no tactical reinforcement system yet);
//   * one PresentationLog (presentation.hpp) for the whole session: camera / fade / audio / UI and
//     object effect calls of every story script, for R1 / P1 / P3 to drain (presentation()).
// LuaHost::campaign is true for story VMs (Is_Campaign_Game).
//
// Determinism: single-threaded (sim thread), vectors in load / stream order, names compared ASCII
// case-insensitively; the per-tick CRC system "story" (SimConfig::story_crc) hashes every model
// (StoryModel::hash), the per-event construct counters, the trigger count, the player world
// (LuaPlayerWorld::hash) and the object bridge's script state (ObjectBridge::hash). Lua VM state and
// the presentation log are not hashed.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/galactic/galactic_events.hpp"

namespace alamo {
class FileProvider;
class GameRandom;
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo
namespace alamo::data {
struct CampaignDef;
}
namespace alamo::galactic {
class GalacticSim;
struct BattleRequest;
}  // namespace alamo::galactic
namespace alamo::objects {
class GameObjectTypes;
class ObjectStore;
struct ObjectHandle;
}  // namespace alamo::objects
namespace alamo::lua {
class StoryModel;
class LuaVm;
class LuaPlayerWorld;
class GlobalValueStore;  // Int-final
class ObjectBridge;
class PresentationLog;
struct StoryNotification;
}  // namespace alamo::lua
namespace alamo::commands {
class CommandPipeline;
}

namespace alamo::sim {

// One plot set: the plots file loaded for one player.
struct StoryPlotSet {
    int32_t player = -1;
    std::string plots_file;  // e.g. "Story_Plots_Campaign_Underworld.XML"
};

// The campaign's plot sets: for every player in player-list order, the campaign tag
// "<faction name>_Story_Name" (Rebel_Story_Name, Empire_Story_Name, Underworld_Story_Name ...).
std::vector<StoryPlotSet> campaign_plot_sets(const data::CampaignDef& campaign, const galactic::GalacticSim& sim);

struct StoryStart {
    const FileProvider* files = nullptr;  // Data\XML plots + Data\Scripts (outlives the session)
    std::vector<StoryPlotSet> plot_sets;  // load order
    bool lua = true;                      // run the plots' Lua scripts
    // Int-final: the session-wide GlobalValue table attached to every plot VM (SimHost::start_story fills it with
    // SimHost::global_values() when null). Null: the process table (L1 bindings fallback).
    lua::GlobalValueStore* global_values = nullptr;
};

// Host events (not from the galactic stream).
enum class StoryHostEvent : uint8_t { TacticalStarted = 0, TacticalEnded = 1 };

// UI / player-action notifications (StorySession::notify_ui). Story type and evaluator (STORY.md 0.1, 5):
//   SelectUnit       STORY_SELECT_UNIT (SingleObjectName: name = the object's type, filter on its owner)
//   SelectPlanet     STORY_SELECT_PLANET   } SelectPlanet class: Param1 planet names, compared with the
//   ZoomIntoPlanet   STORY_ZOOM_INTO_PLANET} planet's type name (generic parameter rule)
//   ZoomOutPlanet    STORY_ZOOM_OUT_PLANET }
//   ClickGui         STORY_CLICK_GUI        (String class: Param1 empty or names `name`; same below)
//   SpeechDone       STORY_SPEECH_DONE      (name = the speech / dialog event name)
//   MovieDone        STORY_MOVIE_DONE       (MovieDone class; generic rule, Param1 = movie name)
//   Generic          STORY_GENERIC
//   MissionLost      STORY_MISSION_LOST
//   BuyBlackMarket   STORY_BUY_BLACK_MARKET (name = item Name), raised by the host's black-market buy handler at click
//                      time (L1P4_LUA_GAPS 2); every plot set, not only the buyer's.
//   GalacticSabotage   ignored by notify_ui: STORY_GALACTIC_SABOTAGE (name = structure type) is routed from the
//                      galactic stream's GalacticSabotage events to every plot set (L1P4_LUA_GAPS 2.5). Story rewards
//                      GIVE_BLACK_MARKET (bm_give_item, plot player) and SABOTAGE_STRUCTURE (sabotage_structure_reward;
//                      Param1 = planet type name, case-insensitive) are applied by the host.
//   OpenCorruption   STORY_OPEN_CORRUPTION  (name = the planet's type name, or the object's)
enum class StoryUiEvent : uint8_t {
    SelectUnit = 0, SelectPlanet, ZoomIntoPlanet, ZoomOutPlanet, ClickGui, SpeechDone, MovieDone, Generic,
    MissionLost, BuyBlackMarket, GalacticSabotage, OpenCorruption, Count
};
std::string_view story_ui_event_name(StoryUiEvent e);  // "SelectUnit", ...
std::string_view story_ui_event_type(StoryUiEvent e);  // "STORY_SELECT_UNIT", ...

struct StoryUiNotification {
    StoryUiEvent kind = StoryUiEvent::Count;
    int32_t player = -1;      // the acting (local) player: only that player's plot sets; -1 = every set
    int32_t object = 0;       // ObjectId in the galactic store (selected unit, planet, sabotaged object); 0 = none
    std::string name;         // explicit name (GUI element, speech, movie, tech, type / planet name)
    // L1/S4 wave 1 (tactical): raised on the battle screen. SelectUnit (object = an ObjectId in the BATTLE's store),
    // ClickGui (tactical HUD component) and SpeechDone go to the attached tactical plot set as TacticalStoryEvent
    // SelectUnit / ClickGui / SpeechDone (notify_tactical; only when `player` is -1 or the tactical plot set's player).
    // SelectUnit / ClickGui never reach the galactic plot sets (dropped without a tactical plot set); SpeechDone and the
    // other kinds also go the galactic way [inferred: a galactic plot's speech can end after the tactical push].
    bool tactical = false;
};

// ---- L1/S4 wave 2a: battles and the tactical story ----

// What the battle host passes at the tactical push (begin_battle).
struct StoryBattleStart {
    const galactic::BattleRequest* request = nullptr;  // the conflict (planet, kind, players, forces, starbase / structures)
    objects::ObjectStore* store = nullptr;  // the tactical mode's primary store (the tactical scripts' objects)
    // The battle's object bridge (orders pipeline, change_owner + TreeSet, abilities, anim, ...); nullptr = a
    // session-owned store-only bridge. Must outlive the battle (end_battle).
    lua::ObjectBridge* bridge = nullptr;
    int32_t local_player = -1;     // tactical plot owner (FILTER_*, UNIT_PROXIMITY); -1 = see begin_battle
    std::string plots_file;        // explicit tactical plots (Story_Plots_*.xml); "" = from the story (see above)
    double seconds_per_frame = 1.0 / 30.0;
    // CHECK_DESTROYED DESTROY_ALL / DESTROY_ALL_UNITS override [port]: true when `player` has nothing left. Unset (the
    // battle host leaves it unset): the original's store rule (wave 2b, SCRIPT_GAPS.md §4.8) - no live SELECTABLE
    // object of the player (units only: DUMMY_LAND_BASE_LEVEL_COMPONENT objects ignored). The original counts the
    // mode's objects only, so units still in a reinforcement pool do not keep a side alive.
    std::function<bool(int32_t player, bool units_only)> side_destroyed;
    // Wave 2b (SCRIPT_GAPS.md §4.1): STORY_SPACE/LAND_TACTICAL are dispatched in single-player games only.
    bool multiplayer = false;
    // Wave 2b HOOK [fog agent] (FOG_OF_WAR.md §6.6): polled each tactical T9 when set (unset: the two types never
    // fire). fog_point_visible = !FOW_Point_Test(player, pos, force 0) fogged (STORY_FOG_POSITION_REVEAL, Param1
    // "x y z"); fog_object_visible = !FOW_Object_Hidden(player, object, 0) (STORY_FOG_OBJECT_REVEAL, Param1 = a type
    // name; [port] fires at most once per poll, the original once per matching object).
    std::function<bool(int32_t player, double x, double y, double z)> fog_point_visible;
    std::function<bool(int32_t player, int32_t object)> fog_object_visible;
};

// Tactical notifications the battle host feeds (notify_tactical). Story type (SingleObjectName family unless
// noted; name = the subject's type name unless `name` is given; filter owner = the subject's owner):
//   Destroyed          STORY_TACTICAL_DESTROY (Destroy: Param1 type names, empty = any; Param3 count) + a named
//                      hero (or a team carrying one) STORY_DEFEAT_HERO
//   HardpointDestroyed STORY_ATTACK_HARDPOINT (name = the hardpoint's name; filter owner = the attacker's: `other`
//                      object or `player`)  [S9 CombatSystem::hardpoint_events / AbilitySystem hardpoint_destroyed]
//   Reinforced         STORY_REINFORCE        UnitArrived  STORY_UNIT_ARRIVED     FullStop  STORY_FULL_STOP
//   Garrisoned         STORY_GARRISON_UNIT    HeroCaptured STORY_CAPTURE_HERO
//   StructureCaptured  STORY_CAPTURE_STRUCTURE (generic rule, Param1 = type name)
//   UnitCommanded      STORY_COMMAND_UNIT (name = unit type, target = `other`'s type, distance)
//   UnitGuarding       STORY_GUARD_UNIT (name = guarding unit type, target = guarded `other`'s type)
//   BaseDestroyed      STORY_DESTROY_BASE (generic rule, Param1 = type name)
//   LoadTacticalMap    STORY_LOAD_TACTICAL_MAP (generic rule, Param1 = name)
//   SelectUnit         STORY_SELECT_UNIT (L1/S4 wave 1 tactical UI, from notify_ui with `tactical`; filter = its owner)
//   ClickGui           STORY_CLICK_GUI  } String class: name = the HUD component / speech event; filter owner = the
//   SpeechDone         STORY_SPEECH_DONE} acting player
enum class TacticalStoryEvent : uint8_t {
    Destroyed = 0, HardpointDestroyed, Reinforced, UnitArrived, FullStop, Garrisoned, HeroCaptured, StructureCaptured,
    UnitCommanded, UnitGuarding, BaseDestroyed, LoadTacticalMap, SelectUnit, ClickGui, SpeechDone, Count
};
std::string_view tactical_story_event_name(TacticalStoryEvent e);  // "Destroyed", ...
std::string_view tactical_story_event_type(TacticalStoryEvent e);  // "STORY_TACTICAL_DESTROY", ...

struct TacticalStoryNotification {
    TacticalStoryEvent kind = TacticalStoryEvent::Count;
    int32_t object = 0;     // subject ObjectId in the battle's store (resolved at the call; 0 = none)
    int32_t other = 0;      // killer / attacker / target / guarded object (0 = none)
    int32_t player = -1;    // filter owner when the object is gone (-1 = the subject's owner)
    std::string name;       // explicit name (hardpoint name; type name of a removed subject)
    double distance = -1.0; // UnitCommanded: destination distance to the target (< 0 unknown)
    // Wave 2b: UnitCommanded = the order's destination (COMMAND_UNIT data2, SCRIPT_GAPS.md §4.3). Without it the
    // `other` object's position stands in [port].
    bool has_position = false;
    double x = 0.0, y = 0.0, z = 0.0;
};

// A LINK_TACTICAL reward fired by a galactic plot (Reward_Param1..: planet, SPACE / LAND / GROUND, attacker
// faction, map, defender faction, units, plots file, ...). Recorded for the battle host (a story-forced battle)
// and for begin_battle's plots lookup (consumed there).
struct StoryLinkedTactical {
    uint64_t frame = 0;
    int32_t plot_set = -1;
    std::vector<std::string> params;  // Reward_Param1..N as text
    bool used = false;                // its plots were loaded by a begin_battle
};

// A game-side trigger in story terms (one line of the trigger log).
struct StoryTrigger {
    uint64_t frame = 0;  // global frame of the service that routed it
    uint32_t index = 0;  // index in the session's trigger stream
    std::string kind;    // galactic_event_name() or "TacticalStarted" / "TacticalEnded"
    galactic::GalacticEvent event;  // raw payload (galactic kinds)
    std::string planet, other_planet;  // planet names ("" = none)
    std::string type;                  // object type name ("" = none)
    std::string faction, other_faction;  // faction names of player / other_player ("" = none)
    std::vector<std::string> units;      // fleet events: unit type names of the fleet (content order)
    std::string text() const;            // canonical one-line form (log)
};

// One fired story event with its cause (one line of the fired log).
struct StoryLogEntry {
    uint64_t frame = 0;
    uint32_t sequence = 0;   // per plot set fired-log sequence
    uint32_t plot_set = 0;
    std::string plot, event, type, params, reward;
    std::string via;  // "clock" (update / settle), "trigger:<kind>#<index>", "lua", "load"
    std::string text() const;
};

class StorySession {
public:
    static constexpr int kMaxRounds = 8;

    // `store` = the galactic mode's primary store (scripts' object handles); all references must outlive
    // the session.
    StorySession(galactic::GalacticSim& sim, objects::ObjectStore& store, const objects::GameObjectTypes& types,
                 GameRandom& rng);
    ~StorySession();
    StorySession(const StorySession&) = delete;
    StorySession& operator=(const StorySession&) = delete;

    // Loads the plot sets and starts their scripts (see the header comment). False when a plots file
    // cannot be read; Lua errors are diagnostics only.
    bool start(const StoryStart& s, std::vector<std::string>* diagnostics = nullptr);

    // T9. `frame` = global frame, `mode_seconds` = galactic mode time.
    void service(uint64_t frame, double mode_seconds);
    // The T9 time argument (STORY.md 4.7 / 7.3 [H]) = (float)(mode frame) x seconds-per-frame (1/30 as a float),
    // computed in float and widened (StoryModel::t9_seconds). A pure function of the mode frame: never wall time, so
    // identical for every worker count and compiler. SimHost::tick_once passes it to service(); the tactical T9
    // uses it with (frame - begin frame).
    static double t9_seconds(uint64_t mode_frame);
    // Host events (tactical mode pushed / popped over the galaxy); routed at the next service.
    void host_event(StoryHostEvent e, uint64_t frame);
    // UI / player-action notification (P2 HUD, galactic dialogs, P3 speech / movie end). Call on the sim
    // thread between ticks; queued in call order and routed at the next service (after the galactic
    // stream and host events) [port: the original dispatches synchronously, STORY.md 4.1]. Single-player:
    // a local UI action is not part of the lockstep command stream (multiplayer would have to send it as
    // a command; story events are Multiplayer-gated anyway, STORY.md 4.3). Not hashed while queued.
    void notify_ui(const StoryUiNotification& n);
    // Where script orders go (CommandPipeline::execute_direct at the VM frame); nullptr = not issued.
    // The host sets it before every service (its pipeline is rebuilt by replay_from / new sessions).
    void set_command_pipeline(commands::CommandPipeline* pipeline);
    // OPT2 t04: fan-out for the scheduled Lua collections of the plot VMs at the start of every service
    // (lua/gc_schedule.hpp); any value gives identical results. Default serial.
    void set_lua_gc_parallel_for(ParallelFor pf) { lua_gc_pf_ = std::move(pf); }

    // ---- L1/S4 wave 2a: battles and the tactical story (header comment) ----
    // At the tactical push (after the battle's store and bridge exist, before its first tick): battle snapshot,
    // STORY_SPACE/LAND_TACTICAL to the participants' galactic plot sets (synchronous), tactical plot set + VMs.
    // A battle still attached is ended first (end_battle(-1)). Returns true when a tactical plot set was
    // loaded (false: no plots for this battle, or a plots file that cannot be read - see `diagnostics`).
    // Tactical plot owner: s.local_player, else the player of the plot set whose STORY_*_TACTICAL event named the
    // plots, else the first participant with a galactic plot set, else the attacker.
    bool begin_battle(const StoryBattleStart& s, uint64_t frame, std::vector<std::string>* diagnostics = nullptr);
    // Before the tactical pop: STORY_VICTORY (n.name = the winner's faction) to the tactical plot set, then the
    // tactical VMs / plot set are released (their log lines stay). No-op without an attached battle.
    void end_battle(int32_t winner, uint64_t frame);
    bool battle_attached() const;
    // Tactical notification (battle host, sim thread): names / owners resolved now from the battle's store,
    // queued and routed at the next service. Ignored without an attached battle.
    void notify_tactical(const TacticalStoryNotification& n);
    // L1 systems (TSW_PLANET_DESTROY §7): a tactical super weapon destroyed galactic planet (destroyer = its owner)
    // during the attached battle. STORY_PLANET_DESTROYED goes to the tactical plot set at once (the current mode's story
    // manager); the galactic plots never hear it (the poll marks it seen; its stream event carries value 1 and is skipped).
    void planet_destroyed_in_battle(PlanetIndex planet, int32_t destroyer, uint64_t frame);
    // The tactical plot set while a battle is attached (nullptr otherwise).
    const lua::StoryModel* tactical_model() const;
    lua::StoryModel* tactical_model();
    const std::vector<StoryLinkedTactical>& linked_tactical() const { return linked_; }
    // S4: the tactical plot set's player (the original's "local player" of COMMAND_UNIT / GUARD_UNIT, SCRIPT_GAPS.md
    // §4.3 / §4.4 [port: the plot owner stands in for the machine's local player]); -1 without a tactical plot set.
    int32_t tactical_player() const;
    // L1/S4 wave 1 (game host): the tactical plot set's plots file ("" without one) - the STORY_MISSION_LOST name the game
    // host raises when the local player loses that battle (shipped Param1 values are these file names).
    std::string tactical_plots_file() const;
    // Wave 2b (SCRIPT_GAPS.md §4.7): a pending corruption mission (the original's GMM +0x1f4 corrupting player /
    // +0x1f8 corruption type), set by whoever starts the mission's battle (no source in the port yet). At the next
    // end_battle: the winner is that player -> the corruption is applied to the battle planet
    // (GalacticSim::set_corruption) and STORY_CORRUPTION_TACTICAL_COMPLETE goes to the galactic plot sets, else
    // STORY_CORRUPTION_TACTICAL_FAILED; then the mission is cleared. player < 0 clears it.
    void set_corruption_mission(int32_t player, int32_t corruption_type);
    // --- S4 (L1_S4_REMAINING.md §4.4): the battle-load screen (battle host, before the map is fixed): STORY_LOAD_TACTICAL_MAP
    // (planet, kind 0 land / 1 space) dispatched at once to the galactic plot sets; a SET_TACTICAL_MAP reward fills the
    // land / space map slot, which take_tactical_map() returns and clears (the battle start reads and clears it).
    void load_tactical_map(PlanetIndex planet, bool space, uint64_t frame);
    std::string take_tactical_map(bool space);
    // --- end S4 ---
    // --- fog follow-up: story reward ENABLE_FOW (FOG_OF_WAR.md §6.6, reward 0x1c) from any plot set: fn(Reward_Param1
    // given, atoi(Reward_Param1)). The battle host sets it (CampaignBattles: the running battle's
    // FogOfWar::reward_enable_fow, else its carried FOW-active / render option). Unset: the reward does nothing [port].
    void set_enable_fow_hook(std::function<void(bool has_param, int32_t value)> fn) { enable_fow_ = std::move(fn); }
    // --- end fog follow-up ---
    // --- story t06 wiring (the tactical victory monitor, BT §3.1a): rewards of the TACTICAL plot set go to the running
    // battle. ENABLE_VICTORY -> enable(Reward_Param1 given, atoi(Reward_Param1)); VICTORY -> victory(Reward_Param1 = the
    // faction name as written, atoi(Reward_Param2) != 0 = short and silent); the battle host resolves the faction (unknown:
    // "Unable to find faction %s to force victory", nothing). Unset: nothing. A galactic plot set's rewards (story t11) act
    // on the galactic monitor directly: GalacticSim::victory_hold_raise / victory_hold_release / story_victory (the
    // campaign end, galactic::check_victory); an unknown VICTORY faction is logged in lua_messages(). ---
    void set_victory_hooks(std::function<void(bool has_param, int32_t value)> enable,
                           std::function<void(const std::string& faction, bool short_silent)> victory) {
        enable_victory_ = std::move(enable);
        victory_ = std::move(victory);
    }
    // --- OI host (owner: game host agent): presentation tap of every batch T9 drains from the galactic event stream
    // (round 0 = the T2..T8 events; rounds 1+ = events raised inside T9 by Lua calls / story rewards, e.g. Lua
    // Activate_Ability's SpecialDialog for a human). Read-only: called before the batch is routed; it must not change
    // sim state. Not hashed. ---
    void set_drain_tap(std::function<void(int round, const std::vector<galactic::GalacticEvent>& events)> fn) {
        drain_tap_ = std::move(fn);
    }
    // --- end OI host ---
    // --- OI galactic (owner: galactic agent): BT §1.6 story lock 0x4472a0 ("this planet's battle must be fought"),
    // written by the story rewards DISABLE_AUTORESOLVE / ENABLE_AUTORESOLVE (global flag +0x41) and
    // RESTRICT_AUTORESOLVE (Param1 planet, Param2 1 add / 0 remove: the planet list +0x308) [M: field wiring]. Source for
    // CampaignBattles' `autoresolve_locked` seam (the host sets it: [&](PlanetIndex p) { return story.autoresolve_locked(p); }).
    // No source is specified for the story-sequence-active flag (0x140b154d0), so none is offered. Not hashed. ---
    bool autoresolve_locked(PlanetIndex planet) const;
    // --- end OI galactic ---

    // ---- attached Lua families (nullptr in builds without engine/lua / the players family) ----
    lua::PresentationLog* presentation();
    lua::ObjectBridge* object_bridge();
    lua::LuaPlayerWorld* player_world();

    // ---- results ----
    const std::vector<StoryTrigger>& triggers() const { return triggers_; }
    const std::vector<StoryLogEntry>& log() const { return log_; }
    // Lua errors / messages (not part of the checksum or the golden log).
    const std::vector<std::string>& lua_messages() const { return lua_messages_; }
    size_t plot_set_count() const { return sets_.size(); }
    const lua::StoryModel& model(size_t set) const;
    lua::StoryModel& model(size_t set);
    int32_t plot_set_player(size_t set) const;
    size_t script_count() const;
    // Text form of the whole run: "# header", then every trigger ("T ...") and fired event ("F ...") in
    // the order they happened. Byte-identical across worker counts and compilers.
    std::string log_text(std::string_view header = {}) const;

    // CRC system "story".
    void hash(StateHasher& h) const;

    // ---- N1 save / load (owner: n1-host; SimHost chunks "story", "story.lua", "story.bridge", "story.players") ----
    // "" when the session can be saved, else why not. Stage 2 (n2-host): nothing refuses any more (an attached
    // tactical story saves with the battle).
    std::string save_blocker() const;
    // "story" (version 2; version 1 = stage 1, no tactical part, still read): session state (T9 clock, trigger stream +
    // queued host / UI triggers, fired log, battle snapshot, LINK_TACTICAL records, planet polls, corruption mission,
    // tactical map slots, autoresolve lock) and every plot set's StoryModel, then the attached tactical story: start
    // frame, seconds per frame, the tactical plot set (identity: player, number, plots file, plot / script counts; poll
    // state; its StoryModel), the queued tactical notifications and the session-owned ObjectBridge (only when the battle
    // host passed none; the host's bridge is the host's to save). Definitions are not saved: read() runs on a session
    // start()ed with the same plot sets and fails (false) when the plot sets / scripts differ or a model refuses its data.
    // Mid-battle load (SimHost two-pass load): read() without an attached tactical story (pass 1) restores the galactic
    // part and stages the tactical identity; the next begin_battle (the battle host's rebuild) then attaches the tactical
    // story WITHOUT the battle-start trigger / STORY_*_TACTICAL dispatch and loads the saved plots file for the saved
    // player and set number; read() with the tactical story attached (pass 2) checks the identity and restores it.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
    // "story.lua" (version 2: + the tactical set's scripts after the galactic sets'): every plot script's LuaVm
    // (LuaVm::write) + its service gate (rate, last service), set / script order. Without an attached tactical story the
    // saved tactical scripts are skipped (pass 1).
    void write_scripts(ByteWriter& w) const;
    bool read_scripts(ByteReader& r);
    // "story.bridge" / "story.players": the families shared by the story VMs (lua::ObjectBridge script state,
    // lua::LuaPlayerWorld). Written through their write / read once engine/lua provides them; until then empty and
    // read() false (pending).
    void write_bridge(ByteWriter& w) const;
    bool read_bridge(ByteReader& r);
    void write_players(ByteWriter& w) const;
    bool read_players(ByteReader& r);
    // "story.presentation" (N1 stage 2): the lua::PresentationLog shared by the story VMs (open / finished blocks the
    // VMs wait on, presentation state, undrained events); its write / read through write_if_saveable.
    void write_presentation(ByteWriter& w) const;
    bool read_presentation(ByteReader& r);

private:
    struct Script;
    struct Set;
    void route(const StoryTrigger& t);
    std::vector<std::pair<std::string, lua::StoryNotification>> notifications(const StoryTrigger& t) const;
    std::string planet_type_name(uint32_t planet) const;
    void poll(size_t set);
    void collect(size_t set, std::string_view via);  // log new fired entries, drain script errors
    void apply_reward(Set& s, const std::string& type, const std::vector<std::string>& params);
    void invade_planet_reward(int32_t plot_player, PlanetIndex planet, const std::string& company);  // a1h t04
    void service_script(size_t set, Script& sc, uint64_t frame, double seconds);
    StoryTrigger translate(const galactic::GalacticEvent& e) const;
    std::string faction_name(int32_t player) const;
    void lua_note(const Script& sc, std::string_view what);

    galactic::GalacticSim* sim_;
    objects::ObjectStore* store_;
    const objects::GameObjectTypes* types_;
    GameRandom* rng_;
    const FileProvider* files_ = nullptr;
    lua::GlobalValueStore* global_values_ = nullptr;  // Int-final: StoryStart::global_values
    ParallelFor lua_gc_pf_ = serial_for;               // OPT2 t04
    // Lua families shared by every VM of the session (declared before sets_: the VMs go first).
    struct Families;
    std::unique_ptr<Families> lua_;
    std::vector<std::unique_ptr<Set>> sets_;
    std::vector<StoryTrigger> triggers_;
    std::vector<StoryTrigger> pending_host_;
    std::vector<StoryTrigger> pending_ui_;
    std::vector<uint8_t> destroyed_seen_;  // per planet row: destroyed flag at the last poll (PLANET_DESTROYED)
    bool destroyed_init_ = false;
    void poll_planets(std::vector<StoryTrigger>& batch);
    void add_unit_names(objects::ObjectHandle h, std::vector<std::string>& out) const;
    std::vector<galactic::GalacticEvent> scratch_;
    // ---- wave 2a ----
    struct Battle;    // last battle snapshot (begin_battle), consumed by the result routing
    struct Tactical;  // attached tactical story
    std::unique_ptr<Battle> battle_;
    std::unique_ptr<Tactical> tactical_;
    std::vector<StoryLinkedTactical> linked_;
    std::vector<int32_t> owner_seen_;  // per planet row: owner at the end of the previous service (INVASION_BOUNCED)
    std::vector<std::string> tactical_headers_;  // log_text header line per tactical plot set loaded
    int32_t corruption_player_ = -1, corruption_type_ = -1;  // wave 2b: pending corruption mission
    std::string tactical_map_[2];                              // S4: SET_TACTICAL_MAP land / space slots
    std::function<void(bool, int32_t)> enable_fow_;            // fog follow-up: ENABLE_FOW reward hook
    std::function<void(bool, int32_t)> enable_victory_;        // story t06 wiring: ENABLE_VICTORY (tactical plots)
    std::function<void(const std::string&, bool)> victory_;    // story t06 wiring: VICTORY (tactical plots)
    std::function<void(int, const std::vector<galactic::GalacticEvent>&)> drain_tap_;  // OI host
    bool autoresolve_disabled_ = false;              // OI galactic: story lock global flag (+0x41)
    std::vector<PlanetIndex> autoresolve_planets_;   // OI galactic: story lock planet list (+0x308), insertion order
    void route_corruption_mission(int32_t winner, uint64_t frame);
    void collect_set(Set& s, uint32_t number, std::string_view via);
    void service_script_in(Set& s, uint32_t number, Script& sc, uint64_t frame, double seconds);
    void service_tactical(uint64_t frame);
    void route_tactical(uint64_t frame);
    void poll_tactical();
    // number_override: the set number of a resumed battle (N1 stage 2); default = after the galactic sets and headers.
    bool load_tactical(const std::string& plots, int32_t player, std::vector<std::string>* diagnostics,
                       uint32_t number_override = UINT32_MAX);
    // ---- N1 stage 2 (n2-host): mid-battle load (read() pass 1 stages, begin_battle resumes) ----
    struct TacticalResume {
        bool has_set = false;
        int32_t player = -1;
        uint32_t number = 0;
        std::string file;
    };
    std::unique_ptr<TacticalResume> resume_tactical_;
    void attach_tactical(const StoryBattleStart& s, uint64_t frame);  // tactical_ for a battle (begin_battle)
    std::vector<StoryLogEntry> log_;
    std::vector<std::string> lua_messages_;
    std::vector<std::string> lines_;  // "T ..." / "F ..." lines in happening order (log_text)
    uint64_t frame_ = 0;
    double seconds_ = 0.0;
};

}  // namespace alamo::sim

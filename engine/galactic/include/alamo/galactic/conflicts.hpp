// S10 "Battle transitions": galactic conflict queues, the conflict state machine, auto-resolve and the
// battle write-back (docs/re/BATTLE_TRANSITIONS.md = BT). Owned by the host; attached to a GalacticSim
// through its ConflictHooks (campaign.hpp). Nothing here runs unless attach() was called, so existing
// galactic runs and goldens are unchanged.
//
// QUEUES (BT §1.1-1.3). Two FIFO vectors of ConflictInfo: space conflicts are queued when a fleet stops in
// an orbit whose entry test found a conflict (hostile combat fleet or starbase); land conflicts only by an
// explicit invade (GalacticSim::invade against hostile ground forces, Lua / story through land_invade()).
// The defender is derived when not given (§1.2 / §1.3); a record with attacker == defender is not queued.
// [port] a space conflict whose (planet, attacker) is already queued is not queued again. A planet with a
// queued conflict is flagged in_battle (impassable for the path finder).
//
// PER TICK (BT §1.4), on_service_tail() = the end of the galactic GOM service, every galactic tick (also
// while the galactic pause is raised): the space head runs the state machine, then the land head when no
// conflict is active. One conflict is active at a time (the shared state at GOM+0x618). Heads are
// re-validated on every pass except while an auto-resolve or a battle runs. Not modelled: the story block
// (§1.4 step 2), MP votes (§1.6 multiplayer), the pre-battle cinematic (state 2 goes straight to 3).
//
// STATES (BT §1.5): Idle(-1) validate + tag -> Decide(4): AI vs AI or an overrun -> AutoSetup(6) forced;
// a human participant -> the pending-battle choice (decide callback, else ConflictConfig::human_choice;
// Pending waits in Vote(0) until choose()) -> Fight: StartBattle(3) / AutoResolve: AutoSetup(6) forced /
// Retreat: AutoSetup with the human flagged retreating. AutoSetup -> AutoRunning(7): resolved when forced
// (same tick), when a retreat is flagged, or once AutoResolveDisplayTime elapsed [port: counted in
// galactic service ticks, BT open item 4]. StartBattle: map + forces (BattleRequest) -> start_battle
// callback (the host pushes the tactical mode) -> InBattle until finish_battle(); without a host (or when
// it refuses) the conflict is auto-resolved instead. The galactic pause (BT §1.6 end) is raised while a
// choice or an auto-resolve waits and released on every exit; the battle itself suspends the galaxy.
// Story-linked battles (BT §2.9, start_linked) bypass the machine: queued at the front and started at once.
//
// BATTLE SETUP (BT §2.1-2.3): map = planet type Space/Land_Tactical_Map, else the planet's stored map,
// else the host's random picker (stored on the planet), then the record's explicit map, then the
// fallbacks _Space_Temperate1.ted / _Desert_L5_01.ted. Forces: space = population-cap split (§2.3,
// defender first): start force + reinforcement pool per player; land = every invader / defender unit
// starts [port: 0x2bc6a0 untraced].
//
// WRITE-BACK (BT §3, §4.1): destroyed objects killed; a non-Is_Playable loser is wiped from the galaxy;
// retreat player R from the victory type; starbases kept / downgraded / destroyed; a winning attacker
// clears the planet's land / orbital queue and (land) lands its survivors; fleets of R (or of the loser
// when R = -1) retreat (destination finders TACTICAL_GAPS 4.5); surface units of R go up into a fleet and leave
// unless R controls the orbit; a super weapon the winner cannot kill makes the winner R (TACTICAL_GAPS 4.8); other queued conflicts whose defender lost get the winner as defender; story events
// BattleWon / BattleLost / BattleRetreat go to the galactic event stream.
//
// DETERMINISM: vectors only, list orders as documented; the only randomness is auto_resolve's survival
// picks on the session's synced RNG. hash() covers queues, state and the result log ("galactic.conflicts").
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/galactic/auto_resolve.hpp"
#include "alamo/galactic/campaign.hpp"

namespace alamo::data {
class GameDatabase;
}
namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::galactic {

// ---- rules ----------------------------------------------------------------------------------------------------
struct ConflictTypeInfo {
    uint64_t categories = 0;                  // CategoryMask
    int32_t population = 0;                   // Population_Value
    int32_t space_population_override = -1;   // Space_Override_Population_Value (-1 = not set)
    bool hero = false;                        // Is_Named_Hero
    bool super_killer = false;                // Is_Super_Weapon_Killer [L field name]
    std::vector<std::pair<uint32_t, int32_t>> members;  // Squadron_Units / Company_Units: (type, count), first-seen order
    uint32_t lower_starbase = kNoIndex;       // next lower starbase level type (last name digit - 1) [M]
    // planet types
    std::string space_map, land_map;          // Space_Tactical_Map / Land_Tactical_Map (trimmed)
    int32_t terrain = -1;                     // TEMPERATE 0 .. URBAN 6, SPACE 7; -1 unknown
};
struct ConflictFactionInfo {
    float space_unit_cap = 20.0f;             // Space_Tactical_Unit_Cap (+0x2cd0)
    float space_retreat_countdown = 10.0f;    // Space_Retreat_Countdown_Seconds (+0x20b0)
    float land_retreat_countdown = 10.0f;     // Land_Retreat_Countdown_Seconds (+0x2190)
    int32_t map_faction = 0;                  // random-map faction index: Rebel 0, Empire 1, Pirates 2, Underworld 4, Hutts 5
};
struct ConflictRules {
    AutoResolveConstants auto_resolve;
    ContrastTable contrast;
    float overrun_multiple = 0.0f;            // Overrun_Auto_Resolve_Multiple (vanilla commented out: 0 = off)
    bool queue_tactical_battles = true;       // Strategic_Queue_Tactical_Battles
    float space_auto_resolve_delay = 0.01f;   // Space_Auto_Resolve_Delay_Seconds
    float land_auto_resolve_delay = 0.01f;    // Land_Auto_Resolve_Delay_Seconds
    float space_retreat_allowed = 30.0f;      // Space_Retreat_Allowed_Countdown_Seconds
    float land_retreat_allowed = 30.0f;       // Land_Retreat_Allowed_Countdown_Seconds
    bool land_base_destruction_forces_retreat = false;   // Land_Base_Destruction_Forces_Retreat
    bool space_station_destruction_forces_retreat = false;  // Space_Station_Destruction_Forces_Retreat
    uint64_t super_category = 0;              // GameObjectCategoryType "Super"
    // OI galactic (TACTICAL_GAPS 4.2): land force split 0x2bc6a0.
    std::vector<std::string> preferred_pathfinder_types;  // Preferred_Pathfinder_Types (GC +0x119f0), order kept
    std::string raid_required_faction;                     // Raid_Force_Required_Faction (+0x11750), empty = any
    uint64_t raid_free_mask = 0, raid_limited_mask = 0;    // Raid_Force_Free / _Limited_Object_Category_Mask
    int32_t raid_max_limited = 0, raid_max_heroes = 0;     // Raid_Force_Max_Limited_Objects / _Max_Heros
    std::vector<ConflictTypeInfo> types;      // by TypeIndex (shorter = defaults)
    std::vector<ConflictFactionInfo> factions;  // by GalacticRules faction index (shorter = defaults)

    const ConflictTypeInfo& type(uint32_t t) const;
    const ConflictFactionInfo& faction(uint32_t f) const;
};

// GameConstants, FactionFiles (caps, retreat countdowns), the typed object view and
// Data\Scripts\Library\PGAICommands.lua (contrast table). Missing pieces keep the defaults (diagnostics).
bool load_conflict_rules(const FileProvider& provider, const data::GameDatabase& db, const objects::GameObjectTypes& types,
                         const GalacticRules& galactic, ConflictRules& out, std::vector<std::string>* diagnostics = nullptr);

// ---- records ---------------------------------------------------------------------------------------------------
enum class ConflictKind : uint8_t { Land = 1, Space = 2 };
enum class BattleChoice : uint8_t { Pending = 0, Fight = 1, AutoResolve = 2, Retreat = 3 };
enum class ConflictState : int8_t {
    Idle = -1, Vote = 0, StartBattle = 3, Decide = 4, Dialog = 5, AutoSetup = 6, AutoRunning = 7, InBattle = 10
};
// VictoryType values (BT §2.5, enum table 0x341ff0).
enum VictoryType : uint8_t {
    kVictoryAllDestroyed = 0x0b, kVictoryAllRetreated = 0x0c, kVictoryRogueBaseDestroyed = 0x0d,
    kVictoryEnemyBaseDestroyed = 0x0e, kVictoryStarbaseDestroyed = 0x0f, kVictoryStory = 0x10,
};

struct ConflictInfo {  // ConflictInfoStruct (0x58 bytes)
    ConflictKind kind = ConflictKind::Space;
    PlanetIndex planet = kInvalidPlanet;
    int32_t attacker = kNoPlayer, defender = kNoPlayer;
    std::vector<ObjectHandle> units;  // land: the invading units (+0x10)
    std::string map;                  // explicit tactical map (+0x30)
};

struct BattleForces {
    int32_t player = kNoPlayer;
    std::vector<ObjectHandle> start;    // GOM+0x498: placed at battle start
    std::vector<ObjectHandle> reserve;  // GOM+0x4d8: reinforcement pool
};
struct BattleRequest {
    ConflictInfo info;
    std::string map;                    // map name ("Data\Art\Maps\" is the loader's prefix)
    int32_t base_level = 0;             // starbase level (space) / ground-base level (land)
    std::vector<BattleForces> forces;   // defender first, then attacker
    ObjectHandle starbase;              // space: the planet's starbase (owned by a participant), else null
    std::vector<ObjectHandle> structures;  // land: the defender's ground structures
    bool land_base_destruction_forces_retreat = false;
    bool space_station_destruction_forces_retreat = false;
};
struct BattleResult {
    int32_t winner = kNoPlayer;
    uint8_t victory = kVictoryAllDestroyed;
    int32_t retreated = kNoPlayer;      // the player the retreat coordinator reports
    std::vector<ObjectHandle> destroyed;  // galactic objects whose tactical counterparts all died
    bool starbase_destroyed = false;
};

struct ConflictLogEntry {
    uint64_t tick = 0;          // conflict-service tick
    int32_t galactic_frame = 0;
    ConflictKind kind = ConflictKind::Space;
    PlanetIndex planet = kInvalidPlanet;
    int32_t attacker = kNoPlayer, defender = kNoPlayer;
    uint8_t how = 0;            // 0 dropped, 1 auto-resolved, 2 tactical battle
    int32_t winner = kNoPlayer, loser = kNoPlayer, retreating = kNoPlayer;
    uint32_t attacker_losses = 0, defender_losses = 0;
    uint32_t rng_draws = 0;
    std::string map;            // tactical battles
    std::string text(const GalacticSim& sim) const;
};

struct ConflictConfig {
    BattleChoice human_choice = BattleChoice::AutoResolve;  // headless answer to the pending-battle choice
    bool fight_ai_battles = false;  // [port test option] AI vs AI conflicts are fought too
};

class ConflictManager final : public ConflictHooks {
public:
    // `sim`, `rules` and `rng` (the session's SYNCED instance) must outlive the manager.
    ConflictManager(GalacticSim& sim, const ConflictRules& rules, SimRng& rng, ConflictConfig cfg = {});
    ~ConflictManager() override;
    ConflictManager(const ConflictManager&) = delete;
    ConflictManager& operator=(const ConflictManager&) = delete;

    void attach() { sim_->set_conflict_hooks(this); }
    void detach();
    ConflictConfig& config() { return cfg_; }

    // ---- host callbacks ----
    // Human pending-battle choice (state 4/0). Pending = wait for choose(). Absent: config().human_choice.
    std::function<BattleChoice(const ConflictInfo&)> decide;
    // Starts the tactical battle (pushes the mode). False = refused (the conflict is auto-resolved).
    std::function<bool(const BattleRequest&)> start_battle;
    // Raises (true) / releases (false) one level of the galactic pause. Absent: GalacticSim push/pop_pause.
    std::function<void(bool)> pause;
    // Random map picker 0x47bf40 (space, terrain index, defender map-faction index, base level). Empty
    // answer = fallback map. Absent: fallback map.
    std::function<std::string(bool space, int32_t terrain, int32_t faction, int32_t base_level)> pick_map;
    // OI galactic: the auto-resolve survival roll (STRATEGIC_GAPS 1.1, 0x60a140 -> learning writer 0x58a9a0): once per
    // combatant of a written-back auto-resolve, combatant order, with its owner, galactic store type (the callee applies
    // the company-leader substitution) and whether it survived. Absent: nothing. The host wires it to
    // GalacticAiWorld::record_auto_resolve_survival (engine/ai); it must not change the galactic sim.
    std::function<void(int32_t owner, uint32_t type, bool survived)> on_survival_roll;

    // ---- ConflictHooks ----
    void on_space_conflict(PlanetIndex planet, int32_t attacker) override;
    bool on_invade(int32_t attacker, PlanetIndex planet, std::span<const ObjectHandle> units) override;
    void on_service_tail() override;

    // ---- direct entry points (Lua / story / tests) ----
    bool initiate_space_conflict(PlanetIndex planet, int32_t attacker, int32_t defender = kNoPlayer,
                                 std::string map = {}, bool at_front = false);
    bool land_invade(PlanetIndex planet, std::vector<ObjectHandle> units, int32_t attacker, int32_t defender = kNoPlayer,
                     std::string map = {}, bool at_front = false);
    // Story-linked battle (BT §2.9.3, LINK_TACTICAL record 0x28bd70 started by 0x28f590): the conflict is queued at the
    // front (land_invade / initiate_space_conflict, at_front) and its tactical battle starts within this call. The state
    // machine never sees it (no validation, overrun, auto-resolve or pending choice); the battle end is the normal
    // finish_battle. Forces (§2.9.5): not sandbox = none (the map and the mission's plots supply every unit); sandbox =
    // the space / land split for the human sides only, AI sides get nothing. False, nothing queued, when another
    // conflict is active (state not Idle: the caller retries) or the queueing is refused (attacker == defender, no
    // defender; [port] the original starts anyway, §2.9.7); false, dropped and logged, when the start fails (no host,
    // map not loaded; [port] the original leaves it queued, §2.9.4).
    struct LinkedBattle {
        ConflictKind kind = ConflictKind::Land;
        PlanetIndex planet = kInvalidPlanet;
        int32_t attacker = kNoPlayer, defender = kNoPlayer;
        std::string map;                  // Reward_Param4
        bool sandbox = false;             // Reward_Param8
        std::vector<ObjectHandle> units;  // land: the record's units (the original's last-invade vector)
    };
    bool start_linked(LinkedBattle b);
    // Answer to a Pending choice (Vote state). Ignored otherwise.
    void choose(BattleChoice c);
    // The host's tactical battle ended (state InBattle): write-back, next conflict.
    void finish_battle(const BattleResult& result);
    // --- P4 sim (owner: galactic agent) ---
    // AutoResolve.Set_Retreating(player) (BT §4.6 vf5 0x60cf00; P4_SIM_SYSTEMS §9.2: Withdrawl event 49 has no payload,
    // player = the header's player id, and its executor ignores this result, so an early / second withdraw is a silent
    // drop): only while an auto-resolve runs (AutoRunning = after Begin), once, for the head's attacker or defender
    // [port: other players refused]; the flagged retreat resolves on the next pass with the retreat attritions.
    // The pending dialog's Retreat stays choose(BattleChoice::Retreat).
    bool set_retreating(int32_t player);
    int32_t retreating() const { return retreating_; }
    // The tactical HUD's auto-resolve (BT §4.5 IDC_AUTORESOLVE: vf1/vf2 -> vf8 gather -> vf4 Begin -> vf6 Update(force))
    // for the running battle (state InBattle): the objects the battle already destroyed (`so_far.destroyed`, starbase
    // flag) are removed first, the survivors become the combatants (auto_setup order) with from_tactical set (budgets x
    // Auto_Resolve_Tactical_Multiplier), the resolution is forced within the call and written back as an auto-resolve
    // (log how = 1). The host ends the tactical mode without finish_battle. Returns the winner, kNoPlayer when no battle
    // is active. [port] "gathers combatants from the current tactical mode" = their galactic objects.
    int32_t resolve_battle(const BattleResult& so_far);
    // --- end P4 sim ---

    // ---- queries ----
    const std::vector<ConflictInfo>& space_queue() const { return space_q_; }
    const std::vector<ConflictInfo>& land_queue() const { return land_q_; }
    ConflictState state() const { return state_; }
    bool battle_active() const { return state_ == ConflictState::InBattle; }
    const BattleRequest& active_request() const { return request_; }
    const std::vector<ConflictLogEntry>& log() const { return log_; }
    // Space force split 0x2bd500 (§2.3 rule 4) for one player's units (list order).
    void split_space_forces(int32_t player, const std::vector<ObjectHandle>& units, BattleForces& out) const;
    // OI galactic: land force split 0x2bc6a0 (TACTICAL_GAPS 4.2): the defender's surface units all start (no cap); the
    // attacker's record units all start when they pass the Raid_Force_* test, else only the lead unit (first
    // Preferred_Pathfinder_Types match among non-heroes, then heroes; else the first non-hero, else the first hero; the
    // fleet-lead / forced-type rule (a) has no source) starts and every other attacker unit in orbit goes to reserve.
    void split_land_forces(const ConflictInfo& head, BattleForces& defender, BattleForces& attacker) const;
    bool raid_force(int32_t player, const std::vector<ObjectHandle>& units) const;  // 0x432e20
    // OI galactic: retreat destination finders (TACTICAL_GAPS 4.5). Candidates = planets newest first (reverse row order),
    // smallest squared 3-D distance (double, strict <); filters in order: orbit controller = player (starbase owner when
    // level > 0, else the first idle combat fleet in orbit), ground-base / starbase owner = player, owner neutral, any.
    // Fleets: every filter excludes `from` (the battle planet). `came_from` is not used any more (kept for callers).
    PlanetIndex retreat_destination(int32_t player, PlanetIndex from, PlanetIndex came_from = kInvalidPlanet) const;
    // Ground (0x2ba850): the first two filters without exclusion (flag clear: controller then ground base; set: the
    // other order), so `from` itself wins when the player controls its orbit (the units just go up into orbit).
    PlanetIndex retreat_destination_ground(int32_t player, PlanetIndex from, bool flag = false) const;
    // OI galactic: battle end of a corruption mission (TACTICAL_GAPS 4.8, GMM+0x1f4): the battle's deaths are applied
    // [M], the conflict leaves the queue and is logged, and the normal fleet / ground / queue / starbase write-back and the
    // BattleWon / BattleLost events are skipped (the host applies the corruption result). State InBattle only.
    void finish_corruption_mission(const BattleResult& result);

    void hash(StateHasher& h) const;
    void register_crc(CrcRegistry& registry, const std::string& name = "galactic.conflicts") const;
    // N1 save / load (alamo/core/save_file.hpp; layout in src/galactic_save.cpp), chunk "galactic.conflicts": queues,
    // state machine, the auto-resolve in flight, the last result, the active BattleRequest, planet maps and the log.
    // Read after GalacticSim::read (objects resolve through the sim's store). The config and the host callbacks are not
    // saved (re-attached by the host). False and nothing changed on bad / truncated data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    std::vector<ConflictInfo>& queue(ConflictKind k) { return k == ConflictKind::Space ? space_q_ : land_q_; }
    void run(ConflictKind kind);
    bool validate(const ConflictInfo& c) const;
    void drop_head(ConflictKind kind, bool log_it);
    void pop_head(ConflictKind kind);
    void raise_pause();
    void release_pause();
    bool human(int32_t player) const;
    void apply_choice(BattleChoice c, const ConflictInfo& head);
    // participants
    std::vector<ObjectHandle> orbit_units(PlanetIndex planet, int32_t player, bool space_only) const;
    std::vector<ObjectHandle> orbit_cargo(PlanetIndex planet, int32_t player) const;
    std::vector<ObjectHandle> surface_units(PlanetIndex planet, int32_t player) const;
    bool can_fight_space(ObjectHandle h) const;
    float unit_power(ObjectHandle h) const;
    bool has_hero(const std::vector<ObjectHandle>& units) const;
    // auto-resolve
    void auto_setup(const ConflictInfo& head);
    void auto_finish(const ConflictInfo& head);
    ArCombatant combatant_of(ObjectHandle h, bool space) const;
    // battle
    enum class Linked : uint8_t { No, Plain, Sandbox };  // start_linked's force rule (BT §2.9.5)
    bool begin_battle(const ConflictInfo& head, Linked linked = Linked::No);
    std::string choose_map(const ConflictInfo& head, int32_t base_level, bool* ok);
    // write-back
    PlanetIndex nearest_planet(PlanetIndex from, PlanetIndex exclude, int32_t filter, int32_t player) const;
    void retreat_fleets(int32_t player, PlanetIndex planet);
    void retreat_ground(int32_t player, PlanetIndex planet);
    void wipe_player(int32_t player);
    void land_survivors(int32_t attacker, PlanetIndex planet, const std::vector<ObjectHandle>& units);
    void post_results(const ConflictInfo& head, int32_t winner, int32_t loser, int32_t retreat, uint8_t victory);
    void rewrite_defenders(int32_t loser, int32_t winner);
    void refresh_in_battle(PlanetIndex planet);

    GalacticSim* sim_;
    const ConflictRules* rules_;
    SimRng* rng_;
    ConflictConfig cfg_;
    std::vector<ConflictInfo> space_q_, land_q_;
    ConflictState state_ = ConflictState::Idle;
    ConflictKind active_ = ConflictKind::Space;
    uint64_t tick_ = 0, timer_start_ = 0;
    bool force_ = false;
    int32_t retreating_ = kNoPlayer;
    int32_t pause_held_ = 0;
    bool has_choice_ = false;
    BattleChoice choice_ = BattleChoice::Pending;
    // auto-resolve in flight (objects parallel to the combatant list)
    AutoResolveInput ar_in_;
    std::vector<ObjectHandle> ar_objects_, ar_cargo_;
    AutoResolveResult last_ar_;
    BattleRequest request_;
    std::vector<std::string> planet_map_;  // planet data +0x1e0 per row
    std::vector<ConflictLogEntry> log_;
};

}  // namespace alamo::galactic

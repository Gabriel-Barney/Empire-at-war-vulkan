// S10 "Battle transitions" host glue (docs/re/BATTLE_TRANSITIONS.md): attaches a galactic ConflictManager
// (engine/galactic conflicts.hpp) to the running campaign of a SimHost and plays its battles as tactical
// battles (tactical_battle.hpp) on pushed Land/Space modes.
//
//   * ConflictManager::pause -> the galactic mode's pause counter (GalacticMode::push_pause / pop_pause).
//   * ConflictManager::start_battle: the map is loaded at once (MapLoader; a missing map or a map of the
//     wrong kind refuses the battle, which is then auto-resolved [port]); the mode push happens right after
//     the galactic tick (SimHost after-mode-update hook, same tick: BT §5 item 3, no RNG reseed).
//   * Each later tick the hook runs TacticalBattle::tick after the tactical mode tick; on a result the
//     tactical mode is popped (End_Game_Mode) and ConflictManager::finish_battle writes the result back to
//     the galaxy in the same tick (BT §5 item 4); the next queued conflict starts on a later galactic tick.
//   * CRC systems (opt-in, registered by attach()): "galactic.conflicts", "battle.state",
//     "battle.movement", "battle.combat", "battle.abilities" (S10-int) (each hashes "no battle" while none runs).
//   * S10-int: ability commands 40 / 43 / 57 / 58 reach the running battle through SimHost's tactical command
//     hook (installed by attach()).
// Lifetime: attach() after SimHost::start_galactic and before the first tick; the object must stay alive
// for as long as the host ticks or captures CRCs (its hook and hashers capture `this`).
//
// CampaignAggressor is a [port] stand-in for the galactic AI (A1 owns the real one), used by alamo_headless
// to make campaigns produce conflicts: every `period` galactic frames (staggered by player id) a player's
// first idle fleet with combat power moves to the nearest planet held by a hostile player (BFS over the
// route graph, neighbours in route order); fleets parked at a hostile planet with ground units invade.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/sim/battle_catalog.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/terrain/ted.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include <map>

#include "alamo/lua/object_bridge.hpp"  // L1/S4 wave 2a story hookup
#include "alamo/lua/script_gaps.hpp"    // L1/S4 wave 2b story hookup
#include "alamo/lua/fog_bridge.hpp"     // fog follow-up: FogOfWar.* for the tactical story VMs
#endif
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
#include <span>

#include "alamo/ai/tactical/tactical_credits.hpp"
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include "alamo/sim/battle_ai.hpp"  // A1 stage 2 tactical AI hookup
#endif

namespace alamo::sim {

struct CampaignBattleOptions {
    galactic::ConflictConfig conflict;  // human choice (headless default), fight AI-vs-AI battles
    BattleConfig battle;
    bool tactical = true;               // false: every battle is refused -> auto-resolved
    bool crc = true;                    // register the CRC systems
    // S10-int: sim-owned animation (sim_anim.hpp, attached to the host separately); battles drive its tactical
    // world (weapon Attack, flinch gate, locomotor Move / Walk, idles, animated fire bones). Null = none.
    SimAnimation* animation = nullptr;
    // refixf t04: model source of the death-clone plans when `animation` is null (TacticalBattle::set_clone_models; an
    // unattached SimAnimation, e.g. with a test's injected models). Null = each battle reads the catalog's files.
    SimAnimation* clone_models = nullptr;
    // --- L1 systems (owner: tactical-systems agent) ---
    // Space_Retreat_Unit_Increment_Wait_Frames per galactic faction index (load_retreat_increments below; FoC data: 1
    // for the factions that set it). Empty = 0 for every faction [port] (Start_Cinematic_Space_Retreat stagger).
    std::vector<int32_t> retreat_increment_frames;
    std::vector<float> retreat_flight_increments;  // Space_Retreat_Flight_Move_Increment (load_retreat_flight_increments)
    // GameConstants of the L1 systems (load_l1_constants; defaults = the shipped FoC values [port]) and the tactical
    // weather tables (tactical::combat::load_weather_data; null = no weather).
    TacticalBattle::L1Constants l1_constants;
    std::shared_ptr<const tactical::combat::WeatherData> weather;
    std::vector<TacticalBattle::L1Faction> l1_factions;  // Bombardment_* faction data (load_l1_factions)
    // --- end L1 systems ---
    // --- battle host (BT pass): S10 seams the session fills (all optional; unset = unchanged behaviour) ---
    // BT §1.4 step 2 story block: true while a story / tutorial sequence is active (retail 0x140b154d0 or the list at
    // 0x140b2b360). While true, a galactic service tail with a human attacker or defender in any queued conflict runs no
    // conflict processing (single player). Source (REMAINING_GAPS §6.4): 0x140b154d0 is the queued-battle record flag
    // (GalacticSim::linked_battle_pending, set by LINK_TACTICAL / the corruption mission), the list is never written.
    // Retail holds the record outside the conflict queues until 0x28f590 promotes it (and clears the flag); the port
    // starts a linked battle at once (BT §2.9.3) and holds the record (flag set) only while another conflict is active,
    // so the block is left unset by default: wiring it to linked_battle_pending would stop that active conflict (a human
    // Vote never answered). The flag's other effects are in GalacticSim::paused(). Tests / hosts only.
    std::function<bool()> story_sequence_active;
    // BT §1.6 story lock 0x4472a0 (story rewards DISABLE / ENABLE / RESTRICT_AUTORESOLVE, global flag +0x41 or the planet
    // list +0x308): "this planet's battle must be fought". A human-involved locked conflict is fought (the pending choice
    // answers Fight); without the hook the ConflictConfig answer applies.
    std::function<bool(PlanetIndex planet)> autoresolve_locked;
    // BT §2.1 / TACTICAL_GAPS §4.10 random map picker 0x47bf40: the candidate maps (Data\Art\Maps\*.TED, CustomMaps, mod
    // paths: name, kind, terrain, faction, player count, max base level, flags). Empty = no picker (fallback maps).
    struct MapCandidate {
        std::string name;
        bool space = false;
        int32_t terrain = -1;       // TEMPERATE 0 .. URBAN 6, SPACE 7; -1 = any (retail terrain bit 31)
        int32_t faction = -1;       // random-map faction index; -1 = any
        int32_t players = 2;        // +0xac
        int32_t max_base_level = 5; // +0xb0 [M]
        bool excluded = false;      // +0xb4
    };
    std::vector<MapCandidate> map_candidates;
    // Object removal feed (0x3a8380, STRATEGIC_GAPS §1.1 survival learning): called at each battle end, before the mode
    // pops, once per TacticalBattle::removals() record (deaths in event order, then the survivors). Hosts connect it to
    // GalacticAiWorld::record_object_removal (which filters AI owners / UNIT_AI / TRANSPORT). Unset: nothing.
    std::function<void(int32_t owner, uint32_t object_type, uint32_t original_type, bool survived)> object_removed;
    // --- end battle host ---
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // A1 land campaign-battle credit transfer (tactical_credits.hpp): for a LAND battle each non-human
    // participant's credits are swapped for its Tactical_Budget_Allowance share at the push and the
    // leftovers returned (x Credit_Multiplier) at the pop. HOOK [A2]: the AI player's galactic budget
    // categories (A2's AIBudget storage, written through the span). Unset (no A2 budget yet): no transfer.
    std::function<std::span<ai::tactical::TacticalBudgetCategory>(int32_t player)> tactical_budget;
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    // --- A1 stage 2 (owner: A1 agent): opt-in tactical AI (battle_ai.hpp). Set: every battle runs a BattleAi for
    // its AI participants (the S10 stand-in orders / reinforcement cadence are switched off; sides without a
    // tactical AI keep the stand-in), CRC system "battle.ai" registered. Null (default): unchanged S10 behaviour.
    std::shared_ptr<const TacticalAiSetup> tactical_ai;
#endif
};

class CampaignBattles {
public:
    using MapLoader = std::function<bool(const std::string& name, terrain::MapData& out)>;
    static MapLoader file_map_loader(const FileProvider& files);

    // `rules`, `catalog` must outlive this object.
    CampaignBattles(SimHost& host, const galactic::ConflictRules& rules, const BattleCatalog& catalog, MapLoader loader,
                    CampaignBattleOptions options = {});
    ~CampaignBattles();
    CampaignBattles(const CampaignBattles&) = delete;
    CampaignBattles& operator=(const CampaignBattles&) = delete;

    // Needs a running galactic campaign (SimHost::start_galactic). False otherwise.
    bool attach(std::vector<std::string>* diagnostics = nullptr);
    galactic::ConflictManager& conflicts() { return *cm_; }
    const galactic::ConflictManager& conflicts() const { return *cm_; }
    const TacticalBattle* battle() const { return battle_.get(); }
    TacticalBattle* battle() { return battle_.get(); }
    const std::vector<std::string>& log() const { return log_; }  // one summary per tactical battle / refusal
    // Playtest QA D1 / V1: TacticalBattle::unit_losses() of the last tactical battle that ended (presentation, not saved).
    std::span<const TacticalBattle::RemovalRecord> last_battle_losses() const { return last_losses_; }
    uint32_t battles_fought() const { return fought_; }
    // S10-int: Change_Owner of an object of the running battle (store, trees, combat / abilities, side). Hosts set
    // lua::ObjectBridge::change_owner of their tactical story VMs to this (story_session owner). False when no
    // battle runs or the object is not a battle object.
    bool change_owner(objects::ObjectHandle h, int32_t owner);
    // --- S4 (owner: S4 agent): corruption missions (engine/galactic corruption.hpp; [inferred] rule there) ---
    // `player` corrupts `planet` with corruption `type` (CorruptionTypeEnum 0..7). The planet type's option of that type
    // with a tactical mission queues a conflict at the queue front (land: land_invade with `units`; space:
    // initiate_space_conflict) on the option's map, attacker = `player`, defender = the planet's owner; when that battle
    // starts its tactical story is the option's Tactical_Script_Name and StorySession::set_corruption_mission(player,
    // type) is set (no story session: the corruption is applied here when `player` wins). An option without a mission
    // corrupts the planet at once. Refused: unknown planet / player, the player owns the planet, the planet already
    // has that corruption, no such option, or the conflict could not be queued. Call between ticks (sim thread).
    enum class CorruptionStart : uint8_t { Refused = 0, Applied = 1, MissionQueued = 2 };
    CorruptionStart start_corruption(PlanetIndex planet, int32_t player, int8_t type, std::vector<objects::ObjectHandle> units = {});
    // --- end S4 ---
    // Int-final: the human corruption command (SpecialAbility 13 kind 6, L1_S4_REMAINING §4.5 entry; layout below) is
    // decoded by attach()'s galactic command hook into start_corruption. The defiler must be a live galactic object of the
    // issuing player (step 1; [port] owner test); Refused -> NotAllowed, unknown planet -> InvalidTarget.
    CorruptionStart last_corruption_start() const { return last_corruption_; }
    // --- P4 sim (owner: galactic agent): the tactical retreat dialog's Autoresolve for `player` (BT §4.5,
    // P4_SIM_SYSTEMS §9.3; retail: a direct GUI call, the port keeps it as the host's port command so replays reproduce
    // it). Gate = the retreat coordinator's can-retreat check for the player (TacticalBattle::stop_for_auto_resolve).
    // Stops the battle and, at this host's next update, ends it like a finished battle except that the galaxy resolves
    // it (ConflictManager::resolve_battle, from tactical) instead of finish_battle; the auto-resolve winner feeds the
    // story / corruption battle ends. A retreating player of the result retreats through the galactic write-back [port:
    // the original starts that player's tactical retreat / early withdrawal, §9.3 step 3]. False when refused.
    // The Withdraw of a running galactic auto-resolve (event 49) is conflicts().set_retreating(player) (BT §4.6, §9.2).
    bool auto_resolve_battle(int32_t player);
    // --- end P4 sim ---
    // GALAXY_MAP_PRESENTATION §6 (OI host, the main-menu attract demo): a tactical battle on `r.map` with no conflict and
    // no galactic forces (r.forces empty: every unit comes from the map, owners = player indices). It starts at the next
    // battle hook like a conflict's battle; its end writes nothing back to the galaxy (no ConflictManager::finish_battle).
    // False when tactical battles are off, a battle runs or is pending, or the map does not load as r.info.kind.
    bool start_map_battle(galactic::BattleRequest r);
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    const BattleAi* tactical_ai() const { return ai_.get(); }  // A1: the running battle's AI (null: none)
    const std::vector<std::string>& ai_log() const { return ai_log_; }  // A1: one AI summary per battle
#endif

    // --- N1 save / load (owner: n1-host, stage 2 n2-host): attach() registers two SimHost save participants, removed by
    // the destructor:
    //   "galactic.conflicts"  the ConflictManager (ConflictManager::write / read)
    //   "battles"             this object's between-battle state: battles fought, log, pad persistence records, the
    //                         carried fog flags, corruption missions, LINK_TACTICAL records consumed, tactical AI log;
    //                         v2 adds the battle in flight: u8 0 none / 1 pending (accepted, pushed at the next hook:
    //                         the map is loaded again through the MapLoader on read) / 2 running, + its BattleRequest.
    //                         Its SaveParticipant::rebuild pushes a running battle at load (save_session.hpp "LOADING A
    //                         SAVE TAKEN DURING A BATTLE"): the request staged by the pass-1 read, the map through the
    //                         MapLoader, then exactly the battle start of hook() (credit transfers, TacticalBattle +
    //                         setters + setup, corruption match, story begin - the story re-attaches the saved tactical
    //                         plots without dispatching BattleStarted -, tactical AI begin, battle chunks).
    // SAVE CHUNKS OF A RUNNING BATTLE: registered at the battle start (and by the rebuild), removed at its end, in this
    // order: battle.movement, battle.combat, battle.abilities, battle.garrison, battle.hangar, battle.fog, battle.damage
    // (TacticalBattle::write_part), battle.state (TacticalBattle's own state + this object's battle-level state: credit
    // transfers, station upgrades passed on, the story bridge, cinematic transports, unit motion bits, base ids, Lua fog
    // cells + epoch), battle.ai (BattleAi, while a tactical AI runs).
    // save_blocker(): a battle that has ended or was stopped for auto-resolve and is popped at the next hook.
    std::string save_blocker() const;
    void write_state(ByteWriter& w) const;
    bool read_state(ByteReader& r);
    // --- end N1 ---

private:
    void hook(SimHost& host);
    void push_battle(SimHost& host);  // Start_Game: the pending battle (hook) or the battle a load rebuilds
    // --- N1 stage 2 (owner: n2-host) ---
    std::string rebuild();             // "battles" SaveParticipant::rebuild
    void register_battle_chunks();
    void remove_battle_chunks();
    void write_battle_level(ByteWriter& w) const;
    bool read_battle_level(ByteReader& r);
    std::unique_ptr<galactic::BattleRequest> resume_;  // load pass 1: the running battle to rebuild
    // --- end N1 stage 2 ---
    // --- battle host (BT pass) ---
    class ConflictGate;                       // story-block forwarder in front of the ConflictManager (BT §1.4 step 2)
    std::unique_ptr<ConflictGate> gate_;
    commands::RejectReason resume_game(uint8_t player, const commands::EventData& ev);  // ResumeGame 51 (TACTICAL_GAPS §4.11)
    // --- end battle host ---

    SimHost* host_;
    const galactic::ConflictRules* rules_;
    const BattleCatalog* catalog_;
    MapLoader loader_;
    CampaignBattleOptions opt_;
    std::unique_ptr<galactic::ConflictManager> cm_;
    std::unique_ptr<TacticalBattle> battle_;
    bool pending_ = false;
    bool direct_ = false;  // the running / pending battle came from start_map_battle
    size_t upgrades_seen_ = 0;  // TacticalBattle::station_upgrades() passed to SimHost::station_replaced
    galactic::BattleRequest pending_req_;
    terrain::MapData pending_map_;
    std::vector<std::string> log_;
    std::vector<TacticalBattle::RemovalRecord> last_losses_;
    uint32_t fought_ = 0;
    // REMAINING_GAPS §6.5 [port]: the per-planet pad persistence vectors (planet data D +0x10 has no galactic-side storage
    // yet), planet order of first use; passed to each campaign battle at its planet. Hashed in "battle.state" once used.
    std::vector<std::pair<PlanetIndex, std::vector<TacticalBattle::PadRecord>>> pad_records_;
    // Fog of war (owner: fog agent): the original's global FOW-active flag / render option (static initial 1;
    // land battles set 1, space battles keep the previous value: FOG_OF_WAR §8 item 2).
    bool fog_active_ = true, fog_render_ = true;
    bool ar_requested_ = false;  // P4 sim: auto_resolve_battle() stopped the running battle
    // --- S4 (owner: S4 agent): corruption missions ---
    struct CorruptionMission {
        PlanetIndex planet = kInvalidPlanet;
        int32_t player = -1;
        int8_t type = -1;
        galactic::ConflictKind kind = galactic::ConflictKind::Land;
        std::string plots;
    };
    std::vector<CorruptionMission> corruption_queued_;  // started, battle not pushed yet (queue order)
    CorruptionMission corruption_active_;               // the running battle's mission (player -1: none)
    CorruptionStart last_corruption_ = CorruptionStart::Refused;  // Int-final: result of the last corruption command
    commands::RejectReason corruption_command(uint8_t player, const commands::EventData& ev);  // Int-final
    void corruption_push();                             // at the push: match pending_req_ against corruption_queued_
    void corruption_end(int32_t winner);                // at the battle end, before the pop
    void corruption_prune();                            // no battle running: forget missions whose conflict is gone
    // L1_S4_REMAINING.md §4.4: at the battle-load screen, STORY_LOAD_TACTICAL_MAP to the galactic story (planet, kind),
    // then a SET_TACTICAL_MAP slot of that kind (StorySession::take_tactical_map) replaces the request's map.
    void story_load_map(galactic::BattleRequest& r);
    // --- end S4 ---
#ifdef ALAMO_SIM_HAVE_STORY
    // --- L1/S4 wave 2a story hookup (owner: L1/S4 agent; keep other hooks separate) ---
    // The battle's Lua ObjectBridge for the tactical story VMs (StorySession::begin_battle): abilities = the
    // battle's AbilitySystem, anim = SimAnimation's world of the pushed mode, change_owner = change_owner()
    // above. Story notifications fed after each battle tick (deaths, hardpoints); end_battle before the pop.
    std::unique_ptr<lua::ObjectBridge> story_bridge_;
    void story_begin(SimHost& host, GameMode& mode);
    void story_feed(SimHost& host);
    // --- end L1/S4 wave 2a ---
    // --- L1/S4 wave 2b story hookup (owner: L1/S4 agent) ---
    // Script hooks of the battle (spawn through the tactical mode, cinematic transports serviced each battle tick),
    // the Dummy_Barrage_Target spawner, the feeds STORY_REINFORCE (reserve deployments), STORY_UNIT_ARRIVED /
    // STORY_FULL_STOP (movement transitions [inferred]), STORY_DESTROY_BASE (the starbase / a ground structure died)
    // and story-forced battles from StorySession::linked_tactical() (LINK_TACTICAL -> ConflictManager::start_linked,
    // pushed in the same hook).
    lua::TacticalScriptHooks story_hooks_;
    lua::RemainingScriptHooks story_remaining_;  // L1 bindings: script_remaining.hpp hooks of the tactical story VMs
    lua::CinematicTransports story_transports_;
    std::map<objects::ObjectId, uint8_t> story_motion_;  // per movable unit: bit 0 arrived, bit 1 moving (lookup by id)
    std::vector<objects::ObjectId> story_base_ids_;      // galactic ids of the request's starbase / structures
    size_t story_linked_ = 0;                            // linked_tactical() entries already started
    void story_linked(SimHost& host);
    // --- end L1/S4 wave 2b ---
    // --- fog follow-up (owner: fog follow-up agent) ---
    // story_begin -> story_fog_begin: the battle's FogOfWar for the tactical story VMs (story_hooks_.fog = story_fog_:
    // FogOfWar.* / Undo_Reveal, epoch = battle number), STORY_FOG_POSITION_REVEAL / STORY_FOG_OBJECT_REVEAL polls
    // (StoryBattleStart fog hooks = !fog_point_fogged / !fog_object_hidden, force 0) and the space-station special
    // weapon of Fire_Special_Weapon (story_hooks_.station_readiness / station_fire -> CombatSystem, fog force 1).
    // ENABLE_FOW story rewards (StorySession::set_enable_fow_hook, installed by hook() once a story runs): the running
    // battle's fog, else the carried fog_active_ / fog_render_.
    void story_fog_begin(StoryBattleStart& st);
    void story_fog_reward(bool has_param, int32_t value);
    StorySession* story_fog_session_ = nullptr;  // the session the ENABLE_FOW hook is installed on
#ifdef ALAMO_LUA_HAVE_FOG
    lua::FogScriptHooks story_fog_;
#endif
    // --- end fog follow-up ---
    // --- L1 systems (owner: tactical-systems agent; campaign_battles.cpp "L1 systems" block) ---
    // story_begin -> l1_hooks: the TacticalScriptHooks the battle's L1 systems answer (special weapons, TSW, engines,
    // ground height, teleport, hyperspace out, retreat increment; the script spawn hook adopts special weapons) and
    // the RemainingScriptHooks of the space fields / weather / bombardment systems. l1_clip_frames: Land / Takeoff clip
    // lengths of cinematic transports (CinematicTransports::service).
    void l1_hooks(SimHost& host);
    std::function<uint32_t(objects::ObjectId, uint8_t)> l1_clip_frames();
    void l1_transport_step(const lua::CinematicStep& st);
    // --- end L1 systems ---
#endif
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    struct CreditTransfer {
        int32_t player = -1;
        ai::tactical::TacticalCreditTransfer state;
    };
    std::vector<CreditTransfer> credits_;  // active land-battle transfers (participant order)
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    // --- A1 stage 2 tactical AI hookup (campaign_battles_ai.cpp) ---
    std::unique_ptr<BattleAi> ai_;
    std::vector<std::string> ai_log_;
    void ai_begin(SimHost& host, GameMode& mode);
    void ai_update();
    void ai_end();
    // --- end A1 ---
#endif
};

// --- L1 systems: Space_Retreat_Unit_Increment_Wait_Frames of every faction (FactionFiles.xml list, by GalacticRules
// faction index; unset = 0) for CampaignBattleOptions::retreat_increment_frames. tactical_battle_l1.cpp. ---
std::vector<int32_t> load_retreat_increments(const FileProvider& provider, const galactic::GalacticRules& rules);
std::vector<float> load_retreat_flight_increments(const FileProvider& provider, const galactic::GalacticRules& rules);  // unset = 500 [port]
TacticalBattle::L1Constants load_l1_constants(const data::GameConstants& constants);  // missing tags keep the defaults
std::vector<TacticalBattle::L1Faction> load_l1_factions(const FileProvider& provider, const galactic::GalacticRules& rules);
// Int-final: every tactical-systems loader above in one call (retreat increments, flight increments, L1 factions, L1
// constants, weather tables) into `opt`; what the hosts (alamo_headless / alamo_game) call before CampaignBattles.
// `constants` null = the shipped defaults. Weather: null when WeatherScenarios.xml has no scenario.
void load_tactical_systems(const FileProvider& provider, const galactic::GalacticRules& rules, const data::GameConstants* constants,
                           CampaignBattleOptions& opt);

// Int-final: the corruption command (SpecialAbility, kind 6). [port] data layout (COMMANDS.md gives only "a per-kind
// layout"): u32 defiler galactic object id, u32 planet galactic object id, u8 corruption type (CorruptionTypeEnum
// 0..7), little-endian (9 bytes). read_corruption_command: false for other kinds / sizes.
inline constexpr uint32_t kSpecialAbilityCorrupt = 6;
commands::EventData corruption_command(uint32_t defiler_id, uint32_t planet_id, int8_t type);
bool read_corruption_command(const commands::EventData& ev, uint32_t& defiler_id, uint32_t& planet_id, int8_t& type);

// battle host: the random map picker 0x47bf40 over `candidates` (TACTICAL_GAPS §4.10): kind and terrain equal (candidate
// terrain -1 = any), faction equal or candidate -1, max base level >= base_level, players >= 2, not excluded; matches in
// list order. Retail picks one with int(0, n - 1) on the FREE (QPC-seeded) RNG - not deterministic. [port] the pick is
// index = FNV-1a(space, terrain, faction, base_level, n) mod n: deterministic, no synced draw (the answer is stored on
// the planet by ConflictManager, so parity only needs a valid map). "" when nothing matches.
std::string pick_random_map(const std::vector<CampaignBattleOptions::MapCandidate>& candidates, bool space, int32_t terrain, int32_t faction,
                            int32_t base_level);

struct CampaignAggressor {    uint32_t period = 600;  // galactic frames between moves per player
    void operator()(SimHost& host, uint8_t player, uint64_t frame) const;
};

}  // namespace alamo::sim

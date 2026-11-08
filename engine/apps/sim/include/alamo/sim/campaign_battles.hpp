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
    // (a1j t05: the BT §1.4 step 2 story block is no longer a seam: attach() always installs the gate, which reads
    // GalacticSim::story_block(), A1_READINGS §31.7.)
    // BT §2.1 / TACTICAL_GAPS §4.10 random map picker 0x47bf40: the candidate maps (Data\Art\Maps\*.TED, CustomMaps, mod
    // paths: name, kind, terrain, faction, player count, max base level, flags). Empty = no picker (fallback maps).
    // NOT WIRED yet (cu8 t07, roadmap cu8f): no host builds the list, because which TED fields give the entry's terrain,
    // faction, player count and max base level (0x47b030) is not in docs/re (RE task).
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
    // a1j t07 (A1_READINGS §38): the parent galaxy of the battle's AI scripts (lua::TacticalGalacticContext:
    // Evaluate_In_Galactic_Context 0x69e1b0, Release_Credits_For_Tactical 0x603c70). galactic_resolver is called at each
    // battle start and returns the galaxy's perception resolver: a GalacticAiWorld's resolver(), or a GalacticSimTokenResolver
    // over the galaxy that the host refreshes. Unset or null: no context (both bindings keep their sim-less behaviour).
    // has_galactic_ai: the player has a galactic AI context (0x4b06d0); unset = the galactic player's `ai` flag [port].
    std::function<const ai::TokenResolver*()> galactic_resolver;
    std::function<bool(int32_t player)> has_galactic_ai;
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
    // q6 t19 (OBJECT_SCRIPTS §10.3): Lua Hide(true) on an object of the running battle (the battle's script bridge, shared
    // by the story and the object scripts: ScriptObjectState::hidden). A const read for the presentation side (model,
    // sound, radar, fog ghost, surface FX, mouse pick); no sim system reads it. False without a battle or a bridge.
    bool script_hidden(objects::ObjectId id) const;
    const std::vector<std::string>& log() const { return log_; }  // one summary per tactical battle / refusal
    // Playtest QA D1 / V1: TacticalBattle::unit_losses() of the last tactical battle that ended (presentation, not saved).
    std::span<const TacticalBattle::RemovalRecord> last_battle_losses() const { return last_losses_; }
    uint32_t battles_fought() const { return fought_; }
    // S10-int: Change_Owner of an object of the running battle (store, trees, combat / abilities, side). Hosts set
    // lua::ObjectBridge::change_owner of their tactical story VMs to this (story_session owner). False when no
    // battle runs or the object is not a battle object.
    bool change_owner(objects::ObjectHandle h, int32_t owner);
    // --- S4 (owner: S4 agent): corruption missions (engine/galactic special_data.hpp CorruptionChoiceDef; [inferred]
    // rule there) ---
    // `player` corrupts `planet` with corruption `type` (CorruptionTypeEnum 0..7). The planet type's option of that type
    // with a tactical mission (story t08, L1_S4_REMAINING §4.5) writes the linked-battle record (queue_linked: the option's
    // kind and map, attacker = `player`, defender = the planet owner's faction, plots = Tactical_Script_Name, not sandbox:
    // no galactic unit enters, the map and the plots supply them) and starts it at once (start_linked_record), so no
    // ground unit has to be in orbit; `units`, when given, replace the last-invade vector [port]. When that battle starts
    // its tactical story is the option's Tactical_Script_Name and StorySession::set_corruption_mission(player, type) is set
    // (no story session: the corruption is applied here when `player` wins). An option without a mission corrupts the
    // planet at once. Refused (cost refunded once charged): unknown planet / player, the player owns the planet, the planet
    // already has that corruption, no such option, a linked record already waits, or the start is refused. The record
    // may wait (banner, another active conflict): MissionQueued all the same. Call between ticks (sim thread).
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
    // --- story t07 / t08 (owner: link-tactical): the linked-battle record GMM+0xf0 (BT §2.7, §2.9) ---
    // One record at most, written by a LINK_TACTICAL reward (story_linked: Reward_Param1..14, BT §2.9.1) or a corruption
    // mission (start_corruption), and started by start_linked_record (0x28f590) right after it is written and at every hook
    // without a battle until it starts or is dropped.
    struct LinkedRecord {
        galactic::ConflictKind kind = galactic::ConflictKind::Land;  // P2 (GROUND / a word the keyword map lacks: land)
        PlanetIndex planet = kInvalidPlanet;                          // P1
        int32_t attacker = -1;                                        // P3 (player)
        std::string map;                                              // P4
        std::string defender;                                         // P5 faction name, resolved at the start (-1 unknown)
        uint32_t forced_type = galactic::kNoIndex;                    // P6 galactic type index (sandbox splits)
        std::string plots;                                            // P7 Story_Plots file
        bool sandbox = false;                                         // P8
        int32_t arrival = 1;                                          // P9 arrival options
        bool presentation_a = false, presentation_b = false;          // P10 / P11
        galactic::LinkedResolution resolution = galactic::LinkedResolution::DestroyAi;  // P12
        bool banner = true;                                           // P13: the pending banner first (+0x179)
        bool flagship_split = true;                                   // P14
        bool banner_shown = false;                                    // +0x178
        int8_t corruption_type = -1;                                  // story t08: the corruption mission's type (else -1)
    };
    // P9-P11 as 0x28f590 step 4 applies them (globals, kept until the next linked start; saved). Sim-side state only: the
    // arrival hold is read by the running battle (story t12, TacticalBattle::set_arrival_hold); the presentation overlays by
    // the UI (story t13, BT §2.9.9 item 1).
    struct LinkedPresentation {
        int32_t arrival = 0;              // GMM+0x30 = P9
        bool arrival_cinematic = false;   // 0x140b27d5e (P9 != 0) [M: arrival cinematic on]
        bool hyperspace_hold = false;     // 0x140b27d5f (P9 == 2): reinforcements wait in hyperspace until Resume_Hyperspace_In
        bool presentation_a = false;      // GMM+0x34 = P10 (overlay +0xf, 0x140a28c93 = !P10) [L]
        bool letterbox = false;           // P10 and P9 != 1: 0x140b27ee0 = 0, 0x140b27ee4 = 6, 0x140a28c20 = 1 [L]
        bool presentation_b = false;      // P11: overlay +0xe, cinematic input block 0x140a28c91 [M]
        bool skip_battle_dialog = false;  // 0x140b305ed: set by a record write, taken by the next battle end (0x87770)
        bool last_battle_skipped_dialog = false;  // the last battle end saw the flag: no battle-summary dialog
    };
    enum class LinkedStart : uint8_t { NoRecord = 0, Waiting = 1, Banner = 2, Started = 3, Refused = 4 };
    // 0x28bd70: refused (false, nothing written) when a record is present, the planet / kind / attacker is invalid or the map
    // or defender name is empty. Sets GalacticSim::queue_linked_battle (0x140b30545) and skip_battle_dialog.
    bool queue_linked(LinkedRecord r);
    // 0x28f590 (BT §2.9.3). Waiting: a normal conflict's pending vote is open, or another conflict is active and does not give
    // way (a1j t05 story block rule) [port: the original takes over]. Banner (P13, first pass): with a decide callback that
    // answers Pending (the game's battle dialog) the banner is up, one galactic pause level is raised and the record waits
    // for linked_begin(); otherwise (headless) the banner is answered at once [port]. Started: the record is consumed,
    // GOM+0x61c := P12, P9-P11 applied, ConflictManager::start_linked (land units = the last-invade vector, then cleared).
    // Refused: consumed and dropped (invalid record, unknown defender, ConflictManager refusal; logged).
    LinkedStart start_linked_record();
    // The banner's Begin (0x28c7e0; also ResumeGame 51 while the banner is up): the pause level is released and the record
    // starts at the next hook. False when no banner is up.
    bool linked_begin();
    const LinkedRecord* linked_record() const { return lk_.present ? &lk_.record : nullptr; }
    bool linked_banner_up() const { return lk_.banner_up; }
    const LinkedPresentation& linked_presentation() const { return lk_.presentation; }
    galactic::LinkedResolution linked_resolution() const { return lk_.resolution; }  // GOM+0x61c (the running linked battle)
    // Lua Resume_Hyperspace_In (0x543570): clears the hold (story t12: the tactical story VMs' ScriptGapHooks::
    // resume_hyperspace_in; the running battle reads it through TacticalBattle::set_arrival_hold).
    void resume_hyperspace_in() { lk_.presentation.hyperspace_hold = false; }
    // The global last-invade vector 0x140a172b0: the units of the last human invade (ConflictGate::on_invade) or landing
    // (a human UnitsLanded routed by the story: the last `value` surface units, as StorySession maps it [port]).
    std::span<const objects::ObjectHandle> last_invade() const { return lk_.last_invade; }
    // --- end story t07 / t08 ---
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    const BattleAi* tactical_ai() const { return ai_.get(); }  // A1: the running battle's AI (null: none)
    const std::vector<std::string>& ai_log() const { return ai_log_; }  // A1: one AI summary per battle
#endif

    // --- N1 save / load (owner: n1-host, stage 2 n2-host): attach() registers two SimHost save participants, removed by
    // the destructor:
    //   "galactic.conflicts"  the ConflictManager (ConflictManager::write / read)
    //   "battles"             this object's between-battle state: battles fought, log (v1-3 also held the pad persistence
    //                         records, now in the galactic save; a v1-3 save's records move to the planets on load), the
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
    // a1j t05: a queued conflict that is a corruption mission's battle record (planet, attacker and kind of a mission in
    // corruption_queued_); mission_queued(): any queued conflict is one.
    bool mission_record(const galactic::ConflictInfo& c) const;
    bool mission_queued() const;
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
    // AI_GALACTIC_TOKENS §9.4: the pad persistence records live on the planets (GalacticSim::built_objects, galactic save);
    // each campaign battle gets its planet's list. Hashed in "battle.state" (planets with records only).
    bool rebuilding_ = false;  // rebuild(): the battle a save load pushes (no recreate of persisted objects)
    // Fog of war (owner: fog agent): the original's global FOW-active flag / render option (static initial 1;
    // land battles set 1, space battles keep the previous value: FOG_OF_WAR §8 item 2).
    bool fog_active_ = true, fog_render_ = true;
    bool ar_requested_ = false;  // P4 sim: auto_resolve_battle() stopped the running battle
    // q6 t09: SimHost::scoring_reset_tactical_stats at auto-resolve begin, read off the conflict manager every hook: an
    // auto-resolve seen running (AutoSetup / AutoRunning), or a new auto-resolved log entry not seen running (forced, same
    // pass). Not saved: the log position is taken at the first hook (also after a load).
    size_t ar_log_seen_ = 0;
    bool ar_log_init_ = false, ar_reset_sent_ = false;
    void scoring_auto_resolve();
    // --- S4 (owner: S4 agent): corruption missions ---
    struct CorruptionMission {
        PlanetIndex planet = kInvalidPlanet;
        int32_t player = -1;
        int8_t type = -1;
        galactic::ConflictKind kind = galactic::ConflictKind::Land;
        std::string plots;
    };
    std::vector<CorruptionMission> corruption_queued_;  // started, battle not pushed yet (queue order)
    // --- story t07 / t08 (owner: link-tactical): campaign_battles.cpp "linked-battle record" block ---
    struct LinkedState {  // saved in "battles" (v3) and hashed in "battle.state" once off its defaults
        LinkedRecord record;
        std::string source;  // the record's log prefix ("story: LINK_TACTICAL <P1>" / "corruption: mission")
        bool present = false;
        bool banner_up = false;  // the banner waits for linked_begin() (one galactic pause level held)
        galactic::LinkedResolution resolution = galactic::LinkedResolution::None;  // GOM+0x61c
        LinkedPresentation presentation;
        std::vector<objects::ObjectHandle> last_invade;
        uint64_t triggers_seen = 0;  // story trigger log entries already scanned for UnitsLanded
    };
    LinkedState lk_;
    bool linked_waits_banner(const LinkedRecord& r) const;
    void linked_end_battle();  // battle end: the dialog-skip flag is taken (0x87770)
    void write_linked(ByteWriter& w) const;
    static void read_linked(ByteReader& r, LinkedState& out);
    void hash_linked(StateHasher& h) const;
    // --- end story t07 / t08 ---
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
    size_t story_linked_ = 0;                            // linked_tactical() entries already consumed
    void story_linked(SimHost& host);
    void story_landings(SimHost& host);  // story t07: human UnitsLanded triggers -> last_invade_
    bool parse_link_tactical(const std::vector<std::string>& params, LinkedRecord& out, std::string& why);  // story t07
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
    StorySession* story_fog_session_ = nullptr;  // the session the ENABLE_FOW (and story t09 victory) hooks are installed on
    // story t09: the tactical plot set's ENABLE_VICTORY / VICTORY rewards (StorySession::set_victory_hooks, installed with the
    // fog hook) -> the running battle's TacticalBattle::reward_enable_victory / reward_victory (BT §3.1a item 3). VICTORY
    // resolves Reward_Param1 through the galactic players (unknown: "Unable to find faction %s to force victory" in log()).
    // No battle running: nothing.
    void story_victory_enable(bool has_param, int32_t value);
    void story_victory(const std::string& faction, bool short_silent);
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
    std::function<void()> lua_close_wait_;  // OPT3 t11: the last battle's VM-freeing job (TacticalAiWorld::release_scripts)
    void ai_begin(SimHost& host, GameMode& mode);
    void ai_update();
    void ai_end();
    // --- end A1 ---
    void galactic_context_begin(ModeKind kind);  // a1j t07: CampaignBattleOptions::galactic_resolver -> the AI's world
#endif
};

// --- L1 systems: Space_Retreat_Unit_Increment_Wait_Frames of every faction (FactionFiles.xml list, by GalacticRules
// faction index; unset = 0) for CampaignBattleOptions::retreat_increment_frames. tactical_battle_l1.cpp. cu5 t01: the
// faction loaders read the definitions `rules` holds (FactionRules::xml_fields, list order, a later definition's
// non-empty tag wins), not FactionFiles.xml again. ---
std::vector<int32_t> load_retreat_increments(const galactic::GalacticRules& rules);
std::vector<float> load_retreat_flight_increments(const galactic::GalacticRules& rules);  // unset = 500 [port]
TacticalBattle::L1Constants load_l1_constants(const data::GameConstants& constants);  // missing tags keep the defaults
std::vector<TacticalBattle::L1Faction> load_l1_factions(const galactic::GalacticRules& rules);
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

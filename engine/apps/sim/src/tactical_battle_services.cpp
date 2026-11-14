// SP5 (docs/SIM_PARALLEL_DESIGN.md §5, "Services chain"): TacticalBattle::tick as a services phase graph.
//
// Every step of the tick is one service, declared in tick order with the components it reads and writes
// (tactical::services::ServiceGraph). Services whose sets are disjoint share a phase and may run at the same time
// (RunMode::Parallel); conflicting ones keep the tick order, so the result is the serial tick's. A step whose sets are
// not narrowed yet declares everything (kAll) and acts as a barrier. The profile laps of the old tick (tac.setup ...
// tac.victory) are barriers at the same places, so the per-segment zones keep their meaning.
//
// Checks (rule 3): the debug write check (--svc-check, ServiceRunDefaults::check) digests every checkable component
// around each service and reports one changed outside the service's write set; RunMode::Reverse runs each phase in
// reverse order, so an undeclared read shows as a CRC difference against the Serial run (tests: test_battles_services).
#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "tactical_battle_internal.hpp"

namespace alamo::sim {

namespace {

namespace svc = tactical::services;

// Components of the battle (bits of svc::ComponentSet). Checkable ones have a digest (service_digest).
enum Comp : uint32_t {
    cRng,        // the synced SimRng of the tick
    cStore,      // the mode's ObjectStore (objects, positions, owners, behaviours)       [checked: hash_state]
    cState,      // host state: the State save part (units, containers, timed objects, landings, retreat, victory,
                 //   stats, queues, fog flags / pings, L1 caches, pads, BT records, hangar records)   [checked]
    cMovement,   // Movement part (MovementSystem, hyperspace, engine control)                 [checked]
    cCombat,     // Combat part (CombatSystem, weather)                                        [checked]
    cAbilities,  // Abilities part (AbilitySystem, special weapons, space fields)              [checked]
    cGarrison,   // Garrison part                                                              [checked]
    cHangar,     // Hangar part (hangars, contents, production, engine loops)                  [checked]
    cFog,        // Fog part (FogOfWar)                                                        [checked]
    cDamage,     // Damage part (DamageTrackingStore)                                          [checked]
    cTrees,      // the mode's spatial trees and model cache
    cSignals,    // per-tick outputs read in the same tick (story feeds, events, engine-loop requests; not saved)
    cHooks,      // animation / audio hooks and their requests
    cHost,       // callbacks into the host: credits, order listeners (the AI), Lua / story feeds
    cCount
};
constexpr svc::ComponentSet bit(Comp c) { return svc::component(c); }
constexpr svc::ComponentSet kAll = svc::kEverything;
constexpr svc::ComponentSet kCheckable = bit(cRng) | bit(cStore) | bit(cState) | bit(cMovement) | bit(cCombat) | bit(cAbilities) |
                                         bit(cGarrison) | bit(cHangar) | bit(cFog) | bit(cDamage);

std::vector<std::string> comp_names() {
    return {"rng", "store", "state", "movement", "combat", "abilities", "garrison", "hangar", "fog", "damage",
            "trees", "signals", "hooks", "host"};
}

TacticalBattle::ServiceRunDefaults g_defaults;
std::atomic<uint64_t> g_violations_total{0};

}  // namespace

void TacticalBattle::set_service_run_defaults(const ServiceRunDefaults& d) { g_defaults = d; }
TacticalBattle::ServiceRunDefaults TacticalBattle::service_run_defaults() { return g_defaults; }

void TacticalBattle::set_service_check(bool on) {
    if (!on) {
        svc_check_.reset();
        return;
    }
    if (svc_check_) return;
    svc_check_ = std::make_unique<svc::Checker>();
    svc_check_->checkable = kCheckable;
    svc_check_->digest = [this](uint32_t c) { return service_digest(c); };
    svc_check_->on_violation = [this](const std::string& s, uint32_t c) {
        g_violations_total.fetch_add(1);
        std::fprintf(stderr, "svc-check: frame %u service %s changed '%s' outside its declared writes\n", frame_, s.c_str(),
                     comp_names()[c].c_str());
    };
}

uint64_t TacticalBattle::service_violations() const { return svc_check_ ? svc_check_->violations : 0; }
uint64_t TacticalBattle::service_violations_total() { return g_violations_total.load(); }

uint64_t TacticalBattle::service_digest(uint32_t c) const {
    StateHasher h;
    const auto part = [&](SavePart p) {
        ByteWriter w;
        write_part(p, w);
        h.bytes(w.bytes().data(), w.bytes().size());
    };
    switch (static_cast<Comp>(c)) {
        case cRng:
            if (tick_.rng) {
                h.u32(tick_.rng->state());
                h.u64(tick_.rng->draws());
            }
            break;
        case cStore:
            if (tick_.mode) tick_.mode->primary().hash_state(h);
            break;
        case cState: part(SavePart::State); break;
        case cMovement: part(SavePart::Movement); break;
        case cCombat: part(SavePart::Combat); break;
        case cAbilities: part(SavePart::Abilities); break;
        case cGarrison: part(SavePart::Garrison); break;
        case cHangar: part(SavePart::Hangar); break;
        case cFog: part(SavePart::Fog); break;
        case cDamage: part(SavePart::Damage); break;
        default: break;
    }
    return h.digest();
}

void TacticalBattle::build_service_graph() {
    svc_graph_ = std::make_unique<svc::ServiceGraph>();
    svc_owner_ = this;
    svc::ServiceGraph& g = *svc_graph_;
    g.set_component_names(comp_names());
    // sp0's step zone names (tac.s.*, docs/PERFORMANCE.md "serial path split"): one zone per service, timed by the runner.
    g.set_zone_prefix("tac.s.");
    const auto lap = [&g](const char* zone) { g.barrier(std::string("lap.") + zone, [zone] { profile::lap(zone); }); };

    // ---- setup -----------------------------------------------------------------------------------------------------
    // s78n t12 review: deaths raised between ticks by command-phase ability effects - the vehicle thief's eject (unit
    // ability 46 / 18 off: Take_Damage kind 13 kills the stolen vehicle), a STIM_PACK that kills its user - are still in
    // the combat system's list, which the coming combat tick clears. Process them now, in list order, like the combat
    // tick's own deaths (process_death skips a unit that was processed already: last tick's deaths, script_damage's).
    // [port] before this the host never saw such a death (the object stayed, dead in combat only).
    g.add("late_deaths", kAll, kAll, [this] {
        if (combat_ && !combat_->deaths().empty()) {
            // s78n t19: the hooks get this tick's context first, so what a late death starts (its death clip, a corpse
            // clone) is stamped with the current frame, not with the tick before
            if (hooks_) hooks_->set_tick(tick_.rng, tick_.global_frame);
            const std::vector<tc::DeathEvent> late(combat_->deaths().begin(), combat_->deaths().end());
            for (const tc::DeathEvent& d : late) process_death(*tick_.mode, d, *tick_.rng);
        }
    });
    g.add("clear", 0, bit(cSignals), [this] {
        story_reinforced_.clear();  // L1/S4 wave 2b story hook: this tick's reserve deployments
        ability_finished_.clear();  // S7 t15 A1: this tick's signal 0x29
        landings_unloaded_.clear();  // a1i t04: signal 0x19 records of the last tick
        production_done_.clear();    // q6 t19: signal 0x22 records of the last tick
        story_garrisoned_.clear();  // S4: this tick's garrison entries / structure captures
        story_captured_.clear();
    });
    // S8 t17 (FIGHTER_FLYING §4.10 B): squadron of the shared team target, before this tick's orders
    g.add("share_groups", kAll, kAll, [this] { share_groups_sync(); });
    g.add("weather", kAll, kAll, [this] { l1_weather_tick(*tick_.rng); });  // L1 systems: land weather service
    // s78n t06: land tick step 4, after the weather (HERO_CLASH §2.1)
    g.add("hero_clash", kAll, kAll, [this] { hero_clash_tick(*tick_.mode, *tick_.rng, tick_.global_frame); });
    // fog of war: common-tick step 4, before the object services (owner: fog agent). FogOfWar::decay_step only.
    g.add("fog_decay", 0, bit(cFog), [this] { fog_decay(); });
    // station contents / skirmish tactical production (before the object services)
    g.add("hg_production", kAll, kAll, [this] { hg_production(*tick_.mode, *tick_.rng); });
    lap("tac.setup");

    // ---- orders: 1. reinforcements, 2. stand-in orders, 3. retreat coordinator -------------------------------------
    g.add("reinforce", kAll, kAll, [this] { reinforce(*tick_.mode, *tick_.rng, true); });
    g.add("landing", kAll, kAll, [this] { landing_service(*tick_.mode, *tick_.rng); });  // a1i t04: landing objects
    g.add("issue_orders", kAll, kAll, [this] {
        if (cfg_.order_interval > 0 && (frame_ - 1) % cfg_.order_interval == 0) issue_orders(*tick_.mode, *tick_.rng);
    });
    g.add("retreat", kAll, kAll, [this] { retreat_policy(); });
    g.add("retreat_end", kAll, kAll, [this] {
        if (retreat_side_ >= 0 && !retreat_complete_ && frame_ >= retreat_done_frame_) {
            bt_retreat_attrition(static_cast<uint8_t>(retreat_side_), *tick_.rng);  // battle host: vf12 at the countdown end (§4.9)
            complete_retreat(*tick_.mode, static_cast<uint8_t>(retreat_side_));
        }
    });
    lap("tac.orders");

    // ---- pre-move: attack blocks, 4. speed factors, movement inputs --------------------------------------------------
    // s78n t20 stage A: serial, ascending mover, before the collision feed and the movement tick
    g.add("attack_block", kAll, kAll, [this] { attack_block_service(*tick_.mode, *tick_.rng); });
    g.add("speed_factors", kAll, kAll, [this] { apply_speed_factors(); });
    // [port] fleet groups: slowest-member cap and straggler wait (no-op while no group exists)
    g.add("fleet", kAll, kAll, [this] { fleet_tick(*tick_.mode); });
    // s78n t20: targets / ranges / hero clashes for the land collision decisions
    g.add("collision_feed", kAll, kAll, [this] { feed_collision_info(tick_.mode->primary()); });
    // S7 fighters: new squadrons (map start, reinforcements, hangar launches, wingmen) and params
    g.add("fighter_sync", kAll, kAll, [this] { fighter_sync(); });
    g.add("attack_targets", kAll, kAll, [this] { feed_attack_targets(tick_.mode->primary()); });
    // SPACE_AVOIDANCE: committed member transforms for the fighter avoidance hook
    g.add("avoid_snapshot", kAll, kAll, [this] {
        if (space_) avoid_snapshot(tick_.mode->primary());
    });
    lap("tac.pre_move");

    // ---- movement --------------------------------------------------------------------------------------------------
    g.add("move_tick", kAll, kAll, [this] { movement_->tick(frame_, *tick_.rng, *tick_.pf); });
    g.add("move_commit", kAll, kAll, [this] { movement_->commit(tick_.mode->primary()); });
    g.add("pushed_clones", kAll, kAll, [this] { kill_pushed_clones(*tick_.mode); });  // s78n t20 batch 1
    // s78l t02: the arrival turn's clip plays (synced variant draws), ascending id
    g.add("turn_clips", kAll, kAll, [this] { play_turn_clip_events(*tick_.rng, tick_.global_frame); });
    g.add("s4_exit_arrivals", kAll, kAll, [this] { s4_exit_arrivals(); });  // s78n t26.1: garrison hosts' +0xf8 hook
    // S8 t11: deploy clip ends (Is_Active, deploy flags, signal 0x29) before the combat tick
    g.add("deploy_sync", kAll, kAll, [this] { oi_deploy_sync(); });
    lap("tac.movement");

    // ---- motion post ------------------------------------------------------------------------------------------------
    // S7 t22 / t25: enter_idle drops the squadrons' team targets (§4.7 step 4, §4.10 D)
    g.add("fighter_idle", kAll, kAll, [this] { fighter_enter_idle(); });
    // S7 fighters: Provoke / Retarget -> combat attack orders (before the combat tick)
    g.add("fighter_events", kAll, kAll, [this] { forward_fighter_events(); });
    g.add("despawn_crush", kAll, kAll, [this] {
        for (const objects::ObjectId id : movement_->despawned())  // §4.6: land bombers at their path end, no death effects
            if (const Unit* u = unit_by_id(id)) bt_kill_silently(*tick_.mode, u->h);
        oi_crush_.assign(movement_->crush_events().begin(), movement_->crush_events().end());  // OI tactical
    });
    g.add("l1_motion", kAll, kAll, [this] { l1_motion(*tick_.mode, *tick_.rng); });  // L1: hyperspace jumps, engine re-enables
    // cu8l t03: the start fleet's hyperspace arrivals (timer draws, finishes)
    g.add("arrivals", kAll, kAll, [this] { arrival_service(*tick_.mode, *tick_.rng); });
    // cu8 t02: Disable_Force_Abilities_Range emitters (Force-disable stamps)
    g.add("force_disable", kAll, kAll, [this] { l1_force_disable(*tick_.mode); });
    // engine loops: SimpleSpace locomotor services (moving first, then idle)
    g.add("engine_loops", kAll, kAll, [this] { hg_engine_service(); });
    // battle host: space squadrons / non-steering land squads at their members' box centre
    g.add("team_positions", kAll, kAll, [this] { bt_team_positions(*tick_.mode); });
    lap("tac.motion_post");

    // ---- trees ------------------------------------------------------------------------------------------------------
    g.add("trees", kAll, kAll, [this] {
        TacticalMode& mode = *tick_.mode;
        objects::ObjectStore& store = mode.primary();
        mode.resolve_models();  // refixf t07: after a load, before the boxes and the parallel sweeps read the model cache
        // SP5 (read old, write new, commit in index order): the boxes are computed per unit in parallel (const reads; the
        // model-cache entries bounds_of fills lazily are filled first, serially: prepare_bounds), the trees updated in unit
        // order.
        const auto n = static_cast<uint32_t>(units_.size());
        const auto want = [&](const Unit& u) { return u.alive && !u.retreated && u.movable && store.alive(u.h); };
        for (const Unit& u : units_)
            if (want(u)) mode.prepare_bounds(u.h);
        std::vector<tactical::Aabb> box(n);
        std::vector<uint8_t> has(n, 0);
        const auto fill = [&](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i)
                if (want(units_[i])) {
                    box[i] = mode.bounds_of(units_[i].h);
                    has[i] = 1;
                }
        };
        if (n >= svc_parallel_min_) (*tick_.pf)(n, kSvcParallelGrain, fill);
        else fill(0, n);
        for (uint32_t i = 0; i < n; ++i)
            if (has[i]) mode.trees().update(static_cast<tactical::EntityHandle>(units_[i].id), box[i]);
        mode.trees().maintain(tick_.sim_time, *tick_.pf);
    });
    lap("tac.trees");
    g.add("anim", kAll, kAll, [this] { drive_animation(tick_.mode->primary(), *tick_.rng, tick_.global_frame); });
    lap("tac.anim");

    // ---- 5. combat (abilities run inside its serial phases), events -------------------------------------------------
    g.add("combat_flags", kAll, kAll, [this] { combat_sync(tick_.mode->primary()); });
    g.add("s4_positions", kAll, kAll, [this] { s4_positions(*tick_.mode); });  // S4: garrisoned units sit on their holders
    // s78n t03: the land teams' TeamTargeting P_U (cleared after the combat tick)
    g.add("combat_team_pos", kAll, kAll, [this] { combat_team_positions(tick_.mode->primary()); });
    lap("tac.combat_pre");
    g.add("abilities_combat", kAll, kAll, [this] {
        hooks_->set_tick(tick_.rng, tick_.global_frame);
        abilities_->tick(frame_, tick_.mode->trees(), *tick_.rng, *tick_.pf);
        combat_->clear_team_positions();  // s78n t03: readers between ticks use the head's own position (save / load equal)
    });
    lap("tac.combat");
    g.add("consume_events", kAll, kAll, [this] { consume_events(*tick_.mode, *tick_.rng); });
    g.add("respawn", kAll, kAll, [this] { oi_respawn_due(*tick_.mode); });  // OI abilities: Tactical_Respawn due
    lap("tac.events");

    // ---- services ---------------------------------------------------------------------------------------------------
    g.add("hangars", kAll, kAll, [this] { hg_tick(*tick_.mode, *tick_.rng); });  // hangars: SPAWN_SQUADRON services, launches
    // cu8k t01: Saber_Throw throws / SWORD_CONTROL / SWORD_CATCH (after the saber's step)
    g.add("saber_throw", kAll, kAll, [this] { saber_throw_service(*tick_.mode); });
    // cu8g t01: HOLSTER_WEAPON (engine-loop suppression, draw / holster SFX requests)
    g.add("holster", kAll, kAll, [this] { hg_holster_service(); });
    g.add("l1_weapons", kAll, kAll, [this] { l1_weapons(*tick_.mode, *tick_.rng); });  // L1: special-weapon volleys, TSW
    lap("tac.services");

    // ---- cleanup ----------------------------------------------------------------------------------------------------
    g.add("expire", kAll, kAll, [this] { expire_objects(*tick_.mode); });
    g.add("garrisons", kAll, kAll, [this] { s4_tick(*tick_.mode, *tick_.rng); });  // S4: garrison enters / exits / ejects, capture
    g.add("pads", kAll, kAll, [this] { pads_tick(*tick_.mode); });  // Int-final: build pads
    // skai: skirmish map battles: the Income_Stream credit trickle (credits through the host's callback)
    g.add("income", bit(cStore) | bit(cState), bit(cHost), [this] { sk_income(*tick_.mode); });
    // battle host: base-shield secondary occupants follow the live shields (land): the passability grid, bt_shields_
    g.add("shield_stamps", bit(cStore), bit(cMovement) | bit(cState), [this] { bt_shield_stamps(*tick_.mode); });
    // DAMAGE_TRACKING service step: the store's decay and each unit's next service frame
    g.add("damage_tracking", bit(cState), bit(cDamage) | bit(cState), [this] { service_damage_tracking(); });
    lap("tac.cleanup");

    // ---- fog --------------------------------------------------------------------------------------------------------
    // fog of war: REVEAL sources, attacker flashes (owner: fog agent). Reads positions / owners / behaviours (store),
    // units, shots and reveal modifiers (combat), weather (state: L1 weather), garrison state; writes the fog and its
    // ping list / weather-dirty flag (state).
    g.add("fog", bit(cStore) | bit(cState) | bit(cCombat) | bit(cAbilities) | bit(cGarrison) | bit(cTrees),
          bit(cFog) | bit(cState), [this] { fog_service(*tick_.mode); });
    g.add("fog_setup", kAll, kAll, [this] { fog_setup_tick(); });  // FOWF t05: setup phase end (all humans ready / countdown)
    lap("tac.fog");

    // ---- queues, victory --------------------------------------------------------------------------------------------
    // q6: attacks on TEAM containers (re-pick, end), before the queues read the orders
    g.add("team_attack", kAll, kAll, [this] { team_attack_service(*tick_.mode, *tick_.rng); });
    // [port: Gabriel] 5b. queueable unit actions: completed orders start the next entry
    g.add("queues", kAll, kAll, [this] { advance_queues(*tick_.mode, *tick_.rng); });
    g.add("swap_back", kAll, kAll, [this] { late_swap_back_signals(); });  // S7 t27: jetpack swap back, signal 0x29
    g.add("stats", bit(cCombat), bit(cState), [this] {
        stats_.shots = combat_->stats().shots;
        stats_.hits = combat_->stats().hits;
    });
    // cu8j t01 (MOVEMENT §4.6.2): the movement coordinator update (retail common tick step 10, after the object services):
    // this tick's new coordinators compose their formations (the FormationClass ctor draws, the land stagger draws).
    // Orders given after it (the AI and story services of the host) compose at the next tick's.
    g.add("coordinators", kAll, kAll, [this] { movement_->coordinator_update(frame_, tick_.global_frame, *tick_.rng); });
    // 6. victory. story t06: the victory monitor (losses since the last tick, countdown, end)
    g.add("victory", kAll, kAll, [this] { check_victory(*tick_.mode); });
    g.add("end_clones", kAll, kAll, [this] {
        if (finished_ && !death_clones_.empty()) remove_death_clones(*tick_.mode, true);  // refixf t04: the clones end with it
    });
    g.add("space_obstacles", kAll, kAll, [this] { space_obstacles_sync(*tick_.mode); });  // refixf t02
    g.add("land_obstacles", kAll, kAll, [this] { land_obstacles_sync(*tick_.mode, *tick_.pf); });  // S8 t02
    g.add("engine_requests", kAll, kAll, [this] { hg_publish_engine_requests(); });  // engine loops: requests for audio
    // s78n t18 review: the gates the command phase reads follow this tick's deaths / retreats
    g.add("pair_gates", kAll, kAll, [this] { feed_pair_gates(); });
    lap("tac.victory");
    g.build();
}

void TacticalBattle::tick(TacticalMode& mode, SimRng& rng, const ParallelFor& pf, double sim_time, uint64_t global_frame) {
    if (finished_) {  // finished outside a tick (auto-resolve stop): the clones go before the mode pops (refixf t04)
        if (!death_clones_.empty()) remove_death_clones(mode, true);
        return;
    }
    ++frame_;
    oi_global_frame_ = global_frame;
    stats_.frames = frame_;
    tick_ = TickContext{&mode, &rng, &pf, sim_time, global_frame};
    if (!svc_graph_ || svc_owner_ != this) {
        build_service_graph();
        if (service_run_defaults().check) set_service_check(true);
    }
    svc_graph_->run(pf, svc_mode_, svc_check_.get());
    tick_ = TickContext{};
}

}  // namespace alamo::sim

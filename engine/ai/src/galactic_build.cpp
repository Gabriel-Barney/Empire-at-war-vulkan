// Galactic production (docs/re/AI_GALACTIC_2.md §2.3): Produce_Force creates one AIBuildTask per selection
// without a free unit; galactic tasks go on the global pending list and the per-planet AIPlanetBuildQueue market
// (every > 30 frames per planet queue: re-check, claim one, bid, start the least-slack task) produces them.
// Produced objects are claimed on the producing planet; failures dispatch Production_Failed; there is no
// time-out. [port] / [M] mark the stand-ins (listed in docs/PHASE4.md A2 open items).
#include "galactic_ai_impl.hpp"

namespace alamo::ai {

namespace {
constexpr float kLevelFactorK = 1.0f;  // 0x140b16c8c = GameConstants +0x148C, ctor 1.0 (AI_GALACTIC_3 §8) [H]
constexpr int64_t kMarketPeriod = kAiFps;  // strict >: more than FPS frames since the last run
constexpr int64_t kPollFrames = 3;          // PRODUCTION_QUEUED polls (galactic)
}  // namespace

float GalacticAiWorld::Impl::funds_time(const BuildTask& b) const {
    // 0x6490d0: the goal budget's time until it can afford cost(T) (0x613720).
    const PlayerAi* pa = player_ai(b.player);
    if (!pa) return 0.0f;
    return pa->ctx.budget.time_to_afford(b.category, b.goal_key, cost_none(b.type));
}

void GalacticAiWorld::Impl::remove_pending(int32_t id) {
    pending_builds.erase(std::remove(pending_builds.begin(), pending_builds.end(), id), pending_builds.end());
    const auto it = builds.find(id);
    if (it != builds.end()) it->second.listed = false;
}

void GalacticAiWorld::Impl::create_build_tasks(TaskForceRec& tf) {
    PlanInstance* pi = plan_of_script(tf.plan);
    if (!pi) return;
    const PlanDefinition& def = library.plans[pi->plan];
    PlayerAi* pa = player_ai(tf.player);
    for (size_t i = 0; i < tf.wanted.size(); ++i) {
        const uint32_t type = tf.wanted[i];
        const gal::TypeInfo* ti = type_info(type);
        BuildTask b;
        b.id = next_build++;
        b.plan = tf.plan;
        b.tf = tf.id;
        b.player = tf.player;
        b.type = type;
        b.created = now;
        b.next_poll = now;
        b.goal_key = pi->goal_key;
        b.category = pi->category;
        b.queue = ti && ti->build_cost > 0 && sim.queue_kind(type) == gal::kGroundQueue ? 0 : 1;  // A2 galactic: queue_kind
        const PlanetIndex stage = tf.stage >= 0 ? static_cast<PlanetIndex>(tf.stage) : kInvalidPlanet;
        const PlanetIndex source = i < tf.wanted_source.size() ? tf.wanted_source[i] : kInvalidPlanet;
        b.dest = valid_planet(stage) ? stage : source;
        auto charge = [&] {
            if (!pa || b.charged) return;
            b.charge = cost_none(type);
            pa->ctx.budget.spend(b.category, b.goal_key, b.charge);  // Allocate 0x691950 -> 0x6110e0
            b.charged = true;
        };
        builds.emplace(b.id, b);
        BuildTask& t = builds[b.id];
        tf.builds.push_back(t.id);
        if (def.magic_plan) {
            // BUILD_TASK_LIST -> HIJACK_UNIT 0x648370 (AI_GALACTIC_3 §4.2-4.3) [H]. No charge: the Allocate needs the
            // AIPlanetBuildTask pointer, which magic tasks never get [M]. GalacticSim::magic_spawn (A2 galactic hook):
            // steal (MagicPlanStealing or a named hero), cancel pending respawns, spawn at the destination as a
            // finished queue item; null = FAILED.
            const PlanetIndex at = valid_planet(t.dest) ? t.dest : (pi->target >= 0 ? static_cast<PlanetIndex>(pi->target) : kInvalidPlanet);
            const bool hero = type < tinfo.size() && tinfo[type].hero;
            const ObjectHandle o = valid_planet(at) ? sim.magic_spawn(tf.player, at, type, def.magic_plan_stealing, hero) : ObjectHandle{};
            if (!o) {
                fail_build(t, "magic spawn refused");
                continue;
            }
            decide(tf.player, "magic spawn " + type_name(type) + " at " + planet_name(at));
            if (PlayerAi* owner = player_ai(tf.player)) owner->known.insert(store.id(o));  // not an On_Unit_Added: the task takes it
            // Step 5: a non-structure created on the surface is launched (op 4) and formed (op 0), FAILED when blockaded.
            if (ti && !structure_kind(ti->kind) && on_surface(o)) {
                if (sim.ai_blockaded(at)) {  // 0x3fa370 (A2GAL_READINGS 4)
                    fail_build(t, "blockaded");
                    continue;
                }
                launch_and_form(tf.player, at, o, unit_jumps(o) ? kAloneFleet : gal::kNoFleet);
            }
            tf.units.push_back(o);  // galactic: 0x6498f0 hands the unit to the task -> FINISHED
            tf.had_units = true;
            finish_build(t);
            continue;
        }
        if (ti && structure_kind(ti->kind)) {
            // structure / generic: charge, bind the build planet = the goal target (0x6956f0) -> PRODUCTION_QUEUED.
            charge();
            t.bound = true;
            t.dest = pi->target >= 0 ? static_cast<PlanetIndex>(pi->target) : source;
            t.bid_planet = t.dest;
        } else if (def.allow_free_store_units) {
            // The free store can supply the unit (0x4d2bd0) -> no charge [M: the unit joins the task force now].
            const ObjectHandle take = free_store_supply(tf.player, type);
            if (take) {
                off_free_store(take);
                tf.units.push_back(take);
                tf.had_units = true;
                finish_build(t);
                continue;
            }
            charge();
        } else {
            charge();
        }
        pending_builds.push_back(t.id);  // the AIPlanetBuildTask on the global pending list 0x140b39330
        t.listed = true;
        decide(tf.player, "build task " + std::to_string(t.id) + " " + type_name(type) + " for " + tf.name +
                              (t.bound ? " at " + planet_name(t.dest) : std::string()));
    }
    tf.wanted.clear();
    tf.wanted_source.clear();
}

// 0x4d2bd0 (a1h t07): the first free-store unit of `type` at a planet. It does not read the entry's +0x30 owner
// (A1_READINGS §28.5), so a unit another PotentialPlan reserved can be taken; that plan's validity test (0x6c45e0)
// then fails at the next Commit.
ObjectHandle GalacticAiWorld::Impl::free_store_supply(int32_t player, uint32_t type) const {
    for (const ObjectHandle u : free_units(player))
        if (store.type(u) == type && planet_of_unit(u) != kInvalidPlanet) return u;
    return {};
}

void GalacticAiWorld::Impl::finish_build(BuildTask& b) {
    b.state = BuildState::Finished;
    remove_pending(b.id);
    for (auto& pq : planet_queues)
        for (PlanetBuildQueue& q : pq) {
            q.tasks.erase(std::remove(q.tasks.begin(), q.tasks.end(), b.id), q.tasks.end());
            if (q.current == b.id) {
                q.current = -1;
                q.current_queue_id = -1;
                q.current_type = gal::kNoIndex;
            }
        }
}

// PRODUCTION_FAILED (7): Production_Failed(tf, type) (0x5fdd90), out of the bookkeeping (0x4d4ee0), finish.
void GalacticAiWorld::Impl::fail_build(BuildTask& b, const char* why) {
    finish_build(b);
    b.state = BuildState::Failed;
    decide(b.player, "build task " + std::to_string(b.id) + " " + type_name(b.type) + " failed (" + why + ")");
    const auto tit = tfs.find(b.tf);
    if (tit == tfs.end()) return;
    lua::AiEventArg a;
    a.kind = lua::AiEventArg::String;
    a.text = type_name(b.type);
    const TaskForceRec copy = tit->second;
    tf_event(copy, PlanEvent::ProductionFailed, {a});
}

void GalacticAiWorld::Impl::end_plan_builds(int32_t plan) {
    for (auto it = builds.begin(); it != builds.end();) {
        BuildTask& b = it->second;
        if (b.plan != plan) {
            ++it;
            continue;
        }
        remove_pending(b.id);
        for (auto& pq : planet_queues)
            for (PlanetBuildQueue& q : pq) {
                q.tasks.erase(std::remove(q.tasks.begin(), q.tasks.end(), b.id), q.tasks.end());
                // A production in progress keeps the queue busy; its object goes to the free store (signals 3 / 4
                // with the task gone).
                if (q.current == b.id) q.current = -1;
            }
        it = builds.erase(it);
    }
}

// PAYG reclaim r = 0x5f7d40(plan, amount) (AI_GALACTIC_3 §8) [H mechanism; M meanings]: the plan's build tasks whose
// production has started with a remaining cost in (0, amount], oldest first (port: task id = creation order; the
// original's wall-clock comparator is a determinism hazard), ties by AI_Combat_Power descending; taken greedily while
// the cost fits the remaining amount; each taken task is flagged (+0xD9). [port] Remaining cost = the queue entry's
// cost x remaining / build time (the installments still to pay).
float GalacticAiWorld::Impl::payg_reclaim(int32_t plan, float amount) {
    struct Cand {
        int32_t id;
        float cost, power;
    };
    std::vector<Cand> cands;
    for (const auto& [id, b] : builds) {
        if (b.plan != plan || b.state != BuildState::Producing || b.queue_id < 0 || !valid_planet(b.producing_at)) continue;
        float rem = 0.0f;
        for (const gal::BuildEntry& e : sim.queue_of(b.producing_at, b.queue))
            if (e.type == b.type && e.player == b.player) {
                rem = e.build_time > 0.0f ? e.cost * (e.remaining / e.build_time) : 0.0f;
                break;
            }
        if (rem > 0.0f && rem <= amount) cands.push_back({id, rem, power_of(b.type)});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.power > y.power; });
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.id < y.id; });
    float r = 0.0f, left = amount;
    for (const Cand& c : cands) {
        if (c.cost > left) continue;
        left -= c.cost;
        r += c.cost;
        builds[c.id].reclaimed = true;
    }
    return r;
}

void GalacticAiWorld::Impl::planet_lost(PlanetIndex p) {
    // Signal 0xE: every task bound to the planet's queues fails (producing: bid < 0) or returns to the market.
    for (PlanetBuildQueue& q : planet_queues[p]) {
        const std::vector<int32_t> ids = q.tasks;
        q.tasks.clear();
        q.current = -1;
        q.current_queue_id = -1;
        q.current_type = gal::kNoIndex;
        for (const int32_t id : ids) {
            const auto it = builds.find(id);
            if (it == builds.end()) continue;
            BuildTask& b = it->second;
            if (b.state == BuildState::Producing) {
                fail_build(b, "planet lost");
            } else if (b.state == BuildState::Queued) {
                b.bid = kPpInfinite;
                b.bid_planet = b.bound ? b.dest : kInvalidPlanet;
                if (!b.listed) {
                    pending_builds.push_back(id);
                    b.listed = true;
                }
            }
        }
    }
}

// The market of one planet's queues (AIPlanetBuildQueue update 0x53cd60), queue 0 then queue 1, from
// ProductionBehavior::Service (AI_GALACTIC_3 §5) [H]: GalacticSim's production-service hook calls it for each planet in
// planet service order (newest registered first) right before that planet's own queues advance.
// Market path test (0x6955a0 / 0x695c90): planet `from` -> b.dest with the delivery class (REMAINING_GAPS §6.2); floods
// cached per frame and (owner, source, mode). No destination (or from == dest) passes.
int GalacticAiWorld::Impl::delivery_mode(const BuildTask& b) const {
    return galactic_ai_delivery_mode(b.type < tinfo.size() && tinfo[b.type].stealth_company, false);  // +0xDA: [port] unset
}

bool GalacticAiWorld::Impl::delivery_reachable(int32_t owner, const BuildTask& b, PlanetIndex from) {
    if (b.dest == kInvalidPlanet || b.dest == from) return true;
    if (delivery_frame != now) {
        delivery_frame = now;
        delivery_reach.clear();
    }
    const int mode = delivery_mode(b);
    const auto key = std::make_tuple(owner, from, mode);
    auto it = delivery_reach.find(key);
    if (it == delivery_reach.end()) {
        const gal::Bitset danger = ai_dangerous(owner);
        it = delivery_reach.emplace(key, galactic_ai_delivery_flood(sim, owner, from, mode, &danger)).first;
    }
    return b.dest < it->second.size() && it->second[b.dest] != 0;
}

void GalacticAiWorld::Impl::market_planet(PlanetIndex p) {
    if (sim.frame() != market_sim_frame) {
        // [port] The galactic tick (T5) runs before this frame's T7 AI update, which sets `now` to the host frame:
        // the market of this tick already counts as that frame.
        market_sim_frame = sim.frame();
        ++now;
    }
    const auto planets = sim.planets();
    if (planet_queues.size() != planets.size()) planet_queues.resize(planets.size());
    const auto players = sim.players();
    const auto& rules = sim.rules();
    {
        const size_t pi = p;
        if (pi >= planets.size()) return;
        const gal::PlanetState& ps = planets[pi];
        if (!ps.is_planet) return;
        const int32_t owner = ps.owner;
        // Owner change of a planet with AI production (signal 0xE).
        for (PlanetBuildQueue& q : planet_queues[pi]) {
            bool lost = false;
            for (const int32_t id : q.tasks) {
                const auto it = builds.find(id);
                if (it != builds.end() && it->second.player != owner) lost = true;
            }
            if (lost) planet_lost(p);
        }
        if (owner < 0 || static_cast<size_t>(owner) >= players.size()) return;
        const uint32_t f = players[static_cast<size_t>(owner)].faction;
        if (f < rules.factions.size() && rules.factions[f].is_neutral) return;
        for (uint32_t qi = 0; qi < 2; ++qi) {
            PlanetBuildQueue& Q = planet_queues[pi][qi];
            if (now - Q.last_run <= kMarketPeriod) continue;
            Q.last_run = now;
            // A finished or dropped production whose task is gone frees the queue.
            if (Q.current < 0 && Q.current_queue_id >= 0) {
                bool there = false;
                for (const gal::BuildEntry& e : sim.queue_of(p, qi)) there = there || (e.type == Q.current_type && e.player == owner);
                if (!there) {
                    Q.current_queue_id = -1;
                    Q.current_type = gal::kNoIndex;
                }
            }
            // 1. the current item's remaining time
            const auto sim_queue = sim.queue_of(p, qi);  // A2 galactic: GalacticSim::queue_of
            float queue_time = sim_queue.empty() ? 0.0f : sim_queue[0].remaining;
            // 2. re-check own tasks
            for (const int32_t id : std::vector<int32_t>(Q.tasks)) {
                const auto it = builds.find(id);
                if (it == builds.end()) {
                    Q.tasks.erase(std::remove(Q.tasks.begin(), Q.tasks.end(), id), Q.tasks.end());
                    continue;
                }
                BuildTask& b = it->second;
                if (b.state == BuildState::Producing) continue;  // bid < 0: kept
                if (b.state != BuildState::Queued) {
                    Q.tasks.erase(std::remove(Q.tasks.begin(), Q.tasks.end(), id), Q.tasks.end());
                    continue;
                }
                const float ft = funds_time(b);
                const bool reachable = delivery_reachable(owner, b, p);  // 0x6955a0
                if (reachable && ft >= queue_time) {
                    queue_time += std::min(ft, btime_at(owner, b.type, p));  // PAYG player
                    if (!can_build_at(owner, b.type, p)) fail_build(b, "not buildable here");
                    continue;
                }
                b.bid = kPpInfinite;  // 0x695a30: planet = null, bid = 1e18, back to the pending list
                b.bid_planet = b.bound ? b.dest : kInvalidPlanet;
                Q.tasks.erase(std::remove(Q.tasks.begin(), Q.tasks.end(), id), Q.tasks.end());
                if (!b.listed) {
                    pending_builds.push_back(id);
                    b.listed = true;
                }
            }
            // 3. claim one pending task that chose this planet (at most one per run)
            for (const int32_t id : pending_builds) {
                BuildTask& b = builds[id];
                if (b.bid_planet != p || b.queue != qi || b.player != owner || b.state != BuildState::Queued) continue;
                Q.tasks.push_back(id);
                queue_time += btime_at(owner, b.type, p);
                remove_pending(id);
                break;
            }
            // 4. bid on every pending task of this owner and queue index
            for (const int32_t id : pending_builds) {
                BuildTask& b = builds[id];
                if (b.queue != qi || b.player != owner || b.bound || b.state != BuildState::Queued) continue;
                if (!can_build_at(owner, b.type, p) || sim.production_disabled(owner, b.type)) continue;  // [port] OI2 ai
                const gal::TypeInfo* ti = type_info(b.type);
                if (!ti) continue;
                if (ground_kind(ti->kind) && static_cast<int32_t>(ps.surface.size()) >= rules.constants.max_ground_forces_on_planet)
                    continue;  // a company with no free ground slot
                if (!delivery_reachable(owner, b, p)) continue;  // the path with mode 2 / 3 / 5 fails (0x695c90)
                if (funds_time(b) < queue_time) continue;
                double travel = 0.0;
                const gal::TypeInfo* tt = type_info(travel_type_of(b.type));  // a company: its transport (SG 1.7)
                const float hs = tt ? tt->hyperspace_speed : ti->hyperspace_speed;
                if (b.dest != kInvalidPlanet && b.dest != p && hs > 0.0f) {
                    const uint32_t fi = players[static_cast<size_t>(owner)].faction;
                    // x the fleet speed multiplier M (STRATEGIC_GAPS §2.1), as the fleet's departure speed.
                    const float fs = (fi < rules.factions.size() ? rules.factions[fi].hyperspace_speed_factor : 1.0f) *
                                     sim.fleet_speed_multiplier();
                    const Vec3 a = planets[b.dest].position, c = ps.position;
                    const double dx = static_cast<double>(a.x) - c.x, dy = static_cast<double>(a.y) - c.y, dz = static_cast<double>(a.z) - c.z;
                    travel = std::sqrt(dx * dx + dy * dy + dz * dz) / (static_cast<double>(hs) * static_cast<double>(fs) * kAiFps);
                }
                const int32_t base = ti->required_ground_base == 0 ? ps.starbase_level : sim.ground_base_level(p);
                const int32_t req = ti->required_ground_base == 0 ? ti->required_star_base : ti->required_ground_base;
                const double level = 1.0 + static_cast<double>(base - req) * static_cast<double>(kLevelFactorK);
                const double ratio = cost_none(b.type) > 0.0f ? static_cast<double>(cost_at(owner, b.type, p)) / static_cast<double>(cost_none(b.type)) : 1.0;
                const double bid = (static_cast<double>(queue_time) + static_cast<double>(btime_at(owner, b.type, p)) + travel) * level * ratio;
                if (bid < b.bid || b.bid_planet == p) {  // ties: the first bidder keeps it
                    b.bid = bid;
                    b.bid_planet = p;
                }
            }
            // 5. start production: idle queue holding tasks -> the least slack (strict <, first wins)
            if (Q.current >= 0 || Q.current_queue_id >= 0 || Q.tasks.empty()) continue;
            BuildTask* pick = nullptr;
            double best = 0.0;
            for (const int32_t id : Q.tasks) {
                BuildTask& b = builds[id];
                if (b.state != BuildState::Queued) continue;
                const double slack = static_cast<double>(funds_time(b)) - static_cast<double>(now - b.created) / kAiFps -
                                     static_cast<double>(btime_at(owner, b.type, p));
                if (!pick || slack < best) {
                    pick = &b;
                    best = slack;
                }
            }
            if (!pick) continue;
            BuildTask& b = *pick;
            b.queue_id = next_queue_id++;
            // Clear the matching reservation slot (type and goal).
            const PlanInstance* plan = plan_of_script(b.plan);
            auto& slots = Q.reservations;
            for (auto r = slots.begin(); r != slots.end(); ++r)
                if (r->type == b.type && plan && r->pp == plan->pp) {
                    slots.erase(r);
                    break;
                }
            int32_t before = 0;
            for (const gal::BuildEntry& e : sim.queue_of(p, qi)) before += e.type == b.type && e.player == owner ? 1 : 0;
            cmd::EventData ev(cmd::EventType::Production);
            ev.set("type_index", b.type);
            ev.set("location", static_cast<uint32_t>(store.id(ps.object)));
            if (order(owner, ev, "build " + type_name(b.type) + " at " + planet_name(p)) != cmd::RejectReason::None) {
                fail_build(b, "production refused");
                continue;
            }
            b.state = BuildState::Producing;
            b.producing_at = p;
            b.bid = -1.0;
            b.queue_entries_before = before;
            Q.current = b.id;
            Q.current_queue_id = b.queue_id;
            Q.current_type = b.type;
        }
    }
}

// An AI player's pay-as-you-go installment a for the head of queue q at planet p (0x53b900, AI_GALACTIC_2 §3.4; q2f
// t01) [H]: while the planet's AIPlanetBuildQueue has a current task (+0x18), the installment is paid from that task's
// goal budget (may pause); otherwise from the refund accumulator (always paid). [port] A current task that left the map
// or whose goal struct is gone counts as none. A player without a PlayerAi pays as a human (credits >= a).
float GalacticAiWorld::Impl::installment(PlanetIndex p, uint32_t q, const gal::BuildEntry& head, float a) {
    PlayerAi* pa = player_ai(head.player);
    if (!pa) return sim.players()[static_cast<size_t>(head.player)].account.credits >= a ? a : -1.0f;
    const int32_t current = p < planet_queues.size() && q <= 1 ? planet_queues[p][q].current : -1;
    const auto it = current >= 0 ? builds.find(current) : builds.end();
    if (it == builds.end()) return pa->ctx.budget.installment(-1, 0, a);
    return pa->ctx.budget.installment(it->second.category, it->second.goal_key, a);
}

// PRODUCTION_QUEUED polls (every 3 frames, galactic) and the producing tasks' queue items.
void GalacticAiWorld::Impl::service_build_tasks(int32_t player) {
    std::vector<int32_t> ids;
    for (const auto& [id, b] : builds)
        if (b.player == player && (b.state == BuildState::Queued || b.state == BuildState::Producing)) ids.push_back(id);
    for (const int32_t id : ids) {
        const auto it = builds.find(id);
        if (it == builds.end()) continue;
        BuildTask& b = it->second;
        if (b.state == BuildState::Queued) {
            if (now < b.next_poll) continue;
            b.next_poll = now + kPollFrames;
            // 0x2ac320 = the galactic population legality (AI_GALACTIC_3 §3.5) [H]: pop(T, 0) > max(capacity - used, 0)
            // fails the task; it applies to an AI only with AI_Uses_Galactic_Pop_Cap (never in shipped data), so a
            // task the market cannot place (e.g. a hero without a galactic cost) waits, like the original (no time-out).
            if (sim.population_capped(player) && pop_of(b.type) > static_cast<double>(sim.population_room(player))) {
                fail_build(b, "population cap");
                continue;
            }
            // [port] A type the story production-disabled after the plan chose it (DISABLE_BUILDABLE, 0x282510) fails
            // the task instead of being queued (OI2 ai; the original's poll reads only 0x2ac320).
            if (sim.production_disabled(player, b.type)) {
                fail_build(b, "production disabled");
                continue;
            }
            if (b.bound && valid_planet(b.dest)) {  // the target's structure slots are full
                const gal::TypeInfo* ti = type_info(b.type);
                const gal::PlanetState& ps = sim.planets()[b.dest];
                const auto& msb = sim.map().planets.max_space_base;
                if (ti && ti->kind == gal::UnitKind::GroundStructure) {
                    // 0x3f8af0 vs 0x372740 (Special_Structures_Land, not <Max_Ground_Base>; galactic_slots.hpp) [H].
                    const int32_t slots = ground_slot_max(b.dest);
                    if (slots >= 0 && ground_slots_used(b.dest) >= slots) fail_build(b, "structure slots full");
                } else if (ti && ti->kind == gal::UnitKind::StarBase) {
                    const int32_t slots = b.dest < msb.size() && msb[b.dest] >= 0 ? msb[b.dest] : 5;
                    if (ps.starbase_level >= slots) fail_build(b, "starbase maxed");
                }
            }
            continue;
        }
        // Producing: the queue item is gone without a claimed object -> a tech upgrade finished, else it was
        // dropped (starbase without base, enemy fleet, re-evaluation) -> failed [port: signal 3/4 vs cancel].
        if (!valid_planet(b.producing_at)) continue;
        int32_t count = 0;
        for (const gal::BuildEntry& e : sim.queue_of(b.producing_at, b.queue)) count += e.type == b.type && e.player == player ? 1 : 0;
        if (count > b.queue_entries_before) continue;
        const gal::TypeInfo* ti = type_info(b.type);
        if (ti && ti->advances_tech != 0) {
            if (PlayerAi* pa = player_ai(player)) pa->ctx.budget.paid(b.category, b.goal_key, b.charge);
            finish_build(b);
        } else {
            fail_build(b, "production dropped");
        }
    }
    // Finished / failed tasks leave the map once their task force no longer lists them as pending.
    for (auto it = builds.begin(); it != builds.end();) {
        const BuildTask& b = it->second;
        if ((b.state == BuildState::Finished || b.state == BuildState::Failed) && b.player == player) {
            const auto tit = tfs.find(b.tf);
            if (tit != tfs.end()) {
                auto& v = tit->second.builds;
                v.erase(std::remove(v.begin(), v.end(), b.id), v.end());
            }
            it = builds.erase(it);
        } else {
            ++it;
        }
    }
}

// 0x525200 (AI_GALACTIC_2 §3.4 "Goal production time"; q2f t01) [H]: over the player's build tasks (id order = the
// execution system's list order) that sit on a planet build queue and belong to the running plan's goal (or to no task
// force), the largest queue wait (0x53bb20) + build time at that planet. Tasks on the global market without a bid add
// nothing. [M] a task "on a planet build queue" (+0xc8) = producing at P, else bidding for / claimed by P (bid_planet).
// 0 when no task matches (the production is over or the plan has none); the goal is always activated here (the
// maintenance test only runs for goals with a running plan).
float GalacticAiWorld::Impl::goal_production_time(int32_t running) const {
    const auto pit = plans.find(running);
    if (running < 0 || pit == plans.end()) return 0.0f;
    const int32_t player = pit->second.player;
    const PlayerAi* pa = player_ai(player);
    if (!pa) return 0.0f;
    const auto goal_of = [pa](int32_t plan) -> const Goal* {
        for (const auto& cat : pa->ctx.goals.active())
            for (const Goal& g : cat)
                if (g.running == plan) return &g;
        return nullptr;
    };
    // slack(T) at P = goal ETA - (NOW - goal created) / FPS - btime(T, P); no goal: 1e18.
    const auto slack = [&](const BuildTask& t, PlanetIndex p) -> float {
        const Goal* g = t.tf >= 0 ? goal_of(t.plan) : nullptr;
        if (!g) return 1e18f;
        return (g->eta - static_cast<float>(now - g->created) / static_cast<float>(kAiFps)) - btime_at(t.player, t.type, p);
    };
    const bool payg = sim.pays_as_you_go(player);
    float t = 0.0f;
    for (const auto& entry : builds) {
        const BuildTask& x = entry.second;
        if (x.player != player || (x.state != BuildState::Queued && x.state != BuildState::Producing)) continue;
        if (x.tf >= 0 && x.plan != running) continue;
        const PlanetIndex p = x.state == BuildState::Producing ? x.producing_at : x.bid_planet;
        if (!valid_planet(p) || x.queue > 1 || static_cast<size_t>(p) >= planet_queues.size()) continue;
        // 0x53bb20(Q, 0, X): the current item's remaining time, then Q's listed tasks with no more slack than X.
        const auto sim_queue = sim.queue_of(p, x.queue);
        float wait = sim_queue.empty() ? 0.0f : sim_queue[0].remaining;
        const PlanetBuildQueue& Q = planet_queues[p][x.queue];
        if (Q.current != x.id && x.tf >= 0) {  // [port] a task without a task force is never ranked (no goal to read)
            const float sx = slack(x, p);
            for (const int32_t id : Q.tasks) {
                const auto yt = builds.find(id);
                if (yt == builds.end()) continue;
                const BuildTask& y = yt->second;
                if (slack(y, p) > sx) continue;
                const float bt = btime_at(y.player, y.type, p), ft = funds_time(y);
                wait += payg ? std::max(bt, ft) : bt + ft;
            }
        }
        t = std::max(t, wait + btime_at(x.player, x.type, p));
    }
    return t;
}

}  // namespace alamo::ai

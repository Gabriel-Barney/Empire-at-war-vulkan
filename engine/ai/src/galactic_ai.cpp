// Galactic AI world: contexts, goal-system hooks, plans on the Lua bridge, task forces, blocking statuses,
// free stores (see galactic_ai.hpp). PotentialPlan: galactic_pp.cpp; build tasks + market: galactic_build.cpp.
// [port] / [L] marks behaviour the RE docs do not fix.
#include "alamo/ai/galactic_ai.hpp"

#include <charconv>
#include <span>

#include "alamo/ai/ai_free_store_path.hpp"
#include "alamo/core/profile.hpp"

#include "alamo/data/xml.hpp"
#include "alamo/lua/gc_schedule.hpp"  // OPT2 t04
#include "galactic_ai_impl.hpp"

namespace alamo::ai {

namespace {
// Apply_Markup target ids handed to the Lua side: (player + 1) << 20 | planet (planet-only ids come from
// AITargetLocationWrapper arguments).
constexpr int32_t kMarkupPlayerShift = 20;
constexpr int32_t kMarkupPlanetMask = (1 << kMarkupPlayerShift) - 1;
}  // namespace

// ================================================================ Impl basics

GalacticAiWorld::Impl::Impl(gal::GalacticSim& s, objects::ObjectStore& st, const objects::GameObjectTypes* ty,
                            const FileProvider* fp, GameRandom& r, const AiDefinitions& d, const EquationSet& e,
                            std::vector<GalacticAiTypeInfo> ti, AiOrderSink snk, GalacticAiConfig c)
    : sim(s), store(st), types(ty), files(fp), rng(r), defs(d), eqs(e), tinfo(std::move(ti)), sink(std::move(snk)),
      cfg(std::move(c)), res(s, tinfo, nullptr, {}) {
    for (GalacticAiTypeInfo& t : tinfo)
        if (t.category_mask == 0) {
            uint64_t m = 0;
            for (const std::string& n : t.categories) m |= ai_category_bit(n);
            t.category_mask = m;
        }
    type_mask.assign(sim.rules().types.size(), 0u);
    for (size_t t = 0; t < type_mask.size() && t < tinfo.size(); ++t) type_mask[t] = tinfo[t].category_mask;
    planet_queues.resize(sim.planets().size());
    res.set_hooks(make_hooks());
}

GalacticSimTokenResolver::Hooks GalacticAiWorld::Impl::make_hooks() {
    GalacticSimTokenResolver::Hooks h;
    h.story_campaign = cfg.story_campaign;
    h.active_goals = [this](int32_t p) { return active_goals(p); };
    h.markup = [this](int32_t p, int32_t planet) {
        const auto it = markup.find({p, planet});
        return it == markup.end() ? 0.0 : it->second;
    };
    h.ai_credits = [this](int32_t p, double& out) {
        const PlayerAi* pa = player_ai(p);
        if (!pa || !pa->ctx.budget.initialised()) return false;
        out = pa->ctx.budget.pools_total();
        return true;
    };
    // BudgetFractionToBuild 0x667120 fails only without a goal system (AI_GALACTIC_TOKENS) [H]. The AIBudgetClass is
    // embedded in the goal system (+0x190), so it exists before its Init 0x6137b0; Init evaluates the category
    // weights first (AI_GALACTIC_2 §3.2), when every rate / available still holds its constructed 0 [M: ctor zeros]
    // (A2 f t01: failing here made the Empire's Infrastructure / MajorItem equations fail at Init, so Offensive got the
    // whole starting bank).
    h.budget = [this](int32_t p, std::string_view category, double& rate, double& available) {
        const PlayerAi* pa = player_ai(p);
        if (!pa) return false;
        if (category.empty()) {  // every category: budget +0x68 and the player's credits [H]
            rate = pa->ctx.budget.income_per_second();
            available = sim.credits(p);
            return true;
        }
        const int32_t c = defs.find_category(category);
        if (c < 0) return false;
        rate = pa->ctx.budget.rate(c);
        available = pa->ctx.budget.available(c);
        return true;
    };
    h.difficulty = [this](int32_t p) {
        const PlayerAi* pa = player_ai(p);
        return pa ? pa->difficulty : -1;
    };
    h.script = [this](const EvalContext& c, std::string_view name, std::span<const Param> params, double& out) {
        return script_evaluate(c, name, params, out);
    };
    return h;
}

int32_t GalacticAiWorld::Impl::active_goals(int32_t p) const {
    int32_t n = 0;
    if (const PlayerAi* pa = player_ai(p))
        for (const auto& cat : pa->ctx.goals.active()) n += static_cast<int32_t>(cat.size());
    return n;
}

uint32_t GalacticAiWorld::Impl::equation(const std::string& name) {
    const std::string key = ai_upper_copy(ai_trim(name));
    const auto it = eq_index.find(key);
    if (it != eq_index.end()) return it->second;
    const uint32_t i = eqs.find(key);
    eq_index.emplace(key, i);
    return i;
}

bool GalacticAiWorld::Impl::evaluate(int32_t self, int32_t opponent, const std::string& fn, int32_t target, double& out) {
    const uint32_t e = equation(fn);
    if (e == kUnresolved) return false;
    EvalContext c;
    c.self = self;
    c.target = target;
    c.location = target;
    c.enemy = opponent;     // bound only when the opponent index != -1 [H]
    c.human = first_human();  // -1 with no human: Human-rooted paths fail (AI_GALACTIC_2 Â§4.3) [H]
    c.rng = &rng.synced();
    const EvalResult r = ai::evaluate(eqs, e, c, res);
    ++eval_count;
    if (target >= 0 || r.ok) ++stats[std::string("~ eval ") + (r.ok ? (r.value > 0.0 ? "pos " : "nonpos ") : "FAIL ") + fn];
    else ++stats["~ eval FAIL " + fn];
    if (!r.ok) return false;
    out = r.value;
    return true;
}

// Script_<File>.Evaluate (AI_GALACTIC_2 Â§4.4) [H]: Data/Scripts/Evaluators/<File>.lua loaded once as ONE shared
// instance; no cache; PlayerObject = Self, Target = the context target; Evaluate(strings..., number); then
// Evaluator_Clean_Up(). Only a Lua number succeeds. Script globals persist across calls and players.
bool GalacticAiWorld::Impl::script_evaluate(const EvalContext& c, std::string_view name, std::span<const Param> params,
                                            double& out) {
    if (!files || !runtime) return false;
    lua::LuaVm* vm = scripts.vm(*runtime, *files, name, errors);
    if (!vm) return false;
    vm->set_time(now_seconds(sim));
    vm->set_frame(static_cast<uint32_t>(sim.frame()));
    lua::set_player_global(*vm, "PlayerObject", c.self);
    if (c.target >= 0 && valid_planet(static_cast<PlanetIndex>(c.target)))
        vm->set_object_global("Target", sim.planets()[static_cast<size_t>(c.target)].object);
    return ScriptEvaluators::evaluate(*vm, name, params, out, errors);
}

std::vector<ObjectHandle> GalacticAiWorld::Impl::player_units(int32_t p) const {
    std::vector<ObjectHandle> out;
    for (const gal::FleetState& f : sim.fleets())
        if (f.alive && f.owner == p)
            for (const ObjectHandle u : f.contents)
                if (store.alive(u)) out.push_back(u);
    for (const gal::PlanetState& ps : sim.planets())
        for (const ObjectHandle u : ps.surface)
            if (store.alive(u) && store.owner(u) == p) out.push_back(u);
    return out;
}

uint32_t GalacticAiWorld::Impl::fleet_of_unit(ObjectHandle u) const { return sim.fleet_containing(u); }

PlanetIndex GalacticAiWorld::Impl::planet_of_unit(ObjectHandle u) const {
    const uint32_t f = fleet_of_unit(u);
    if (f != gal::kNoFleet) return sim.fleets()[f].moving ? kInvalidPlanet : sim.fleets()[f].planet;
    const auto planets = sim.planets();
    for (size_t i = 0; i < planets.size(); ++i)
        if (std::find(planets[i].surface.begin(), planets[i].surface.end(), u) != planets[i].surface.end())
            return static_cast<PlanetIndex>(i);
    return kInvalidPlanet;
}

bool GalacticAiWorld::Impl::tf_owns(ObjectHandle u) const {
    for (const auto& [id, t] : tfs)
        if (t.alive && std::find(t.units.begin(), t.units.end(), u) != t.units.end()) return true;
    return false;
}

std::vector<ObjectHandle> GalacticAiWorld::Impl::free_units(int32_t p) const {
    std::vector<ObjectHandle> out;
    for (const ObjectHandle u : player_units(p))
        if (!tf_owns(u)) out.push_back(u);
    return out;
}

PlanetIndex GalacticAiWorld::Impl::planet_arg(const lua::AiArg& a) const {
    if (a.target >= 0) return static_cast<PlanetIndex>(a.target);
    if (a.object) {
        const PlanetIndex p = sim.planet_of(a.object);
        if (p != kInvalidPlanet) return p;
        return planet_of_unit(a.object);
    }
    if (a.task_force >= 0) {
        const auto it = tfs.find(a.task_force);
        if (it != tfs.end())
            for (const ObjectHandle u : it->second.units) {
                const PlanetIndex p = planet_of_unit(u);
                if (p != kInvalidPlanet) return p;
            }
    }
    return kInvalidPlanet;
}

cmd::RejectReason GalacticAiWorld::Impl::order(int32_t player, const cmd::EventData& ev, const std::string& what) {
    return note_order(player, sink ? sink(static_cast<uint8_t>(player), ev) : cmd::RejectReason::Unhandled, what);
}

// The bookkeeping of order() for an AI command the AI executes as a direct call instead of an event.
cmd::RejectReason GalacticAiWorld::Impl::note_order(int32_t player, cmd::RejectReason r, const std::string& what) {
    ++orders;
    if (r != cmd::RejectReason::None) ++rejected;
    decide(player, "order " + what + (r == cmd::RejectReason::None ? "" : " rejected " + std::string(cmd::reject_reason_name(r))));
    return r;
}

// FleetManagement (event id 8, A2 galactic hook; AI_GALACTIC_2 Â§5.4) executed directly like every AI order.
cmd::RejectReason GalacticAiWorld::Impl::fleet_op(int32_t player, gal::FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit,
                                                  const std::string& what) {
    return order(player, gal::fleet_management_event(op, target, fleet, unit), what);
}

// Launch (op 4) a surface unit into the orbit fleet (found or made), then form (op 0) it into `fleet` when that is
// another fleet [H: op order]. kAloneFleet = a fleet of its own; kNoFleet = it stays in the orbit fleet [port: the
// sim has no company transports, so a launched ground company jumps only with a ship of the orbit fleet].
bool GalacticAiWorld::Impl::launch_and_form(int32_t player, PlanetIndex p, ObjectHandle unit, uint32_t fleet) {
    if (!valid_planet(p) || !store.alive(unit)) return false;
    const uint32_t uid = static_cast<uint32_t>(store.id(unit));
    const std::string un = std::to_string(uid);
    if (fleet_op(player, gal::kFleetLaunch, cmd::kNoObject, cmd::kNoObject, uid, "launch " + un + " at " + planet_name(p)) !=
        cmd::RejectReason::None)
        return false;
    if (fleet == gal::kNoFleet) return true;
    const uint32_t now_in = fleet_of_unit(unit);
    if (fleet == now_in) return true;
    uint32_t fid = cmd::kNoObject;
    if (fleet != kAloneFleet) {
        if (fleet >= sim.fleets().size() || !sim.fleets()[fleet].alive) return true;
        fid = static_cast<uint32_t>(store.id(sim.fleets()[fleet].object));
    }
    return fleet_op(player, gal::kFleetForm, static_cast<uint32_t>(store.id(sim.planets()[p].object)), fid, uid,
                    "form " + un + (fid == cmd::kNoObject ? std::string(" alone") : " into fleet " + std::to_string(fid))) ==
           cmd::RejectReason::None;
}

bool GalacticAiWorld::Impl::move_fleet(int32_t player, uint32_t fleet, PlanetIndex to) {
    if (fleet == gal::kNoFleet || !valid_planet(to)) return false;
    cmd::EventData ev(cmd::EventType::MoveObjectToObject);
    ev.set("object", static_cast<uint32_t>(store.id(sim.fleets()[fleet].object)));
    ev.set("target", static_cast<uint32_t>(store.id(sim.planets()[to].object)));
    return order(player, ev, "move fleet " + std::to_string(store.id(sim.fleets()[fleet].object)) + " -> " + planet_name(to)) ==
           cmd::RejectReason::None;
}

std::vector<int32_t> GalacticAiWorld::Impl::hops_to(PlanetIndex to) const {
    const gal::PlanetGraph& g = sim.graph();
    std::vector<int32_t> d(sim.planets().size(), -1);
    if (to >= d.size() || !g.included(to)) return d;
    std::vector<PlanetIndex> q{to};
    d[to] = 0;
    for (size_t i = 0; i < q.size(); ++i)
        for (uint32_t e = g.edge_begin(q[i]); e < g.edge_end(q[i]); ++e) {
            const PlanetIndex n = g.neighbour(e);
            if (n < d.size() && d[n] < 0) {
                d[n] = d[q[i]] + 1;
                q.push_back(n);
            }
        }
    return d;
}

PlanetIndex GalacticAiWorld::Impl::nearest_owned(int32_t player, int32_t target) const {
    const auto planets = sim.planets();
    if (target < 0) {
        for (size_t i = 0; i < planets.size(); ++i)
            if (planets[i].is_planet && planets[i].owner == player && planets[i].starbase_level > 0) return static_cast<PlanetIndex>(i);
        for (size_t i = 0; i < planets.size(); ++i)
            if (planets[i].is_planet && planets[i].owner == player) return static_cast<PlanetIndex>(i);
        return kInvalidPlanet;
    }
    const std::vector<int32_t> d = hops_to(static_cast<PlanetIndex>(target));
    PlanetIndex best = kInvalidPlanet;
    for (size_t i = 0; i < planets.size(); ++i) {
        if (!planets[i].is_planet || planets[i].owner != player || d[i] < 0) continue;
        if (best == kInvalidPlanet || d[i] < d[best]) best = static_cast<PlanetIndex>(i);
    }
    return best;
}

uint32_t GalacticAiWorld::Impl::next_starbase_type(int32_t player, PlanetIndex p) const {
    const auto& all = sim.rules().types;
    for (size_t t = 0; t < all.size(); ++t)
        if (all[t].kind == gal::UnitKind::StarBase && can_build_at(player, static_cast<uint32_t>(t), p))
            return static_cast<uint32_t>(t);
    return gal::kNoIndex;
}

int32_t GalacticAiWorld::Impl::script_player(int32_t script) const {
    if (script >= kFreeStoreScriptBase) return script - kFreeStoreScriptBase;
    const auto it = plans.find(script);
    return it == plans.end() ? -1 : it->second.player;
}

// Planet targets (PlanetAITargetLocation, slot 4) [L, AI_GALACTIC open item 5]: Friendly = owned by the player
// or an ally; Enemy = owned by a hostile non-neutral player; Neutral = unowned or neutral-owned; Contested = in
// the conflict list. Global never applies to a planet.
bool GalacticAiWorld::Impl::applies(int32_t self, int32_t target, uint32_t flags) const {
    if (target < 0 || !valid_planet(static_cast<PlanetIndex>(target))) return false;
    const gal::PlanetState& ps = sim.planets()[static_cast<size_t>(target)];
    const auto players = sim.players();
    const auto& rules = sim.rules();
    const bool owned = ps.owner >= 0 && static_cast<size_t>(ps.owner) < players.size();
    const bool neutral_owner = !owned || (players[static_cast<size_t>(ps.owner)].faction < rules.factions.size() &&
                                          rules.factions[players[static_cast<size_t>(ps.owner)].faction].is_neutral);
    if ((flags & app::Friendly) && owned && (ps.owner == self || !sim.is_hostile(self, ps.owner)) && !neutral_owner) return true;
    if ((flags & app::Enemy) && owned && !neutral_owner && sim.is_hostile(self, ps.owner)) return true;
    if ((flags & app::Neutral) && neutral_owner) return true;
    if ((flags & app::Contested) && ps.in_battle) return true;
    return false;
}

// ================================================================ AiScriptHost: task forces

int32_t GalacticAiWorld::Impl::tf_force_count(int32_t tf) {
    const auto it = tfs.find(tf);
    if (it == tfs.end() || !it->second.alive) return 0;
    prune_tf(it->second);
    return static_cast<int32_t>(it->second.units.size());
}

void GalacticAiWorld::Impl::tf_set_flag(int32_t tf, lua::AiTfFlag flag, bool value) {
    const auto it = tfs.find(tf);
    if (it == tfs.end()) return;
    const auto pit = plans.find(it->second.plan);
    if (pit == plans.end()) return;
    PlanInstance& pi = pit->second;
    switch (flag) {
        case lua::AiTfFlag::PlanResult: pi.result = value; break;
        case lua::AiTfFlag::Removable: pi.non_removable = !value; break;
        case lua::AiTfFlag::TestTargetContrast: pi.test_contrast = value; break;  // writes PP+0x4D (Â§5.1)
        case lua::AiTfFlag::BlockGoalProposal:  // +0x2a0 list of the goal system (Â§5.1) [H]
            if (PlayerAi* pa = player_ai(pi.player)) {
                if (value) {
                    pa->ctx.goals.block_proposals(pi.goal_key);
                    pi.blocking = true;
                } else {
                    pa->ctx.goals.unblock_proposals(pi.goal_key);
                    pi.blocking = false;
                }
            }
            break;
    }
}

int32_t GalacticAiWorld::Impl::tf_produce_force(int32_t tf, const lua::AiArg& stage, bool) {
    const auto it = tfs.find(tf);
    if (it == tfs.end() || !it->second.alive) return -1;
    TaskForceRec& t = it->second;
    const PlanetIndex sp = planet_arg(stage);
    if (sp != kInvalidPlanet) t.stage = static_cast<int32_t>(sp);
    create_build_tasks(t);
    return new_status(t.plan, tf, StatusKind::ProduceForce);
}

bool GalacticAiWorld::Impl::tf_all_on_free_store(int32_t tf) {
    // Are_All_Units_On_Free_Store (0x5fa090) = not 0x6bffe0(PP): every selection of the TF had a free unit.
    const auto it = tfs.find(tf);
    return it != tfs.end() && it->second.wanted.empty() && it->second.builds.empty();
}

void GalacticAiWorld::Impl::tf_release_forces(int32_t tf, double fraction) {
    const auto it = tfs.find(tf);
    if (it == tfs.end()) return;
    TaskForceRec& t = it->second;
    const auto n = std::min(static_cast<size_t>(std::floor(fraction * static_cast<double>(t.units.size()) + 1e-9)), t.units.size());
    for (size_t i = 0; i < n; ++i) pp_unit_left(t.plan, t.units[i]);  // 0x5ff610 -> 0x6c27d0
    t.units.erase(t.units.begin(), t.units.begin() + static_cast<std::ptrdiff_t>(n));
}

void GalacticAiWorld::Impl::tf_release_unit(int32_t tf, ObjectHandle unit) {
    const auto it = tfs.find(tf);
    if (it == tfs.end()) return;
    auto& u = it->second.units;
    if (std::find(u.begin(), u.end(), unit) == u.end()) return;
    pp_unit_left(it->second.plan, unit);  // 0x5ff610 -> 0x6c27d0
    u.erase(std::remove(u.begin(), u.end(), unit), u.end());
}

std::vector<ObjectHandle> GalacticAiWorld::Impl::tf_units(int32_t tf) {
    const auto it = tfs.find(tf);
    if (it == tfs.end()) return {};
    prune_tf(it->second);
    return it->second.units;
}

std::string GalacticAiWorld::Impl::tf_goal_type_name(int32_t tf) {
    const auto pit = plans.find(plan_of_tf(tf));
    if (pit == plans.end() || pit->second.goal_type >= defs.goals.size()) return {};
    return defs.goals[pit->second.goal_type].name;
}

// [port] every free unit, the category mask ignored: the galactic task force's slot 47 is a stub in the original (0x13b7d0
// returns 0, so 0x5fc040 collects nothing there; A1_READINGS §39.3), not changed here.
bool GalacticAiWorld::Impl::tf_collect_free_units(int32_t tf, uint64_t /*category_mask*/) {
    const auto it = tfs.find(tf);
    if (it == tfs.end()) return false;
    for (const ObjectHandle u : free_units(it->second.player)) {
        off_free_store(u);
        it->second.units.push_back(u);
    }
    if (!it->second.units.empty()) it->second.had_units = true;
    return true;
}

double GalacticAiWorld::Impl::target_distance(int32_t target, const lua::AiArg& other) {
    if (!target_valid(target)) return -1.0;
    const PlanetIndex o = planet_arg(other);
    if (!valid_planet(o)) return -1.0;
    const Vec3 a = sim.planets()[static_cast<size_t>(target)].position, b = sim.planets()[o].position;
    const double dx = static_cast<double>(a.x) - b.x, dy = static_cast<double>(a.y) - b.y, dz = static_cast<double>(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int32_t GalacticAiWorld::Impl::free_store_count(int32_t script, int which) {
    const int32_t p = script - kFreeStoreScriptBase;
    int32_t n = 0;
    for (const ObjectHandle u : free_units(p)) {
        const gal::TypeInfo* t = type_info(store.type(u));
        const bool space = t && space_kind(t->kind);
        if (which < 0 || (which == 1) == space) ++n;
    }
    return n;
}

bool GalacticAiWorld::Impl::unit_safe(ObjectHandle unit) {
    // Only ground units in orbiting fleets can be unsafe [H]: unsafe when a hostile fleet shares the orbit [L].
    const gal::TypeInfo* t = type_info(store.type(unit));
    if (!t || !ground_kind(t->kind)) return true;
    const uint32_t f = fleet_of_unit(unit);
    if (f == gal::kNoFleet || sim.fleets()[f].moving) return true;
    const PlanetIndex p = sim.fleets()[f].planet;
    if (!valid_planet(p)) return true;
    for (const uint32_t o : sim.planets()[p].orbit)
        if (o < sim.fleets().size() && sim.fleets()[o].alive && sim.is_hostile(store.owner(unit), sim.fleets()[o].owner)) return false;
    return true;
}

// ================================================================ Apply_Markup host

int32_t GalacticAiWorld::Impl::markup_target_of_object(int32_t player, ObjectHandle object) {
    const PlanetIndex p = sim.planet_of(object);  // planets: the planet's AI target (data +0x354)
    if (p == kInvalidPlanet || player < 0) return -1;  // other objects: by name in 0x140b310a8 [port: none in galactic]
    return ((player + 1) << kMarkupPlayerShift) | static_cast<int32_t>(p);
}

int32_t GalacticAiWorld::Impl::markup_target_of_player(int32_t, int32_t) {
    return -1;  // galactic target lists hold planets only [port]
}

void GalacticAiWorld::Impl::add_markup(int32_t target, double delta) {
    if (target < 0) return;
    int32_t player = (target >> kMarkupPlayerShift) - 1;
    const int32_t planet = target & kMarkupPlanetMask;
    if (player < 0) player = script_player(current_plan);  // an AITarget argument: the calling plan's registry
    if (player < 0 || !valid_planet(static_cast<PlanetIndex>(planet))) return;
    markup[{player, planet}] += delta;
}

// ================================================================ PlayerAi

bool PlayerAi::blocked(uint32_t goal_type, int32_t target) {
    if (target < 0) return false;
    const int m = std::min(5, static_cast<int>(w->defs.goals[goal_type].reachability));
    return !reachable(m, static_cast<PlanetIndex>(target));
}

bool PlayerAi::reachable(int mode, PlanetIndex target) {
    if (reach_frame != w->now) {
        reach_frame = w->now;
        reach.assign(6, {});
    }
    std::vector<uint8_t>& r = reach[static_cast<size_t>(mode)];
    if (r.empty()) flood(mode, &r);
    return target < r.size() && r[target];
}

// The reachability flood 0x3f12e0 from the player's planets (row order) at AI level `mode` (FIXPASS_GALACTIC §1.4,
// ai_reach_flood): reached = Edge_Cost accepts the planet as the destination, expanded = accepted as a transit planet,
// with the corrected request flags (Friendly_Only: everything but fog off, so only planets held by the player or an
// ally that are not dangerous). Danger = the AI's Is_Planet_Dangerous (GalacticPerception::fill_view). Returns the
// reached planets in flood output order.
std::vector<PlanetIndex> PlayerAi::flood(int mode, std::vector<uint8_t>* reached) {
    const gal::PlanetGraph& g = w->sim.graph();
    gal::PlayerView view = w->sim.player_view(player);
    view.dangerous = w->ai_dangerous(player);
    const auto planets = w->sim.planets();
    std::vector<PlanetIndex> starts;
    for (size_t i = 0; i < planets.size(); ++i)
        if (planets[i].is_planet && planets[i].owner == player) starts.push_back(static_cast<PlanetIndex>(i));
    std::vector<uint8_t> r;
    std::vector<PlanetIndex> out = ai_reach_flood(g, view, starts, mode, &r);
    r.resize(planets.size(), 0);
    if (reached) *reached = std::move(r);
    return out;
}

gal::Bitset GalacticAiWorld::Impl::ai_dangerous(int32_t player) const {
    if (snap.planet_count != sim.planets().size()) return {};
    ALAMO_PROFILE_SCOPE("ai.gal.danger");  // OPT2 t11: the perception reader on the goal / market paths
    gal::PlayerView v;
    perc.fill_view(snap, player, v);
    return std::move(v.dangerous);
}

// 0x665160 (AI_GALACTIC_3 Â§8) [H; M level flags]: recomputed when more than 10 x FPS frames old.
const std::vector<PlanetIndex>& PlayerAi::source_planets(int level) {
    SourceList& s = sources[static_cast<size_t>(std::clamp(level, 0, 5))];
    if (s.frame < 0 || w->now - s.frame > static_cast<int64_t>(10 * kAiFps)) {
        s.frame = w->now;
        s.planets = flood(std::clamp(level, 0, 5), nullptr);
    }
    return s.planets;
}

// The AI's remaining galactic population for the solver 0x5f5080 (AI_GALACTIC_3 Â§3.5) [H; M "committed"]: capacity -
// pop of the galactic free-store units (skipping units at unusable planets) - the population committed by the
// player's instantiated plans (PP+0x1A8 over the budget's goal structs).
double GalacticAiWorld::Impl::ai_remaining_pop(int32_t player) const {
    double r = static_cast<double>(sim.population_capacity(player));
    for (const ObjectHandle u : free_units(player)) {
        // A2 gal t04: "unusable planet" = not revealed (Is_Planet_Revealed 0x3fa420, A2GAL_READINGS 2.3) [H].
        if (sim.population_skips(player, planet_of_unit(u))) continue;
        r -= pop_of(store.type(u));
    }
    for (const auto& [id, pi] : plans) {
        if (pi.player != player || pi.ended) continue;
        const auto it = pps.find(pi.pp);
        if (it != pps.end()) r -= it->second.pop;
    }
    return r;
}

bool PlayerAi::accept(Goal& g) {
    // Find_Plan_For_Goal (0x6bae50): template-enabled plans serving the type, library order; ONE synced draw.
    std::vector<uint32_t> cand;
    std::vector<double> ratio;
    for (size_t i = 0; i < w->library.plans.size(); ++i)
        if (ctx.templates.plan_value(static_cast<uint32_t>(i)) > 0 && w->library.plans[i].serves(g.type)) {
            cand.push_back(static_cast<uint32_t>(i));
            ratio.push_back(ctx.learning.plan_success_ratio(static_cast<uint32_t>(i)));
        }
    const std::string& gname = w->defs.goals[g.type].name;
    if (cand.empty()) {
        ++w->stats[gname + " no plan"];
        return false;
    }
    const uint32_t k = pick_weighted(ratio, w->rng.synced());
    const uint32_t plan = cand[k];
    // PotentialPlan solve at proposal time (draws Plan Setup C / IgnoreTarget contrast / MagicPlan floats).
    // Init's first solve ignores the free-store reservations and reserves nothing (A1_READINGS §28.6 step 1).
    PotentialPlanRec pp;
    if (!w->solve_potential_plan(*this, plan, g.type, g.target, pp, true)) {
        ++w->stats[gname + " no potential plan (" + w->library.plans[plan].name + ")"];
        return false;
    }
    ++w->stats[gname + " accepted"];
    const int32_t h = w->next_pp++;
    g.plan = plan;
    g.pp_valid = true;
    g.cost = pp.net;  // Init 0x6c0d00: net cost -> goal +0x78, ETA -> +0x7c
    g.eta = pp.eta;
    g.pp_handle = h;
    w->pps.emplace(h, std::move(pp));
    return true;
}

bool PlayerAi::plan_invalid(const Goal& g) {
    if (g.running < 0) return true;
    const auto it = w->plans.find(g.running);
    return it == w->plans.end() || it->second.ended || w->pp_stolen(g.pp_handle);
}

bool PlayerAi::replaceable(const Goal& g) {
    if (g.running < 0) return true;
    const auto it = w->plans.find(g.running);
    return it == w->plans.end() || !it->second.non_removable;
}

bool PlayerAi::can_fund(const Goal& g) {
    const int32_t c = category_of(g);
    if (ctx.budget.has_goal(c, g.serial)) return ctx.budget.can_fund_goal(c, g.serial);
    return ctx.budget.can_fund(c, g.cost, g.eta, time_limit(g));
}

void PlayerAi::budget_add(Goal& g) {
    if (ctx.budget.add_goal(category_of(g), g.serial, g.cost, g.eta, time_limit(g))) g.budgeted = true;
}

void PlayerAi::budget_release(Goal& g) {
    if (!g.budgeted) return;
    float r = 0.0f;  // PAYG reclaim 0x5f7d40, computed when the plan ended (its build tasks are gone by now)
    if (const auto it = w->reclaim.find(g.serial); it != w->reclaim.end()) {
        r = it->second;
        w->reclaim.erase(it);
    }
    ctx.budget.release(category_of(g), g.serial, r);
    g.budgeted = false;
}

void PlayerAi::budget_weights() {
    // 0x614590 / 0x6137b0: each category's equation, pre-loaded with the previous normalised weight (a failing
    // equation keeps it); negatives -> 0; normalised.
    const size_t n = w->defs.categories.size();
    std::vector<float> raw(n, 0.0f);
    for (size_t c = 0; c < n; ++c) {
        const auto ci = static_cast<int32_t>(c);
        raw[c] = ctx.budget.initialised() ? ctx.budget.weight(ci) : 0.0f;
        const std::string& eq = ctx.templates.budget_equation(ci);
        double v = 0.0;
        if (!eq.empty() && evaluate(eq, -1, v)) raw[c] = static_cast<float>(v);
    }
    if (!ctx.budget.initialised()) ctx.budget.init(raw, w->sim.credits(player));
    else ctx.budget.reweight(raw);
}

// Per galactic income tick (0x614340 from 0x4b2ef0, AI_GALACTIC_2 Â§3.1) [H]: GalacticSim's income-tick listener
// (A2 galactic hook), per AI player inside the payment loop right after its Add_Credits, never on a zero-income
// cycle (A2GAL_READINGS 1.1); income = the raw Player_Income (before Credit_Multiplier, floor and cap) [H],
// T = the income period; the drift reads the credits after this player's Add_Credits.
void GalacticAiWorld::Impl::on_income(const gal::IncomeTick& t) {
    PlayerAi* pa = player_ai(t.player);
    if (!pa || !pa->ctx.budget.initialised()) return;
    if (t.player < 0 || static_cast<size_t>(t.player) >= sim.players().size() || sim.players()[static_cast<size_t>(t.player)].human) return;
    // T = 0x4b2010 = GameConstants Fiscal_Cycle_Time_In_Secs (AI_GALACTIC_3 Â§8) [H].
    if (t.gain == 0.0f) return;  // the sim never ticks a zero-income cycle; keep the accrual off for it anyway
    pa->ctx.budget.tick(t.gain, sim.rules().constants.fiscal_cycle_seconds, sim.credits(t.player));
}

std::vector<int32_t> PlayerAi::category_order() {
    std::vector<int32_t> order;
    for (size_t c = 0; c < w->defs.categories.size(); ++c) order.push_back(static_cast<int32_t>(c));
    std::stable_sort(order.begin(), order.end(), [this](int32_t a, int32_t b) { return ctx.budget.weight(a) > ctx.budget.weight(b); });
    return order;
}

void PlayerAi::queue_production(Goal& g) {
    if (g.pp_handle >= 0) w->reserve_pp_queues(g.pp_handle);
}

void PlayerAi::drop_potential_plan(Goal& g) {
    if (g.pp_handle >= 0) {
        w->release_pp(g.pp_handle);
        w->pps.erase(g.pp_handle);
    }
    g.pp_handle = -1;
}

PlayerAi::PlayerAi(GalacticAiWorld::Impl* world, int32_t p, const AiPlayerTypeDef* t, int32_t d)
    : w(world), player(p), type(t), difficulty(d) {
    paths.set_background(world->cfg.path_latency, world->cfg.background);  // OPT2 t02
}

int32_t PlayerAi::quota_players() {
    int32_t n = 0, humans = 0;
    for (const gal::PlayerState& ps : w->sim.players()) {
        ++n;
        humans += ps.human ? 1 : 0;
    }
    return n - humans;
}

void PlayerAi::first_service(int64_t) {
    // Opponent (Â§1.1): enemies in index order owning planets or fleets, not neutral; a human wins at once,
    // else the last playable one [M].
    const auto players = w->sim.players();
    const auto& rules = w->sim.rules();
    int32_t pick = -1;
    for (size_t p = 0; p < players.size(); ++p) {
        const auto pi = static_cast<int32_t>(p);
        if (pi == player || !w->sim.is_hostile(player, pi)) continue;
        const uint32_t f = players[p].faction;
        if (f < rules.factions.size() && rules.factions[f].is_neutral) continue;
        bool owns = w->sim.planets_owned(pi) > 0;
        for (const gal::FleetState& fl : w->sim.fleets()) owns = owns || (fl.alive && fl.owner == pi);
        if (!owns) continue;
        if (players[p].human) {
            pick = pi;
            break;
        }
        if (f < rules.factions.size() && rules.factions[f].is_playable) pick = pi;
    }
    ctx.opponent = pick;
    w->decide(player, "opponent " + std::to_string(pick));
    // Execution slot 6 (0x526120, PHASE2_ANSWERS Q11): create the path finder, recompute at once, stagger the next
    // recompute by slot (player index [M: PlayerClass +0x48]) over K = players - humans.
    paths.create(w->sim.graph(), w->path_input(player), w->sim.frame(), player, quota_players(), w->cfg.path_pf);
}

void PlayerAi::template_init(int64_t) {
    budget_weights();
    std::string names;
    for (const uint32_t t : ctx.templates.active()) names += " " + w->defs.templates[t].name;
    w->decide(player, "templates" + names);
}

void PlayerAi::execution_service(int64_t) {
    // Free-store move arrivals (FIXPASS_GALACTIC §1.3, §1.5) [port]: the arrival signal fired during the galactic tick,
    // before this service, so Get_Next_Hop reads the matrix as of the last recompute (not this service's).
    w->step_free_store_moves(player);
    // 0x526590: the path finder's 90-frame recompute first (galactic mode frame), then the rest (AI_GALACTIC §2.8).
    paths.service(w->sim.frame(), w->sim.graph(), [this] { return w->path_input(player); }, w->cfg.path_pf);
    w->claim_units(player);
    w->service_build_tasks(player);
    w->service_statuses(player);
    w->auto_land_free_units(player);  // unit hook step 2 (AI_GALACTIC §2.8)
    w->service_free_store(player);
    for (size_t i = 0; i < bonuses.size();) {
        if (ctx.goals.give_desire_bonus(bonuses[i].goal_type, bonuses[i].target, bonuses[i].bonus) || bonuses[i].deadline <= w->now)
            bonuses.erase(bonuses.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
}

void PlayerAi::templates_changed(int64_t) {
    std::string names;
    for (const uint32_t t : ctx.templates.active()) names += " " + w->defs.templates[t].name;
    w->decide(player, "templates" + names);
}

// ================================================================ plans

int32_t GalacticAiWorld::Impl::instantiate(PlayerAi& p, Goal& g) {
    const auto ppit = pps.find(g.pp_handle);
    if (g.plan == kAiNone || ppit == pps.end()) return -1;
    std::string err;
    std::unique_ptr<lua::LuaVm> vm = runtime->acquire(g.plan, &err);
    if (!vm) {
        errors.push_back(err);
        return -1;
    }
    const PotentialPlanRec& pp = ppit->second;
    const PlanDefinition& def = library.plans[g.plan];
    PlanInstance pi;
    pi.id = next_plan++;
    pi.player = p.player;
    pi.plan = g.plan;
    pi.goal_type = g.type;
    pi.goal_key = g.serial;
    pi.category = defs.goals[g.type].category;
    pi.target = g.target;
    pi.pp = g.pp_handle;
    pi.test_contrast = pp.contrast_ok;
    pi.target_owner = g.target >= 0 ? sim.planets()[static_cast<size_t>(g.target)].owner : -1;
    lua::attach_ai_host(*vm, this, pi.id);
    lua::attach_markup_host(*vm, this);
    vm->set_time(now_seconds(sim));
    vm->set_frame(static_cast<uint32_t>(sim.frame()));
    if (g.target >= 0) {
        lua::set_ai_target_global(*vm, "AITarget", g.target);
        vm->set_object_global("Target", sim.planets()[static_cast<size_t>(g.target)].object);
    } else {
        lua::set_nil_global(*vm, "AITarget");
        lua::set_nil_global(*vm, "Target");
    }
    lua::set_budget_global(*vm, pi.id);
    lua::set_player_global(*vm, "PlayerObject", p.player);
    std::string tf_names;
    for (size_t i = 0; i < def.task_forces.size(); ++i) {
        TaskForceRec tf;
        tf.id = next_tf++;
        tf.plan = pi.id;
        tf.player = p.player;
        tf.def = static_cast<uint32_t>(i);
        tf.name = def.task_forces[i].name;
        // Stage (0x140b3a660 through the target's reachability object) [port: the player's planet nearest the
        // target in hops; the target itself when the player owns none on a path].
        {
            const PlanetIndex st = nearest_owned(p.player, def.ignore_target ? -1 : g.target);
            tf.stage = st == kInvalidPlanet ? g.target : static_cast<int32_t>(st);
        }
        for (const PpSelection& s : pp.sel) {
            if (s.tf != static_cast<int32_t>(i)) continue;
            if (s.unit) {
                if (store.alive(s.unit) && store.owner(s.unit) == p.player && !tf_owns(s.unit)) {
                    off_free_store(s.unit);
                    tf.units.push_back(s.unit);
                }
            } else {
                tf.wanted.push_back(s.type);
                tf.wanted_source.push_back(s.source);
            }
        }
        tf.had_units = !tf.units.empty();
        lua::set_task_force_global(*vm, tf.name, tf.id, lua::AiTaskForceKind::Galactic);
        if (vm->create_thread(tf.name + "_Thread") < 0) continue;  // a missing thread removes the TF
        tf_names += " " + tf.name + "(" + std::to_string(tf.units.size()) + "+" + std::to_string(tf.wanted.size()) + ")";
        pi.tfs.push_back(tf.id);
        tfs.emplace(tf.id, std::move(tf));
    }
    pi.vm = std::move(vm);
    pi.live_threads = static_cast<uint32_t>(pi.tfs.size());
    // AdditionalCost is charged once at instantiation (AI_GALACTIC Â§1.9.4 step 5): the goal's budget spend.
    if (def.additional_cost != 0.0f) p.ctx.budget.spend(pi.category, pi.goal_key, def.additional_cost);
    char cost[64];
    std::snprintf(cost, sizeof cost, " net %.0f gross %.0f eta %.1f", static_cast<double>(pp.net), static_cast<double>(pp.gross),
                  static_cast<double>(pp.eta));
    decide(p.player, "plan " + std::to_string(pi.id) + " " + def.name + " for " + defs.goals[g.type].name + " @" +
                         (g.target >= 0 ? planet_name(static_cast<PlanetIndex>(g.target)) : std::string("global")) + tf_names + cost);
    const int32_t id = pi.id;
    plans.emplace(id, std::move(pi));
    return id;
}

void GalacticAiWorld::Impl::end_plan(int32_t id, bool by_goal_system) {
    const auto it = plans.find(id);
    if (it == plans.end() || it->second.ended) return;
    PlanInstance& pi = it->second;
    pi.ended = true;
    for (auto s = statuses.begin(); s != statuses.end();) s = s->second.plan == id ? statuses.erase(s) : std::next(s);
    if (PlayerAi* owner = player_ai(pi.player)) {  // the PAYG reclaim of the goal's release (0x614880 -> 0x5f7d40)
        const AiBudget::GoalEntry* e = owner->ctx.budget.goal(pi.category, pi.goal_key);
        if (e && sim.pays_as_you_go(pi.player) && e->c + e->a > 0.0f) reclaim[pi.goal_key] = payg_reclaim(id, e->c + e->a);
    }
    end_plan_builds(id);
    for (const int32_t t : pi.tfs) {
        const auto tit = tfs.find(t);
        if (tit == tfs.end()) continue;
        for (const ObjectHandle u : tit->second.units) pp_unit_left(id, u);  // plan end 0x5f6a50 step 3 -> 0x6c27d0
        tfs.erase(tit);
    }
    PlayerAi* pa = player_ai(pi.player);
    if (pa) {
        // [L, AI_GALACTIC_2 open item 6] a plan's exit removes its Block_Goal_Proposal entries.
        if (pi.blocking) pa->ctx.goals.unblock_proposals(pi.goal_key);
        const float dur = pi.goal_type < defs.goals.size() ? defs.goals[pi.goal_type].tracking_duration : 0.0f;
        pa->ctx.learning.record_goal_outcome(pi.goal_type, pi.target, pi.plan, pi.result, dur, now);
        if (!by_goal_system) pa->ctx.goals.plan_ended(id);
    }
    if (pi.pp >= 0) release_pp(pi.pp);  // 0x6c2320 queue reservations
    decide(pi.player, "plan " + std::to_string(id) + " " + library.plans[pi.plan].name + " ended " +
                          (pi.result ? "success" : "failure") + (by_goal_system ? " (released)" : ""));
    if (pi.vm) {
        append_script_errors(*pi.vm, library.plans[pi.plan].name + ": ", errors);  // S7 t27: with SCRIPT ERROR lines
        lua::attach_markup_host(*pi.vm, nullptr);
        runtime->release(pi.plan, std::move(pi.vm));
    }
    plans.erase(it);
}

void GalacticAiWorld::Impl::tf_event(const TaskForceRec& tf, PlanEvent event, const std::vector<lua::AiEventArg>& args) {
    const auto it = plans.find(tf.plan);
    if (it == plans.end() || !it->second.vm) return;
    std::string err;
    // Raised while the plan's own script runs (inside a binding): queued for its next PumpEvents, so a handler
    // may ScriptExit (yield); otherwise called at once [M: the queued flag of 0x6006b0].
    const bool queued = current_plan == tf.plan;
    const std::string_view name = plan_event_name(event);
    if (!lua::dispatch_task_force_event(*it->second.vm, tf.id, lua::AiTaskForceKind::Galactic, tf.name, name, args, queued, &err) &&
        !err.empty())
        errors.push_back(library.plans[it->second.plan].name + " " + std::string(name) + ": " + err);
}

void GalacticAiWorld::Impl::drain_world_requests(lua::LuaPlayerWorld& w, int32_t player, int32_t plan) {
    for (const lua::DesireBonusRequest& r : w.desire_bonuses) {
        PlayerAi* t = player_ai(r.player);
        if (!t) continue;
        const uint32_t g = defs.find_goal(r.goal);
        const ObjectHandle h = store.find(r.target);
        PlanetIndex p = h ? sim.planet_of(h) : kInvalidPlanet;
        if (p == kInvalidPlanet && h) p = planet_of_unit(h);
        if (g == kAiNone) continue;
        PendingBonus b;
        b.goal_type = g;
        b.target = p == kInvalidPlanet ? -1 : static_cast<int32_t>(p);
        b.bonus = static_cast<float>(r.bonus);
        b.deadline = now + static_cast<int32_t>(static_cast<float>(kAiFps) * static_cast<float>(r.duration < 0.0 ? 0.0 : r.duration));
        t->bonuses.push_back(b);
    }
    w.desire_bonuses.clear();
    for (const int32_t p : w.purge_goal_requests)
        if (PlayerAi* t = player_ai(p)) t->ctx.goals.purge(plan);
    w.purge_goal_requests.clear();
    for (const lua::BudgetFlushRequest& r : w.budget_flushes)
        if (PlayerAi* t = player_ai(r.player >= 0 ? r.player : player)) t->ctx.budget.flush(defs.find_category(r.category));
    w.budget_flushes.clear();
    w.tactical_releases.clear();
}

void GalacticAiWorld::Impl::service_plans(int32_t player) {
    std::vector<int32_t> ids;
    for (const auto& [id, pi] : plans)
        if (pi.player == player && !pi.ended) ids.push_back(id);
    for (const int32_t id : ids) {
        auto it = plans.find(id);
        if (it == plans.end() || it->second.ended) continue;
        PlanInstance& pi = it->second;
        // Original_Target_Owner_Changed (code 5): old, new owner as player wrappers.
        if (pi.target >= 0) {
            const int32_t o = sim.planets()[static_cast<size_t>(pi.target)].owner;
            if (o != pi.target_owner) {
                const int32_t old = pi.target_owner;
                pi.target_owner = o;
                for (const int32_t t : std::vector<int32_t>(pi.tfs)) {
                    const auto tit = tfs.find(t);
                    if (tit == tfs.end()) continue;
                    lua::AiEventArg a, b;
                    a.kind = b.kind = lua::AiEventArg::Player;
                    a.id = old;
                    b.id = o;
                    tf_event(tit->second, PlanEvent::OriginalTargetOwnerChanged, {a, b});
                }
            }
        }
        it = plans.find(id);
        if (it == plans.end()) continue;
        lua::LuaVm& vm = *it->second.vm;
        vm.set_time(now_seconds(sim));
        vm.set_frame(static_cast<uint32_t>(sim.frame()));
        current_plan = id;
        vm.service_threads();
        current_plan = -1;
        drain_world_requests(world, player, id);
        it = plans.find(id);
        if (it == plans.end()) continue;
        it->second.live_threads = static_cast<uint32_t>(it->second.vm->thread_count());
        if (it->second.vm->exit_requested() || it->second.live_threads == 0) end_plan(id, false);
    }
}

// ================================================================ execution: units, statuses

int32_t GalacticAiWorld::Impl::new_status(int32_t plan, int32_t tf, StatusKind kind) {
    StatusRec s;
    s.id = next_status++;
    s.plan = plan;
    s.tf = tf;
    s.kind = kind;
    s.started = now;
    if (kind == StatusKind::Done) {
        s.finished = true;
        s.result = lua::AiStatusResult::True;
    }
    statuses.emplace(s.id, s);
    return s.id;
}

void GalacticAiWorld::Impl::prune_tf(TaskForceRec& tf) {
    auto& u = tf.units;
    u.erase(std::remove_if(u.begin(), u.end(), [this, &tf](ObjectHandle h) { return !store.alive(h) || store.owner(h) != tf.player; }),
            u.end());
}

std::vector<uint32_t> GalacticAiWorld::Impl::tf_fleets(const TaskForceRec& tf) const {
    std::vector<uint32_t> out;
    for (const ObjectHandle u : tf.units) {
        const uint32_t f = fleet_of_unit(u);
        if (f != gal::kNoFleet && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
    }
    return out;
}

// The fleet can jump: GalacticSim's rule (a member with Hyperspace and a speed > 0; begin_move refuses otherwise).
bool GalacticAiWorld::Impl::movable_fleet(uint32_t f) const {
    if (f >= sim.fleets().size() || !sim.fleets()[f].alive) return false;
    for (const ObjectHandle u : sim.fleets()[f].contents)
        if (store.alive(u) && unit_jumps(u)) return true;
    return false;
}

// STRATEGIC_GAPS Â§1.7: a ground company jumps as its Company_Transport_Unit (GalacticSim::travel_type).
bool GalacticAiWorld::Impl::unit_jumps(ObjectHandle u) const {
    const gal::TypeInfo* t = type_info(sim.travel_type(u));
    return t && t->hyperspace && t->hyperspace_speed > 0.0f;
}

uint32_t GalacticAiWorld::Impl::movable_tf_fleet_at(const TaskForceRec& tf, PlanetIndex p) const {
    for (const uint32_t f : tf_fleets(tf))
        if (!sim.fleets()[f].moving && sim.fleets()[f].planet == p && movable_fleet(f)) return f;
    return gal::kNoFleet;
}

void GalacticAiWorld::Impl::claim_units(int32_t player) {
    PlayerAi* pa = player_ai(player);
    if (!pa) return;
    std::vector<ObjectHandle> objs = player_units(player);
    for (const gal::PlanetState& ps : sim.planets()) {
        if (ps.starbase && store.alive(ps.starbase) && store.owner(ps.starbase) == player) objs.push_back(ps.starbase);
        for (const ObjectHandle s : ps.structures)
            if (store.alive(s) && store.owner(s) == player) objs.push_back(s);
    }
    for (const ObjectHandle u : objs) {
        const objects::ObjectId id = store.id(u);
        if (!pa->known.insert(id).second) continue;
        const uint32_t type = store.type(u);
        // Signals 3 / 4 of the producing planet (0x53c4f0): the AI build task with the matching production
        // (planet, type, earliest queue id) claims the new object.
        const PlanetIndex at = sim.planet_of(sim.container_of(u)) != kInvalidPlanet ? sim.planet_of(sim.container_of(u)) : planet_of_unit(u);
        BuildTask* match = nullptr;
        for (auto& [bid, b] : builds)
            if (b.state == BuildState::Producing && b.player == player && b.type == type && b.producing_at == at &&
                (!match || b.queue_id < match->queue_id))
                match = &b;
        if (match) {
            const auto tit = tfs.find(match->tf);
            PlanetBuildQueue& q = planet_queues[match->producing_at][match->queue];
            if (q.current == match->id) {
                q.current = -1;
                q.current_queue_id = -1;
                q.current_type = gal::kNoIndex;
            }
            if (PlayerAi* owner = player_ai(match->player)) owner->ctx.budget.paid(match->category, match->goal_key, match->charge);
            finish_build(*match);
            if (tit != tfs.end()) {
                tit->second.units.push_back(u);  // PRODUCTION_END -> JOIN_TASK_FORCE
                tit->second.had_units = true;
                continue;
            }
        } else {
            // A finished production whose task is gone (the plan ended): the queue frees and the object goes to
            // the free store.
            if (at < planet_queues.size())
                for (PlanetBuildQueue& q : planet_queues[at])
                    if (q.current < 0 && q.current_queue_id >= 0 && q.current_type == type) {
                        q.current_queue_id = -1;
                        q.current_type = gal::kNoIndex;
                        break;
                    }
        }
        const gal::TypeInfo* ti = type_info(type);
        if (!ti || structure_kind(ti->kind)) continue;
        if (pa->fs_vm && now > 0) {  // On_Unit_Added (vf19), synchronous
            lua::AiEventArg a;
            a.kind = lua::AiEventArg::Object;
            a.object = u;
            std::string err;
            if (!lua::call_global_function(*pa->fs_vm, "On_Unit_Added", {a}, &err) && !err.empty())
                errors.push_back("free store " + std::to_string(player) + " On_Unit_Added: " + err);
        }
    }
    // Task forces that lost every unit (No_Units_Remaining, galactic code 7 [H]).
    std::vector<int32_t> empty;
    for (auto& [id, tf] : tfs) {
        if (tf.player != player) continue;
        prune_tf(tf);
        bool pending = false;
        for (const int32_t b : tf.builds) {
            const auto bit = builds.find(b);
            pending = pending || (bit != builds.end() && (bit->second.state == BuildState::Queued || bit->second.state == BuildState::Producing));
        }
        if (tf.had_units && tf.units.empty() && !pending && !tf.no_units_sent) {
            tf.no_units_sent = true;
            empty.push_back(id);
        }
    }
    for (const int32_t id : empty) {
        const auto it = tfs.find(id);
        if (it != tfs.end()) {
            const TaskForceRec copy = it->second;
            tf_event(copy, PlanEvent::NoUnitsRemaining, {});
        }
    }
}

void GalacticAiWorld::Impl::service_statuses(int32_t player) {
    std::vector<int32_t> ids;
    for (const auto& [id, s] : statuses) {
        if (s.finished) continue;
        if (script_player(s.plan) == player) ids.push_back(id);
    }
    for (const int32_t id : ids) {
        const auto it = statuses.find(id);
        if (it == statuses.end() || it->second.finished) continue;
        for (int k = 0; k < 50 && !it->second.finished; ++k)  // up to 50 transitions per service [H]
            if (!step_status(it->second)) break;
    }
    for (auto s = statuses.begin(); s != statuses.end();)
        s = (s->second.finished && plans.find(s->second.plan) == plans.end()) ? statuses.erase(s) : std::next(s);
}

// One FSM step; returns true when it changed state and may step again this service.
bool GalacticAiWorld::Impl::step_status(StatusRec& s) {
    auto finish = [&s](bool ok) {
        s.finished = true;
        s.result = ok ? lua::AiStatusResult::True : lua::AiStatusResult::False;
        return false;
    };
    const auto tit = tfs.find(s.tf);
    switch (s.kind) {
        case StatusKind::Done: return finish(true);
        case StatusKind::BudgetWait: {
            const double v = budget_op(s.plan, s.op == lua::AiBudgetOp::WaitSpendable ? lua::AiBudgetOp::Spendable : lua::AiBudgetOp::Unallocated,
                                       0.0, {});
            if (v - s.amount >= -0.0001) return finish(true);
            return false;
        }
        case StatusKind::ProduceForce: {
            if (tit == tfs.end()) return finish(false);
            TaskForceRec& tf = tit->second;
            prune_tf(tf);
            for (const int32_t b : tf.builds) {
                const auto bit = builds.find(b);
                if (bit != builds.end() && (bit->second.state == BuildState::Queued || bit->second.state == BuildState::Producing))
                    return false;  // still producing (no time-out, Â§2.3)
            }
            if (tf.units.empty()) return finish(false);
            if (tf.stage < 0) return finish(true);
            const auto stage = static_cast<PlanetIndex>(tf.stage);
            bool all_there = true;
            std::vector<ObjectHandle> drop;
            // Pickups: ground units on a surface without a TF hyperspace fleet in orbit get one sent to them
            // (first idle TF fleet, planet order) [port: AIBuildTask TRANSPORTS_TO_ORBIT / PATHING_DEST].
            std::vector<uint32_t> busy;
            {
                std::vector<PlanetIndex> pickup;
                for (const ObjectHandle u : tf.units) {
                    const PlanetIndex at = planet_of_unit(u);
                    if (at == kInvalidPlanet || at == stage || !on_surface(u)) continue;
                    const gal::TypeInfo* uti = type_info(store.type(u));
                    if (uti && structure_kind(uti->kind)) continue;
                    const uint32_t here = movable_tf_fleet_at(tf, at);
                    if (here != gal::kNoFleet) {  // launch (op 4) then form (op 0) into that fleet; it waits this pass
                        launch_and_form(tf.player, at, u, here);
                        busy.push_back(here);
                    } else if (unit_jumps(u)) {
                        // a company with its own transport (SG 1.7) launches into a fleet of its own below
                    } else if (std::find(pickup.begin(), pickup.end(), at) == pickup.end()) {
                        pickup.push_back(at);
                    }
                }
                std::sort(pickup.begin(), pickup.end());
                for (const PlanetIndex p : pickup) {
                    bool coming = false;
                    for (const uint32_t f : tf_fleets(tf)) coming = coming || (sim.fleets()[f].moving && sim.fleets()[f].to == p);
                    if (coming) continue;
                    for (const uint32_t f : tf_fleets(tf)) {
                        if (sim.fleets()[f].moving || !movable_fleet(f) || std::find(busy.begin(), busy.end(), f) != busy.end()) continue;
                        if (std::find(pickup.begin(), pickup.end(), sim.fleets()[f].planet) != pickup.end()) continue;
                        if (move_fleet(tf.player, f, p)) busy.push_back(f);
                        break;
                    }
                }
            }
            bool any_movable = false;
            for (const uint32_t f : tf_fleets(tf)) any_movable = any_movable || movable_fleet(f);
            for (const ObjectHandle u : tf.units) {
                const gal::TypeInfo* uti = type_info(store.type(u));
                if (uti && structure_kind(uti->kind)) continue;  // starbases / structures stay where built
                const PlanetIndex at = planet_of_unit(u);
                if (at == stage) continue;
                all_there = false;
                if (at == kInvalidPlanet) continue;  // in transit
                const uint32_t f = fleet_of_unit(u);
                if (f == gal::kNoFleet || !movable_fleet(f)) {
                    if (f == gal::kNoFleet && movable_tf_fleet_at(tf, at) != gal::kNoFleet) {
                        launch_and_form(tf.player, at, u, movable_tf_fleet_at(tf, at));
                    } else if (f == gal::kNoFleet && unit_jumps(u)) {
                        launch_and_form(tf.player, at, u, kAloneFleet);  // it carries itself (its own transport)
                    } else if (f != gal::kNoFleet || !any_movable) {
                        drop.push_back(u);  // stuck in a fleet without hyperspace, or no ship to fetch it [port]
                    }
                } else if (!sim.fleets()[f].moving && std::find(busy.begin(), busy.end(), f) == busy.end()) {
                    if (!move_fleet(tf.player, f, stage)) return finish(false);  // pathing to the destination failed
                }
            }
            if (!drop.empty()) {
                for (const ObjectHandle d : drop) tf_release_unit(tf.id, d);
                decide(tf.player, "tf " + tf.name + " leaves " + std::to_string(drop.size()) + " immobile units");
                return true;
            }
            if (all_there) return finish(true);
            return false;  // [H] no time-out in the build FSM
        }
        case StatusKind::MoveTo: {
            if (tit == tfs.end()) return finish(false);
            if (sim.paused()) return false;
            TaskForceRec& tf = tit->second;
            prune_tf(tf);
            if (tf.units.empty()) return finish(false);
            {
                std::vector<ObjectHandle> drop;
                for (const ObjectHandle u : tf.units) {
                    const gal::TypeInfo* uti = type_info(store.type(u));
                    if (uti && structure_kind(uti->kind)) continue;
                    const PlanetIndex at = planet_of_unit(u);
                    if (at == kInvalidPlanet || at == s.dest) continue;
                    const uint32_t f = fleet_of_unit(u);
                    if (f != gal::kNoFleet && movable_fleet(f)) continue;
                    if (f == gal::kNoFleet && movable_tf_fleet_at(tf, at) != gal::kNoFleet) {
                        launch_and_form(tf.player, at, u, movable_tf_fleet_at(tf, at));
                    } else if (f == gal::kNoFleet && unit_jumps(u)) {
                        launch_and_form(tf.player, at, u, kAloneFleet);
                    } else {
                        drop.push_back(u);
                    }
                }
                for (const ObjectHandle d : drop) tf_release_unit(tf.id, d);
                if (tf.units.empty()) return finish(false);
            }
            bool arrived = true;
            for (const uint32_t f : tf_fleets(tf)) {
                const gal::FleetState& fs = sim.fleets()[f];
                if (fs.moving) {
                    arrived = false;
                    continue;
                }
                if (fs.planet == s.dest || !movable_fleet(f)) continue;
                arrived = false;
                gal::PathScratch scratch;
                std::vector<PlanetIndex> path;
                const gal::PlayerView view = sim.player_view(tf.player);
                const gal::PathRequest req = gal::PathRequest::for_fleet_move_mode(fs.planet, s.dest, s.mode);
                if (!gal::find_fleet_path(sim.graph(), view, req, scratch, path) || path.size() < 2) {
                    const gal::PathRequest any = gal::PathRequest::for_fleet_move_mode(fs.planet, s.dest, 5);
                    if (!gal::find_fleet_path(sim.graph(), view, any, scratch, path) || path.size() < 2) {
                        decide(tf.player, "tf " + tf.name + " no path " + planet_name(fs.planet) + " -> " + planet_name(s.dest));
                        return finish(false);
                    }
                }
                const PlanetIndex hop = path[1];
                if (hop != s.dest && s.mode < 2) {
                    bool hostile_orbit = false;
                    for (const uint32_t o : sim.planets()[hop].orbit)
                        hostile_orbit = hostile_orbit || (sim.fleets()[o].alive && sim.is_hostile(tf.player, sim.fleets()[o].owner));
                    if (hostile_orbit) return finish(false);
                }
                if (!move_fleet(tf.player, f, hop)) return finish(false);
            }
            for (const ObjectHandle u : tf.units) {
                const gal::TypeInfo* uti = type_info(store.type(u));
                if (uti && structure_kind(uti->kind)) continue;
                if (planet_of_unit(u) != s.dest) arrived = false;
            }
            if (arrived && !sim.planets()[s.dest].in_battle) return finish(true);
            return false;
        }
        case StatusKind::Invade: {
            if (!valid_planet(s.dest)) return finish(false);
            const gal::PlanetState& ps = sim.planets()[s.dest];
            const int32_t player = tit != tfs.end() ? tit->second.player : script_player(s.plan);
            if (ps.owner == player) return finish(true);
            if (ps.in_battle) return false;
            if (now - s.started > static_cast<int64_t>(kAiFps) * 30) return finish(false);
            return false;
        }
    }
    return false;
}

int32_t GalacticAiWorld::Impl::tf_order(int32_t tf, lua::AiTfOrder kind, const lua::AiArg& arg) {
    const auto it = tfs.find(tf);
    if (it == tfs.end() || !it->second.alive) return -1;
    TaskForceRec& t = it->second;
    prune_tf(t);
    const int32_t plan = t.plan;
    switch (kind) {
        case lua::AiTfOrder::MoveTo:
        case lua::AiTfOrder::Raid: {
            const PlanetIndex dest = planet_arg(arg);
            if (!valid_planet(dest) || t.units.empty()) return -1;
            const int32_t id = new_status(plan, tf, StatusKind::MoveTo);
            StatusRec& s = statuses[id];
            s.dest = dest;
            const auto pit = plans.find(plan);
            s.mode = pit != plans.end() && pit->second.goal_type < defs.goals.size()
                         ? static_cast<uint8_t>(std::min(5, static_cast<int>(defs.goals[pit->second.goal_type].reachability)))
                         : uint8_t{5};
            decide(t.player, "tf " + t.name + " move to " + planet_name(dest));
            return id;
        }
        case lua::AiTfOrder::LandUnits: {
            // Land_Units 0x6cec80: ground units land directly; transports detach (op 2) then land (op 5); up to
            // Max_Ground_Forces_On_Planet (Â§5.4). A2 galactic: FleetManagement land (op 5) per unit, onto the player's
            // own planet [port: the sim carries ground units in fleets directly, so there is no transport to detach].
            std::map<PlanetIndex, std::vector<ObjectHandle>> by_planet;
            for (const ObjectHandle u : t.units) {
                const gal::TypeInfo* ti = type_info(store.type(u));
                const uint32_t f = fleet_of_unit(u);
                if (ti && ground_kind(ti->kind) && f != gal::kNoFleet && !sim.fleets()[f].moving) by_planet[sim.fleets()[f].planet].push_back(u);
            }
            // P4 ai: every landed unit fires the Ground_Activated trigger (Lua_Land_Units 0x6cec80 -> 0x3a00b0 per landed
            // unit, P4_SIM_SYSTEMS Â§3.4: the HackSuperWeapon plan's hack). [port] At a planet the player does not own the
            // port lands nothing (no invasion here), but the units there still fire the trigger as the landing would.
            std::vector<std::pair<ObjectHandle, PlanetIndex>> landed;
            for (auto& [p, units] : by_planet) {
                if (!valid_planet(p)) continue;
                if (sim.planets()[p].owner != t.player) {
                    for (const ObjectHandle u : units) landed.emplace_back(u, p);
                    continue;
                }
                const auto room = static_cast<size_t>(std::max<int64_t>(
                    0, static_cast<int64_t>(sim.rules().constants.max_ground_forces_on_planet) - static_cast<int64_t>(sim.planets()[p].surface.size())));
                if (units.size() > room) units.resize(room);
                for (const ObjectHandle u : units) {
                    const uint32_t f = fleet_of_unit(u);
                    if (f == gal::kNoFleet) continue;
                    if (fleet_op(t.player, gal::kFleetLand, static_cast<uint32_t>(store.id(sim.planets()[p].object)),
                                 static_cast<uint32_t>(store.id(sim.fleets()[f].object)), static_cast<uint32_t>(store.id(u)),
                                 "land " + std::to_string(store.id(u)) + " at " + planet_name(p)) == cmd::RejectReason::None)
                        landed.emplace_back(u, p);
                }
            }
            ground_trigger(t, landed);
            return new_status(plan, tf, StatusKind::Done);
        }
        case lua::AiTfOrder::LaunchUnits: {
            // Launch_Units 0x6cf070: launch (op 4) then form a fleet (op 0); aborts if blockaded [H].
            std::map<PlanetIndex, std::vector<ObjectHandle>> by_planet;
            for (const ObjectHandle u : t.units)
                if (on_surface(u)) by_planet[planet_of_unit(u)].push_back(u);
            for (const auto& [p, units] : by_planet) {
                if (sim.ai_blockaded(p)) return -1;  // 0x3fa370 (A2GAL_READINGS 4)
                // A2 galactic: FleetManagement launch (op 4) into the orbit fleet; the launched units form one fleet
                // there (op 0 not needed: they share the orbit fleet) [port: no company transports, see launch_and_form].
                for (const ObjectHandle u : units) launch_and_form(t.player, p, u, gal::kNoFleet);
            }
            return new_status(plan, tf, StatusKind::Done);
        }
        case lua::AiTfOrder::Invade: {
            std::map<PlanetIndex, std::vector<ObjectHandle>> by_planet;
            for (const ObjectHandle u : t.units) {
                const gal::TypeInfo* ti = type_info(store.type(u));
                const uint32_t f = fleet_of_unit(u);
                if (ti && ground_kind(ti->kind) && f != gal::kNoFleet && !sim.fleets()[f].moving) by_planet[sim.fleets()[f].planet].push_back(u);
            }
            if (by_planet.empty()) return -1;
            auto pick = by_planet.begin();
            const auto pit = plans.find(plan);
            if (pit != plans.end() && pit->second.target >= 0) {
                const auto at_target = by_planet.find(static_cast<PlanetIndex>(pit->second.target));
                if (at_target != by_planet.end()) pick = at_target;
            }
            const auto& [p, units] = *pick;
            if (!valid_planet(p)) return -1;
            // 0x6ce600 calls GameObjectManager::Land_Invade (GalacticSim::invade) directly with the fleet owner as the
            // attacker, not the Invade event: the event executor's gates (0x48ff50: galactic pause counter, pending
            // story-linked battle, issuer = local player) do not apply (A1_READINGS §31.2, a1h t03).
            if (note_order(t.player, sim.invade(t.player, p, units), "invade " + planet_name(p) + " with " + std::to_string(units.size())) !=
                cmd::RejectReason::None)
                return -1;
            const int32_t id = new_status(plan, tf, StatusKind::Invade);
            statuses[id].dest = p;
            return id;
        }
        case lua::AiTfOrder::FormUnits: {
            // Form_Units 0x6cd5b0: merge (op 3) per orbit. A2 galactic: FleetManagement merge of each task-force unit
            // into the fleet of the orbit's first task-force unit (unit-wise, so other units stay where they are).
            std::map<PlanetIndex, std::vector<ObjectHandle>> by_planet;
            for (const ObjectHandle u : t.units) {
                const uint32_t f = fleet_of_unit(u);
                if (f != gal::kNoFleet && !sim.fleets()[f].moving) by_planet[sim.fleets()[f].planet].push_back(u);
            }
            for (const auto& [p, units] : by_planet) {
                if (units.size() < 2) continue;
                const uint32_t dst = fleet_of_unit(units.front());
                for (size_t i = 1; i < units.size(); ++i) {
                    const uint32_t src = fleet_of_unit(units[i]);
                    if (src == gal::kNoFleet || src == dst || dst == gal::kNoFleet || !sim.fleets()[dst].alive) continue;
                    fleet_op(t.player, gal::kFleetMerge, static_cast<uint32_t>(store.id(sim.fleets()[dst].object)),
                             static_cast<uint32_t>(store.id(sim.fleets()[src].object)), static_cast<uint32_t>(store.id(units[i])),
                             "merge " + std::to_string(store.id(units[i])) + " at " + planet_name(p));
                }
            }
            return new_status(plan, tf, StatusKind::Done);
        }
        case lua::AiTfOrder::ActivateAbility: {
            // P4 ai: GalacticTaskForce vf39 0x6cd180 -> 0x3a00b0 per member (P4_SIM_SYSTEMS Â§1.7 / Â§6.1): every member in
            // orbit (in a fleet that is not moving; landed units do not fire) tries its Ground_Activated abilities at its
            // fleet's planet, task-force order. The applies run inside GalacticSim (AI picks on the synced RNG).
            // L1P4_LUA_GAPS 1.2: with a type argument, only the members whose representative type (squadron / company
            // type, GalacticSim::representative_type) is that type (tf_activate_ability).
            std::vector<std::pair<ObjectHandle, PlanetIndex>> at;
            for (const ObjectHandle u : t.units) {
                if (activate_filter != gal::kNoIndex && sim.representative_type(u) != activate_filter) continue;
                const uint32_t f = fleet_of_unit(u);
                if (f != gal::kNoFleet && !sim.fleets()[f].moving) at.emplace_back(u, sim.fleets()[f].planet);
            }
            ground_trigger(t, at);
            return new_status(plan, tf, StatusKind::Done);
        }
        case lua::AiTfOrder::ForceTestSpaceConflict: return new_status(plan, tf, StatusKind::Done);
    }
    return -1;
}

// P4 ai: GalacticSim::ground_activated_trigger (P4_SIM_SYSTEMS Â§6.1) for each (unit, planet), in the given order. A unit
// the trigger removes (departure, vigilance, hack) is pruned from the task force by the next prune_tf.
// L1P4_LUA_GAPS 1: GalacticTaskForce.Activate_Ability([object type]): the ActivateAbility order restricted to the members
// of the named type (the binding validated the name; an unknown one here fires nothing).
int32_t GalacticAiWorld::Impl::tf_activate_ability(int32_t tf, std::string_view ability, bool, const lua::AiArg& arg) {
    activate_filter = ability.empty() ? gal::kNoIndex : sim.rules().find_type(ability);
    if (!ability.empty() && activate_filter == gal::kNoIndex) return -1;
    const int32_t id = tf_order(tf, lua::AiTfOrder::ActivateAbility, arg);
    activate_filter = gal::kNoIndex;
    return id;
}

void GalacticAiWorld::Impl::ground_trigger(TaskForceRec& t, const std::vector<std::pair<ObjectHandle, PlanetIndex>>& at) {
    for (const auto& [u, p] : at) {
        if (!store.alive(u) || !valid_planet(p)) continue;
        const std::string what = type_name(store.type(u)) + " " + std::to_string(store.id(u)) + " at " + planet_name(p);
        bool accepted = false;
        const bool applied = sim.ground_activated_trigger(u, p, &accepted);
        if (accepted) decide(t.player, "tf " + t.name + " ability " + what + (applied ? " applied" : " not applied"));
    }
    prune_tf(t);
}

// BudgetWrapper (the plan's goal struct, AI_GALACTIC_2 Â§3.4) [M where noted].
double GalacticAiWorld::Impl::budget_op(int32_t script, lua::AiBudgetOp op, double amount, std::string_view goal_type) {
    PlanInstance* p = plan_of_script(script);
    if (!p) return 0.0;
    PlayerAi* pa = player_ai(p->player);
    if (!pa) return 0.0;
    AiBudget& b = pa->ctx.budget;
    const auto a = static_cast<float>(amount);
    const AiBudget::GoalEntry* e = b.goal(p->category, p->goal_key);
    switch (op) {
        case lua::AiBudgetOp::Allocate: b.spend(p->category, p->goal_key, a); break;  // req -= n, alloc += n
        case lua::AiBudgetOp::FlushAll:  // 0x6bcdf0 (AI_GALACTIC_3 Â§8) [H]: alloc -= min(goal cost, alloc), then req = 0
            if (e) {
                const auto pit = pps.find(p->pp);
                const float cost = pit == pps.end() ? 0.0f : pit->second.net;  // goal +0x78 = the PP's net cost
                b.set_alloc(p->category, p->goal_key, e->alloc - std::min(cost, e->alloc));
                b.set_request(p->category, p->goal_key, 0.0f);
            }
            break;
        case lua::AiBudgetOp::FlushUnallocated:  // [M] the outstanding request is dropped
            b.set_request(p->category, p->goal_key, 0.0f);
            break;
        case lua::AiBudgetOp::Spendable:  // [M] allocated and covered, not yet spent
            return e ? std::max(0.0, std::min(static_cast<double>(e->alloc - e->s), static_cast<double>(sim.credits(p->player)))) : 0.0;
        case lua::AiBudgetOp::Unallocated: return e ? std::max(0.0, static_cast<double>(e->req)) : 0.0;
        case lua::AiBudgetOp::GiveToGoal:
        case lua::AiBudgetOp::TakeFromGoal: {
            const uint32_t g = defs.find_goal(goal_type);
            for (auto& [id, q] : plans)
                if (q.player == p->player && q.goal_type == g && q.target == p->target && id != script) {
                    const float d = op == lua::AiBudgetOp::GiveToGoal ? a : -a;
                    b.spend(q.category, q.goal_key, -d);
                    b.spend(p->category, p->goal_key, d);
                    break;
                }
            break;
        }
        default: break;
    }
    return 0.0;
}

// ================================================================ free store

bool GalacticAiWorld::Impl::free_store_move(int32_t script, ObjectHandle unit, const lua::AiArg& destination) {
    // Move_Object 0x68f5b0 (FIXPASS_GALACTIC §1.2) [H]. Refusals: paused / battle pending, not a free-store object, an
    // invalid destination, a unit not at a planet, in transit, or a landed transport at a blockaded planet.
    const int32_t player = script_player(script);
    if (player < 0 || !store.alive(unit) || store.owner(unit) != player || tf_owns(unit)) return false;
    if (sim.paused()) return false;
    const PlanetIndex dest = planet_arg(destination);
    if (!valid_planet(dest)) return false;
    const PlanetIndex at = planet_of_unit(unit);
    if (at == kInvalidPlanet) return false;
    if (unit_in_transit(unit)) return false;
    if (at == dest) return true;  // already there (on the surface or in a fleet orbiting it): true, nothing happens
    if (on_surface(unit) && sim.ai_blockaded(at)) return false;  // 0x3fa370 (A2GAL_READINGS 4): landed units stay
    // Launches a landed unit, splits the unit into its own fleet, then moves hop by hop (step_free_store_move).
    // A2 galactic: FleetManagement launch + form / detach. A ground company jumps as its Company_Transport_Unit
    // (STRATEGIC_GAPS §1.7); a unit that cannot jump by itself stays in the orbit fleet, which then moves with it.
    const bool jumps = unit_jumps(unit);
    if (on_surface(unit)) {
        if (!launch_and_form(player, at, unit, jumps ? kAloneFleet : gal::kNoFleet)) return false;
    } else {
        const uint32_t f = fleet_of_unit(unit);
        if (f == gal::kNoFleet || sim.fleets()[f].moving) return false;
        if (jumps && sim.fleets()[f].contents.size() > 1 &&
            fleet_op(player, gal::kFleetDetach, static_cast<uint32_t>(store.id(sim.planets()[at].object)),
                     static_cast<uint32_t>(store.id(sim.fleets()[f].object)), static_cast<uint32_t>(store.id(unit)),
                     "detach " + std::to_string(store.id(unit)) + " at " + planet_name(at)) != cmd::RejectReason::None)
            return false;
    }
    const uint32_t f = fleet_of_unit(unit);
    if (f == gal::kNoFleet || sim.fleets()[f].moving) return false;
    // The FreeStoreMovementBlockStatus (mode 0) joins the move list; its first Get_Next_Hop runs at once.
    fs_moves.push_back({unit, player, dest});
    step_free_store_move(fs_moves.back());
    if (fs_moves.back().done) fs_moves.pop_back();
    return true;
}

// 0x3f9270 (FIXPASS_GALACTIC §1.3 step 6) [H]: the starbase planet's owner, else the first orbiting fleet's owner
// [port: fleet state < 3 = an alive fleet in orbit; the transient test 0x3a9760 [L] is false for every fleet].
int32_t GalacticAiWorld::Impl::planet_orbit_owner(PlanetIndex p) const {
    if (!valid_planet(p)) return -1;
    const gal::PlanetState& ps = sim.planets()[p];
    std::vector<OrbitFleet> orbit;
    for (const uint32_t o : ps.orbit)
        if (o < sim.fleets().size() && sim.fleets()[o].alive) orbit.push_back({sim.fleets()[o].owner, false});
    const bool starbase = ps.starbase && store.alive(ps.starbase);
    const int32_t owner = ps.owner >= 0 ? ps.owner : -1;
    return orbit_owner(starbase, owner, orbit);
}

// FreeStoreMovementBlockStatus::Get_Next_Hop (0x71f710, FIXPASS_GALACTIC §1.3) [H]: the AI execution path finder's
// matrix of the last 90-frame recompute (never A*), the Friendly_Only test (every planet after the start has node cost
// 0), no fallback on failure (the unit stays in its split fleet), the orbit-owner check on path[1], then the hop
// executed directly. Re-queried at every arrival. [port] The arrival / pause signals are the execution service's poll of
// a stationary fleet (step_free_store_moves, before that service's matrix recompute).
void GalacticAiWorld::Impl::step_free_store_move(FreeStoreMove& m) {
    if (!store.alive(m.unit) || store.owner(m.unit) != m.player || tf_owns(m.unit)) {
        m.done = true;
        return;
    }
    const uint32_t f = fleet_of_unit(m.unit);
    if (f == gal::kNoFleet) {
        m.done = true;
        return;
    }
    const gal::FleetState& fs = sim.fleets()[f];
    if (fs.moving) return;  // in hyperspace: no arrival signal yet
    PlayerAi* pa = player_ai(m.player);
    if (!pa) {
        m.done = true;
        return;
    }
    const PlanetIndex cur = valid_planet(fs.planet) ? fs.planet : kInvalidPlanet;
    const FreeStoreHopResult r = free_store_next_hop(pa->paths, m.player, cur, m.dest, sim.paused(),
                                                     [this](PlanetIndex p) { return planet_orbit_owner(p); });
    switch (r.kind) {
        case FreeStoreHop::Wait: return;
        case FreeStoreHop::Arrived: m.done = true; return;
        case FreeStoreHop::NotOnPlanet:
            m.done = true;
            decide(m.player, "free store: fleet " + std::to_string(store.id(fs.object)) + " doesn't appear to be on a planet");
            return;
        case FreeStoreHop::NoPath:
            m.done = true;
            decide(m.player, "free store: Unable to find a path from " + planet_name(cur) + " to " + planet_name(m.dest));
            return;
        case FreeStoreHop::OrbitBlocked: m.done = true; return;
        case FreeStoreHop::Issue: break;
    }
    if (!move_fleet(m.player, f, r.hop)) m.done = true;
}

// Arrival / pause signals of the pending moves (issue order; the list order matters only for save / load).
void GalacticAiWorld::Impl::step_free_store_moves(int32_t player) {
    for (FreeStoreMove& m : fs_moves)
        if (m.player == player && !m.done) step_free_store_move(m);
    fs_moves.erase(std::remove_if(fs_moves.begin(), fs_moves.end(), [](const FreeStoreMove& m) { return m.done; }), fs_moves.end());
}

// Free-store unit hook 0x68fde0 step 2 (AI_GALACTIC Â§2.8) [H; port: once per execution service]: AI players land
// their free ground units that are safe, not in transit, in a stationary fleet at their own planet with a surface,
// under Max_Ground_Forces_On_Planet. A2 galactic: FleetManagement land (op 5).
void GalacticAiWorld::Impl::auto_land_free_units(int32_t player) {
    if (player < 0 || static_cast<size_t>(player) >= sim.players().size() || sim.players()[static_cast<size_t>(player)].human) return;
    const auto& rules = sim.rules();
    for (const ObjectHandle u : free_units(player)) {
        const gal::TypeInfo* ti = type_info(store.type(u));
        if (!ti || !ground_kind(ti->kind)) continue;
        const uint32_t f = fleet_of_unit(u);
        if (f == gal::kNoFleet || sim.fleets()[f].moving || unit_in_transit(u) || !unit_safe(u)) continue;
        const PlanetIndex p = sim.fleets()[f].planet;
        if (!valid_planet(p)) continue;
        const gal::PlanetState& ps = sim.planets()[p];
        if (ps.owner != player || ps.in_battle) continue;
        if (!sim.surface_accessible(p)) continue;  // 0x375220: a destroyed planet has no surface
        if (static_cast<int32_t>(ps.surface.size()) >= rules.constants.max_ground_forces_on_planet) continue;
        fleet_op(player, gal::kFleetLand, static_cast<uint32_t>(store.id(ps.object)), static_cast<uint32_t>(store.id(sim.fleets()[f].object)),
                 static_cast<uint32_t>(store.id(u)), "auto-land " + std::to_string(store.id(u)) + " at " + planet_name(p));
    }
}

int32_t GalacticAiWorld::Impl::find_target(int32_t script, const lua::AiFindTargetRequest& r) {
    int32_t player = r.source.player;
    if (player < 0 && r.source.task_force >= 0) {
        const auto it = tfs.find(r.source.task_force);
        if (it != tfs.end()) player = it->second.player;
    }
    if (player < 0 && r.source.object) player = store.owner(r.source.object);
    if (player < 0) player = script_player(script);
    PlayerAi* pa = player_ai(player);
    if (!pa) return -1;
    bool ok = true;
    const uint32_t flags = parse_application_flags(r.application, &ok);
    Reachability reach = Reachability::Any;
    if (!r.reachability.empty()) parse_reachability(r.reachability, reach);
    const int m = std::min(5, static_cast<int>(reach));
    // Reachable_Target's filter (FIXPASS_GALACTIC §1.1): the source-planet list of the level (the reachability flood
    // 0x665160 -> 0x3f12e0 under the level's request flags, cached up to 10 x FPS frames). Friendly_Only keeps only
    // planets held by the player or an ally (flag 4 clear, §1.4).
    std::vector<uint8_t> keep;
    if (!r.reachability.empty()) {
        keep.assign(sim.planets().size(), 0);
        for (const PlanetIndex k : pa->source_planets(m))
            if (k < keep.size()) keep[k] = 1;
    }
    std::vector<int32_t> cand;
    std::vector<double> value;
    for (uint32_t t = 0; t < sim.planets().size(); ++t) {  // the target list order (planet order)
        if (!applies(player, static_cast<int32_t>(t), flags)) continue;
        if (!keep.empty() && !keep[t]) continue;
        double v = 0.0;
        if (!evaluate(player, pa->ctx.opponent, r.perception, static_cast<int32_t>(t), v)) continue;
        cand.push_back(static_cast<int32_t>(t));
        value.push_back(v);
    }
    // Selection (AI_TACTICAL §6.1 steps 4-5, FIXPASS_GALACTIC §1.5): at most one synced draw, only with >= 2 positive
    // candidates and p < 1.
    const int32_t i = find_target_pick(value, r.probability, [this](float total) { return rng.synced().range_float(0.0f, total); });
    return i >= 0 ? cand[static_cast<size_t>(i)] : -1;
}

// --- L1 final (script_remaining.hpp) ---
// Add_Force(unit): a live unit of the task force's player held by no task force joins it [port].
bool GalacticAiWorld::Impl::tf_add_unit(int32_t tf, ObjectHandle unit) {
    const auto it = tfs.find(tf);
    if (it == tfs.end() || !it->second.alive || !store.alive(unit) || store.owner(unit) != it->second.player || tf_owns(unit))
        return false;
    off_free_store(unit);
    it->second.units.push_back(unit);
    it->second.had_units = true;
    return true;
}
// Refit_To_Definition(planet, budget) (AI_GALACTIC.md 2.4 [H] fill loop, L1_S4_REMAINING Â§2.12 [M]): percentage
// task-force definitions only (-3 otherwise). budget = 0: a finished status, nothing changes. budget > 0: repeat
//   1. pick a percent team by synced float roulette over its weight: the deficit max(0, pct/100 x (units + 1) - count)
//      [port: N = W/Wb x Max_Ground_Forces_On_Planet is not specified; all deficits 0 -> the percentages];
//   2. pick a type of the team uniformly with the synced int, rejecting (and retrying without) types the player
//      cannot build or with cost <= 0 [H]; a team without a buildable type gets weight 0;
//   3. stop when the budget is below the unit cost [H]; else the type joins the TF's wanted list (Produce_Force
//      builds it) and the budget drops by its cost.
// budget < 0 releases surplus [port]: units from the back of the list while their cost fits in -budget.
// Returns a Done status [port: the original's status 0x6c6500 waits for the scheduled builds].
int32_t GalacticAiWorld::Impl::tf_refit(int32_t tf, ObjectHandle planet, double budget) {
    const auto it = tfs.find(tf);
    if (it == tfs.end() || !it->second.alive) return -1;
    TaskForceRec& t = it->second;
    const auto pit = plans.find(t.plan);
    if (pit == plans.end() || pit->second.plan >= library.plans.size()) return -1;
    const uint32_t lib = pit->second.plan;
    const TaskForceDef& d = library.plans[lib].task_forces[t.def];
    if (!d.percent_based()) return -3;
    (void)planet;  // [port] the planet only selects the original's force limit (not specified)
    if (budget < 0.0) {
        double free = -budget;
        prune_tf(t);
        for (size_t i = t.units.size(); i-- > 0;) {
            const double c = static_cast<double>(cost_none(store.type(t.units[i])));
            if (c > free) break;
            free -= c;
            tf_release_unit(tf, t.units[i]);
        }
        return new_status(t.plan, tf, StatusKind::Done);
    }
    std::vector<uint32_t> count(d.teams.size(), 0);
    prune_tf(t);
    for (const ObjectHandle u : t.units) {
        const uint32_t ty = store.type(u);
        for (uint32_t k = 0; k < d.teams.size(); ++k) {
            const std::vector<uint32_t>& l = team_type_list(lib, t.def, k);
            if (std::find(l.begin(), l.end(), ty) != l.end()) {
                ++count[k];
                break;
            }
        }
    }
    for (const uint32_t ty : t.wanted)
        for (uint32_t k = 0; k < d.teams.size(); ++k) {
            const std::vector<uint32_t>& l = team_type_list(lib, t.def, k);
            if (std::find(l.begin(), l.end(), ty) != l.end()) {
                ++count[k];
                break;
            }
        }
    std::vector<bool> usable(d.teams.size(), true);
    for (uint32_t k = 0; k < d.teams.size(); ++k) usable[k] = d.teams[k].percent;
    double left = budget;
    for (int guard = 0; left > 0.0 && guard < 256; ++guard) {
        double units = 0.0;
        for (const uint32_t c : count) units += static_cast<double>(c);
        std::vector<float> w(d.teams.size(), 0.0f);
        float total = 0.0f;
        for (uint32_t k = 0; k < d.teams.size(); ++k) {
            if (!usable[k]) continue;
            const double share = static_cast<double>(d.teams[k].percent_value) / 100.0;
            w[k] = static_cast<float>(std::max(0.0, share * (units + 1.0) - static_cast<double>(count[k])));
            total += w[k];
        }
        if (total <= 0.0f)
            for (uint32_t k = 0; k < d.teams.size(); ++k) {
                w[k] = usable[k] ? d.teams[k].percent_value : 0.0f;
                total += w[k];
            }
        if (total <= 0.0f) break;
        const uint32_t team = pick_cumulative(w, rng.synced());
        if (team >= d.teams.size()) break;
        std::vector<uint32_t> cand = team_type_list(lib, t.def, team);
        uint32_t pick = kAiNone;
        while (!cand.empty()) {
            const int32_t i = rng.synced().range_int(0, static_cast<int32_t>(cand.size()) - 1);
            const uint32_t ty = cand[static_cast<size_t>(i)];
            if (galactic_ai_player_can_build(sim, t.player, ty) && cost_none(ty) > 0.0f) {
                pick = ty;
                break;
            }
            cand.erase(cand.begin() + i);
        }
        if (pick == kAiNone) {
            usable[team] = false;
            continue;
        }
        const double c = static_cast<double>(cost_none(pick));
        if (left < c) break;
        left -= c;
        t.wanted.push_back(pick);
        t.wanted_source.push_back(kInvalidPlanet);
        ++count[team];
    }
    return new_status(t.plan, tf, StatusKind::Done);
}
// FindTarget.Best_Of(tf, targets, perception) (0x697450: Human bound as FindTarget, AI_GALACTIC_TOKENS 6a) [port]: the
// target (planet of each entry) with the highest perception value > 0 for the task force's player (L1_S4_REMAINING
// Â§2.12 [H]: strict, first wins ties; failing evaluations skipped); no random draw. -1 when none qualifies.
int32_t GalacticAiWorld::Impl::best_of(int32_t script, int32_t tf, const std::vector<lua::AiArg>& targets, std::string_view perception) {
    int32_t player = script_player(script);
    if (const auto it = tfs.find(tf); it != tfs.end()) player = it->second.player;
    PlayerAi* pa = player_ai(player);
    if (!pa) return -1;
    const std::string fn(perception);
    int32_t best = -1;
    double best_v = 0.0;
    for (size_t i = 0; i < targets.size(); ++i) {
        const PlanetIndex p = planet_arg(targets[i]);
        if (!valid_planet(p)) continue;
        double v = 0.0;
        if (!evaluate(player, pa->ctx.opponent, fn, static_cast<int32_t>(p), v) || !(v > 0.0)) continue;  // Â§2.12: > 0
        if (best < 0 || v > best_v) {
            best = static_cast<int32_t>(i);
            best_v = v;
        }
    }
    return best;
}
// --- end L1 final ---

void GalacticAiWorld::Impl::service_free_store(int32_t player) {
    PlayerAi* pa = player_ai(player);
    if (!pa || !pa->fs_vm) return;
    lua::LuaVm& vm = *pa->fs_vm;
    const double t = now_seconds(sim);
    vm.set_time(t);
    vm.set_frame(static_cast<uint32_t>(sim.frame()));
    if (!lua::global_is_nil(vm, "ServiceRate")) {
        const double rate = lua::global_number(vm, "ServiceRate", 0.0);
        const double last = lua::global_number(vm, "LastService", 0.0);
        if (t - last > rate) {
            vm.service_threads();
            lua::set_number_global(vm, "LastService", t);
        }
    }
    // UnitServiceRate -> On_Unit_Service per free unit. The shipped GalacticFreeStore misspells it
    // (UnitServciceRate), so this never runs in vanilla galactic - preserved [H].
    if (!lua::global_is_nil(vm, "UnitServiceRate")) {
        const double rate = lua::global_number(vm, "UnitServiceRate", 0.0);
        const double last = lua::global_number(vm, "LastUnitService", 0.0);
        if (t - last > rate) {
            for (const ObjectHandle u : free_units(player)) {
                if (unit_reserved.count(store.id(u)) != 0) continue;  // entry +0x30 set (0x4d5a70, A1_READINGS §28.5)
                lua::AiEventArg a;
                a.kind = lua::AiEventArg::Object;
                a.object = u;
                std::string err;
                if (!lua::call_global_function(vm, "On_Unit_Service", {a}, &err) && !err.empty())
                    errors.push_back("free store On_Unit_Service: " + err);
            }
            lua::set_number_global(vm, "LastUnitService", t);
        }
    }
    drain_world_requests(world, player, -1);
    append_script_errors(vm, "free store " + std::to_string(player) + ": ", errors);  // S7 t27: with SCRIPT ERROR lines
    lua::clear_script_logs(vm);
}

// ================================================================ GalacticAiWorld

GalacticAiWorld::GalacticAiWorld(gal::GalacticSim& sim, objects::ObjectStore& store, const objects::GameObjectTypes* types,
                                 const FileProvider* files, GameRandom& rng, const AiDefinitions& defs,
                                 const EquationSet& equations, std::vector<GalacticAiTypeInfo> type_info, AiOrderSink sink,
                                 GalacticAiConfig cfg)
    : impl_(std::make_unique<Impl>(sim, store, types, files, rng, defs, equations, std::move(type_info), std::move(sink),
                                   std::move(cfg))) {
    Impl& w = *impl_;
    w.world.attach_galactic(&sim);
    w.world.attach_ai_paths(&w);  // Lua Find_Path from the AI's own scripts answers from the AI path finders
    w.world.set_perception({&equations, &w.res}, {&equations, &w.res});
    PlanScriptRuntime::VmSetup setup;
    setup.host = [&w] {
        lua::LuaHost h;
        h.store = &w.store;
        h.types = w.types;
        h.rng = &w.rng;
        h.campaign = true;
        h.files = w.files;
        return h;
    };
    setup.prepare = [&w](lua::LuaVm& vm) {
        lua::attach_player_world(vm, &w.world);
        lua::attach_ai_host(vm, &w, -1);
        lua::attach_markup_host(vm, &w);
    };
    w.runtime = std::make_unique<PlanScriptRuntime>(std::move(setup));
    w.gate.start_game();
}

GalacticAiWorld::~GalacticAiWorld() {
    if (!impl_) return;
    for (lua::LuaPlayerWorld* sw : impl_->story_worlds) sw->attach_ai_paths(nullptr);  // the story world may outlive us
    impl_->world.attach_ai_paths(nullptr);
    for (auto& [id, p] : impl_->plans) p.vm.reset();
    for (auto& a : impl_->ai)
        if (a) a->fs_vm.reset();
    impl_->scripts.clear();
    impl_->runtime.reset();
}

bool GalacticAiWorld::load_plans(const std::vector<std::string>& plan_files, std::vector<std::string>* diagnostics) {
    Impl& w = *impl_;
    if (!w.files) return false;
    return w.runtime->load(*w.files, plan_files, w.defs, w.library, diagnostics) > 0;
}

bool GalacticAiWorld::start(std::vector<std::string>* diagnostics) {
    Impl& w = *impl_;
    const auto players = w.sim.players();
    w.ai.clear();
    w.ai.resize(players.size());
    bool any = false;
    for (const GalacticAiPlayerConfig& pc : w.cfg.players) {
        if (pc.player < 0 || static_cast<size_t>(pc.player) >= players.size()) continue;
        const AiPlayerTypeDef* type = w.defs.find_player(pc.ai_type);
        if (!type) {
            if (diagnostics) diagnostics->push_back("Unrecognized AI player type " + pc.ai_type);
            continue;
        }
        auto pa = std::make_unique<PlayerAi>(&w, pc.player, type, std::clamp(pc.difficulty, 0, 2));
        float sleep = 0.0f;
        const std::string& dname = type->difficulty[static_cast<size_t>(pa->difficulty)];
        for (const GalacticAiDifficulty& d : w.cfg.difficulties)
            if (ai_iequal(d.name, dname)) {
                pa->contrast_mult = d.galactic_contrast;
                sleep = d.galactic_goal_sleep;
            }
        std::vector<GoalSource> sources;
        for (const std::string& set_name : type->function_sets) {
            const GoalFunctionSet* set = w.defs.find_set(set_name);
            if (!set) {
                if (diagnostics) diagnostics->push_back(type->name + ": unknown function set " + set_name);
                continue;
            }
            for (const GoalFunctionEntry& e : set->entries)
                if (w.defs.goals[e.goal].mode == AiMode::Galactic) sources.push_back({e.goal, e.function});
        }
        std::vector<uint32_t> templates;
        for (const std::string& t : type->templates[static_cast<size_t>(AiMode::Galactic)]) {
            const uint32_t i = w.defs.template_index(t);
            if (i == kAiNone) {
                if (diagnostics) diagnostics->push_back(type->name + ": unknown template " + t);
            } else {
                templates.push_back(i);
            }
        }
        PlayerAi& p = *pa;
        p.ctx.templates.init(w.defs, w.library, std::move(templates));
        p.ctx.budget.reset(w.defs.categories.size());
        GoalSystemConfig gcfg;
        gcfg.sleep_frames = static_cast<int32_t>(static_cast<float>(kAiFps) * sleep);
        gcfg.extra_goals_per_category = type->goal_set_extension_size;  // AIPlayerType +0xE8 (0x721b60) [H]
        p.ctx.goals.init(w.defs, std::move(sources), &p, &p.ctx.learning, &p.ctx.templates, &w.rng.synced(), gcfg);
        p.ctx.attach(&p, [&w, &p](const std::string& eq, double& out) { return w.evaluate(p.player, p.ctx.opponent, eq, -1, out); });
        if (w.cfg.stagger) {  // OPT2 t01: a fixed planning phase from the player's AI ordinal (players in index order)
            int32_t ordinal = 0;
            for (const GalacticAiPlayerConfig& o : w.cfg.players) ordinal += o.player < pc.player ? 1 : 0;
            p.ctx.set_stagger(ordinal, static_cast<int32_t>(w.cfg.players.size()));
        }
        const std::string& fs = type->free_store_script[static_cast<size_t>(AiMode::Galactic)];
        if (!fs.empty() && w.files) {
            std::vector<uint8_t> bytes;
            const std::string path = "Data\\Scripts\\FreeStore\\" + fs + ".lua";
            if (w.files->read(path, bytes)) {
                std::string err;
                p.fs_vm = w.runtime->make_vm(fs, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, &err);
                if (p.fs_vm) {
                    lua::attach_ai_host(*p.fs_vm, &w, kFreeStoreScriptBase + pc.player);
                    p.fs_vm->call("Base_Definitions", &err);
                    lua::set_free_store_global(*p.fs_vm, kFreeStoreScriptBase + pc.player, true);
                    lua::set_player_global(*p.fs_vm, "PlayerObject", pc.player);
                    lua::set_number_global(*p.fs_vm, "LastService", 0.0);
                    lua::set_number_global(*p.fs_vm, "LastUnitService", 0.0);
                    p.fs_vm->create_thread("main");
                } else if (diagnostics) {
                    diagnostics->push_back(err);
                }
            } else if (diagnostics) {
                diagnostics->push_back(path + ": not found");
            }
        }
        for (const ObjectHandle u : w.player_units(pc.player)) p.known.insert(w.store.id(u));
        for (const gal::PlanetState& ps : w.sim.planets()) {
            if (ps.starbase && w.store.alive(ps.starbase)) p.known.insert(w.store.id(ps.starbase));
            for (const ObjectHandle s : ps.structures)
                if (w.store.alive(s)) p.known.insert(w.store.id(s));
        }
        w.ai[static_cast<size_t>(pc.player)] = std::move(pa);
        any = true;
    }
    if (any && w.income_hook == 0) {  // A2 galactic: the budget's income ticks (Â§3.1)
        std::weak_ptr<int> token = w.alive;
        Impl* self = &w;
        w.income_hook = w.sim.add_income_hook([token, self](const gal::IncomeTick& t) {
            if (!token.expired()) self->on_income(t);
        });
        // The production market runs inside the planets' production service (AI_GALACTIC_3 Â§5).
        w.sim.set_production_service_hook([token, self](PlanetIndex p) {
            if (!token.expired()) self->market_planet(p);
        });
        // q2f t01: an AI player's pay-as-you-go installments go through its budget (0x53b900).
        w.sim.set_installment_hook([token, self, gs = &w.sim](PlanetIndex p, uint32_t q, const gal::BuildEntry& head, float a) {
            if (!token.expired()) return self->installment(p, q, head, a);
            return gs->players()[static_cast<size_t>(head.player)].account.credits >= a ? a : -1.0f;  // no AI any more
        });
    }
    return any;
}

void GalacticAiWorld::update_player(int32_t player, int64_t now, bool galactic_on_top, bool auto_resolve_busy) {
    Impl& w = *impl_;
    w.now = now;
    if (w.perception_frame != now) {
        ALAMO_PROFILE_SCOPE("ai.gal.frame");  // OPT2 t01: once-per-frame work (GC, snapshot, perception, story requests)
        w.perception_frame = now;
        {  // OPT2 t04: staggered scheduled Lua collections, once per frame before any script of this world runs
            std::vector<lua::LuaVm*> vms;  // free stores by player, plans by id, evaluators by name (saved orders)
            for (const auto& a : w.ai)
                if (a && a->fs_vm) vms.push_back(a->fs_vm.get());
            for (const auto& [id, pi] : w.plans)
                if (pi.vm) vms.push_back(pi.vm.get());
            for (const auto& [key, e] : w.scripts.loaded_vms())
                if (e.vm) vms.push_back(e.vm.get());
            lua::collect_scheduled(vms, static_cast<uint64_t>(now), w.cfg.lua_gc_pf);
        }
        // OPT2 t02: background path recomputes due on this galactic frame are applied before anything reads them.
        for (const auto& a : w.ai)
            if (a) a->paths.publish(static_cast<int32_t>(w.sim.frame()));
        {
            ALAMO_PROFILE_SCOPE("ai.gal.snapshot");  // OPT2 t11: perception cost, split
            w.snap = snapshot_from_sim(w.sim, w.cfg.ai_uses_fog_galactic);
        }
        {
            ALAMO_PROFILE_SCOPE("ai.gal.perception");  // OPT2 t11
            w.perc.update(w.snap, static_cast<uint32_t>(w.sim.frame()));
        }
        {
            ALAMO_PROFILE_SCOPE("ai.gal.memcopy");  // OPT2 t11
            w.mem.clear();
            for (uint32_t p = 0; p < w.snap.player_count(); ++p) w.mem.push_back(w.perc.memory(static_cast<int32_t>(p)));
            w.s2 = std::make_unique<GalacticTokenResolver>(w.snap, w.mem);
            w.res.set_s2(w.s2.get());
        }
        {
            ALAMO_PROFILE_SCOPE("ai.gal.refresh");  // OPT2 t11
            w.res.refresh(w.sim.frame());
        }
        // Story requests and Suspend_AI from the story session's world (and the AI's own).
        bool suspended = w.world.ai_suspended();
        for (lua::LuaPlayerWorld* sw : w.story_worlds) {
            w.drain_world_requests(*sw, -1, -1);
            suspended = suspended || sw->ai_suspended();
        }
        w.gate.suspend(suspended);
    }
    PlayerAi* pa = w.player_ai(player);
    if (!pa || !galactic_on_top) return;
    const bool enabled = w.gate.enabled(AiMode::Galactic) && !w.sim.paused();
    pa->ctx.service(now, enabled, auto_resolve_busy);
}

std::vector<GalacticAiWorld::TacticalBudgetRow> GalacticAiWorld::tactical_budget_rows(int32_t player) const {
    std::vector<TacticalBudgetRow> rows;
    const PlayerAi* p = impl_->player_ai(player);
    if (!p) return rows;
    for (size_t c = 0; c < p->ctx.budget.size(); ++c) {
        const auto ci = static_cast<int32_t>(c);
        rows.push_back({p->ctx.templates.tactical_allowance(ci), p->ctx.budget.available(ci), p->ctx.budget.pool(ci)});
    }
    return rows;
}

// STRATEGIC_GAPS Â§1.1 [H]: the two writers of the per-type survival table (0x58a9a0).
void GalacticAiWorld::record_object_removal(int32_t owner, uint32_t object_type, uint32_t original_type, bool survived) {
    PlayerAi* p = impl_->player_ai(owner);
    if (!p) return;  // owner without an AI (player +0x360 = 0)
    const auto& ti = impl_->tinfo;
    if (object_type >= ti.size() || !ti[object_type].unit_ai || ti[object_type].transport_behavior) return;
    p->ctx.learning.record_survival(impl_->survival_key(original_type), survived);
}

void GalacticAiWorld::record_auto_resolve_survival(int32_t owner, uint32_t type, bool survived) {
    PlayerAi* p = impl_->player_ai(owner);
    if (!p) return;
    p->ctx.learning.record_survival(impl_->survival_key(type), survived);
}

double GalacticAiWorld::survival_ratio(int32_t player, uint32_t type) const {
    const PlayerAi* p = impl_->player_ai(player);
    return p ? p->ctx.learning.survival_ratio(impl_->survival_key(type)) : 1.0;
}

void GalacticAiWorld::set_suspended(bool suspended) { impl_->gate.suspend(suspended); }
void GalacticAiWorld::set_galactic_paused(bool paused) { impl_->gate.on_galactic_pause(paused); }
void GalacticAiWorld::attach_story_world(lua::LuaPlayerWorld* world) {
    auto& v = impl_->story_worlds;
    for (lua::LuaPlayerWorld* old : v)
        if (old != world && old->ai_paths() == impl_.get()) old->attach_ai_paths(nullptr);
    v.clear();
    if (world) {
        v.push_back(world);
        world->attach_ai_paths(impl_.get());  // story Find_Path answers from the AI path finders
    }
}
void GalacticAiWorld::set_global_values(lua::GlobalValueStore* store) { impl_->gv_store = store; }  // Int-final
const PlanLibrary& GalacticAiWorld::plans() const { return impl_->library; }
bool GalacticAiWorld::is_ai_player(int32_t player) const { return impl_->player_ai(player) != nullptr; }
const AiContext* GalacticAiWorld::context(int32_t player) const {
    const PlayerAi* p = impl_->player_ai(player);
    return p ? &p->ctx : nullptr;
}
size_t GalacticAiWorld::running_plans() const { return impl_->plans.size(); }
size_t GalacticAiWorld::background_jobs_in_flight() const {
    size_t n = 0;
    for (const auto& a : impl_->ai) n += (a && a->paths.pending()) ? 1u : 0u;
    return n;
}
size_t GalacticAiWorld::running_plans(int32_t player) const {
    size_t n = 0;
    for (const auto& [id, p] : impl_->plans) n += p.player == player ? 1u : 0u;
    return n;
}
const std::vector<AiDecision>& GalacticAiWorld::decision_log() const { return impl_->log; }
std::vector<std::string> GalacticAiWorld::lua_errors() const {
    std::vector<std::string> e = impl_->errors;
    for (const auto& [id, p] : impl_->plans)
        if (p.vm) append_script_errors(*p.vm, impl_->library.plans[p.plan].name + ": ", e);  // S7 t27: with SCRIPT ERROR lines
    return e;
}
uint64_t GalacticAiWorld::orders_issued() const { return impl_->orders; }
const std::map<std::string, uint32_t>& GalacticAiWorld::proposal_stats() const {
    impl_->stats["~ evaluations"] = static_cast<uint32_t>(impl_->eval_count);
    impl_->stats["~ token cache hits"] = static_cast<uint32_t>(impl_->res.cache_hits());
    return impl_->stats;
}
uint64_t GalacticAiWorld::orders_rejected() const { return impl_->rejected; }
const GalacticSimTokenResolver& GalacticAiWorld::resolver() const { return impl_->res; }

bool GalacticAiWorld::free_store_move(int32_t player, objects::ObjectHandle unit, PlanetIndex dest) {
    if (!impl_->player_ai(player)) return false;
    lua::AiArg a;
    a.present = true;
    a.target = static_cast<int32_t>(dest);
    return impl_->free_store_move(kFreeStoreScriptBase + player, unit, a);
}

size_t GalacticAiWorld::pending_free_store_moves() const {
    size_t n = 0;
    for (const FreeStoreMove& m : impl_->fs_moves) n += m.done ? 0u : 1u;
    return n;
}

objects::ObjectHandle GalacticAiWorld::free_store_supply(int32_t player, uint32_t type) const {
    return impl_->player_ai(player) ? impl_->free_store_supply(player, type) : objects::ObjectHandle{};
}

int32_t GalacticAiWorld::free_store_owner(objects::ObjectHandle unit) const {
    const auto it = impl_->unit_reserved.find(impl_->store.id(unit));
    return it == impl_->unit_reserved.end() ? -1 : it->second;
}

void GalacticAiWorld::set_free_store_owner(objects::ObjectHandle unit, int32_t pp) {
    if (pp < 0) impl_->unit_reserved.erase(impl_->store.id(unit));
    else impl_->unit_reserved[impl_->store.id(unit)] = pp;
}

int32_t GalacticAiWorld::reachable_target(int32_t player, std::string_view perception, std::string_view application,
                                          std::string_view reachability, double probability) {
    if (!impl_->player_ai(player)) return -1;
    lua::AiFindTargetRequest r;
    r.source.present = true;
    r.source.player = player;
    r.perception = std::string(perception);
    r.application = std::string(application);
    r.reachability = std::string(reachability);
    r.probability = probability;
    return impl_->find_target(kFreeStoreScriptBase + player, r);
}

std::vector<PlanetIndex> GalacticAiWorld::reach_planets(int32_t player, int level) {
    PlayerAi* pa = impl_->player_ai(player);
    if (!pa) return {};
    return pa->source_planets(level);
}

float GalacticAiWorld::plan_production_time(int32_t running) const { return impl_->goal_production_time(running); }

std::vector<uint32_t> GalacticAiWorld::solve_potential_plan(int32_t player, std::string_view plan, std::string_view goal,
                                                           int32_t target, bool* ok, float* eta) {
    Impl& w = *impl_;
    std::vector<uint32_t> out;
    if (ok) *ok = false;
    PlayerAi* pa = w.player_ai(player);
    const uint32_t p = w.library.find(plan);
    const uint32_t g = w.defs.find_goal(goal);
    if (!pa || p == kAiNone || g == kAiNone) return out;
    PotentialPlanRec rec;
    if (!w.solve_potential_plan(*pa, p, g, target, rec)) return out;
    for (const PpSelection& s : rec.sel) out.push_back(s.type);
    if (ok) *ok = true;
    if (eta) *eta = rec.eta;
    return out;
}

void GalacticAiWorld::hash(StateHasher& h) const {
    const Impl& w = *impl_;
    h.u32(static_cast<uint32_t>(w.ai.size()));
    for (const auto& a : w.ai) {
        h.boolean(a != nullptr);
        if (a) a->ctx.hash(h);
    }
    h.u32(static_cast<uint32_t>(w.plans.size()));
    for (const auto& [id, p] : w.plans) {
        h.i32(id);
        h.i32(p.player);
        h.u32(p.plan);
        h.u32(p.goal_type);
        h.i32(p.target);
        h.boolean(p.result);
        h.boolean(p.non_removable);
        h.u32(p.live_threads);  // cached on the sim thread: hashers may run on job workers (no VM access)
    }
    h.u32(static_cast<uint32_t>(w.tfs.size()));
    for (const auto& [id, t] : w.tfs) {
        h.i32(id);
        h.i32(t.stage);
        h.u32(static_cast<uint32_t>(t.units.size()));
        for (const ObjectHandle u : t.units) h.i32(w.store.id(u));
    }
    h.u32(static_cast<uint32_t>(w.statuses.size()));
    for (const auto& [id, s] : w.statuses) {
        h.i32(id);
        h.boolean(s.finished);
        h.u8(static_cast<uint8_t>(s.result));
    }
    h.u32(static_cast<uint32_t>(w.builds.size()));
    for (const auto& [id, b] : w.builds) {
        h.i32(id);
        h.u32(b.type);
        h.i32(b.tf);
        h.u8(static_cast<uint8_t>(b.state));
        h.u32(b.bid_planet);
        h.f64(b.bid);
        h.i32(b.queue_id);
    }
    h.u32(static_cast<uint32_t>(w.pending_builds.size()));
    for (const int32_t b : w.pending_builds) h.i32(b);
    for (const auto& pq : w.planet_queues)
        for (const PlanetBuildQueue& q : pq) {
            h.i64(q.last_run);
            h.i32(q.current);
            h.i32(q.current_queue_id);
            h.u32(static_cast<uint32_t>(q.tasks.size()));
            for (const int32_t t : q.tasks) h.i32(t);
            h.u32(static_cast<uint32_t>(q.reservations.size()));
        }
    h.u32(static_cast<uint32_t>(w.unit_reserved.size()));
    for (const auto& [u, pp] : w.unit_reserved) {
        h.i32(u);
        h.i32(pp);
    }
    h.u32(static_cast<uint32_t>(w.fs_moves.size()));
    for (const FreeStoreMove& m : w.fs_moves) {
        h.i32(w.store.id(m.unit));
        h.u32(m.dest);
    }
    h.u32(static_cast<uint32_t>(w.markup.size()));
    for (const auto& [k, v] : w.markup) {
        h.i32(k.first);
        h.i32(k.second);
        h.f64(v);
    }
    h.u32(static_cast<uint32_t>(w.globals.size()));
    for (const auto& [k, v] : w.globals) {
        h.str(k);
        lua::hash_global_value(h, v);  // L1 final: same bytes for non-table values
    }
    h.u64(w.orders);
}

void GalacticAiWorld::register_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash(h); });
}

// ================================================================ AI execution path finder (GALACTIC.md Â§4)

// One recompute's inputs: Node_Weight from this world's perception (published by this frame's update), the
// Is_Planet_Revealed fog gate only for a non-human AI player (GalacticPerception::all_pairs_input, Q8 step 2) and the
// Layer_Z_Adjust of each planet's type (Q2). The spans point into perc / planet_zoff (valid for this call).
gal::AllPairsInput GalacticAiWorld::Impl::path_input(int32_t player) {
    const auto planets = sim.planets();
    if (planet_zoff.size() != planets.size()) {
        planet_zoff.assign(planets.size(), 0.0f);
        for (size_t i = 0; i < planets.size() && types; ++i) {
            const gal::PlanetState& ps = planets[i];
            if (!ps.is_planet || !store.alive(ps.object)) continue;
            const objects::TypeIndex t = store.type(ps.object);
            if (t < types->size()) planet_zoff[i] = types->get_float(t, "Layer_Z_Adjust");
        }
    }
    gal::AllPairsInput in = perc.all_pairs_input(player);
    in.z_offset = planet_zoff;
    return in;
}

AiPathFinder* GalacticAiWorld::Impl::ensure_path_finder(int32_t player) {
    PlayerAi* pa = player_ai(player);
    if (!pa) return nullptr;
    if (!pa->paths.created()) pa->paths.create_lazy(sim.graph(), path_input(player), cfg.path_pf);  // 0x524c10 [M]
    return &pa->paths;
}

// AiPathQuery (Lua Find_Path): only a CREATED finder answers; a non-AI player or one whose AI has not had its first
// service yet gets false, and the binding falls back to its own path [port: the original's lazy getter 0x524c10 would
// create the finder here; kept on GalacticAiWorld::path() only, so a Lua query never changes AI state].
bool GalacticAiWorld::Impl::ai_path(int32_t player, PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out) {
    out.clear();
    const PlayerAi* pa = player_ai(player);
    if (!pa || !pa->paths.created()) return false;
    pa->paths.get_path(from, to, out);
    return true;
}

std::vector<PlanetIndex> GalacticAiWorld::path(int32_t player, PlanetIndex from, PlanetIndex to) {
    std::vector<PlanetIndex> out;
    if (AiPathFinder* f = impl_->ensure_path_finder(player)) f->get_path(from, to, out);
    return out;
}

const AiPathFinder* GalacticAiWorld::path_finder(int32_t player) const {
    const PlayerAi* pa = impl_->player_ai(player);
    return pa ? &pa->paths : nullptr;
}

AiPathQuery& GalacticAiWorld::path_query() { return *impl_; }

void GalacticAiWorld::hash_paths(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(impl_->ai.size()));
    for (const auto& pa : impl_->ai) {
        h.boolean(pa != nullptr);
        if (pa) pa->paths.hash(h);
    }
}

void GalacticAiWorld::register_path_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash_paths(h); });
}

// ================================================================ data helpers

std::vector<GalacticAiDifficulty> load_ai_difficulties(const FileProvider& files) {
    std::vector<GalacticAiDifficulty> out;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\DifficultyAdjustments.xml", bytes)) return out;
    const data::XmlDocument doc =
        data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "DifficultyAdjustments.xml");
    if (!doc.ok()) return out;
    for (const data::XmlNode n : doc.root().children()) {
        GalacticAiDifficulty d;
        d.name = std::string(n.attribute("Name", n.name()));
        float v = 0.0f;
        if (ai_parse_float(n.child_text("Galactic_AI_Contrast_Multiplier"), v)) d.galactic_contrast = v;
        if (ai_parse_float(n.child_text("Galactic_AI_Goal_Cycle_Sleep_Duration"), v)) d.galactic_goal_sleep = v;
        out.push_back(std::move(d));
    }
    return out;
}

namespace {
bool parse_count(std::string_view s, int32_t& out) {  // a whole decimal integer (surrounding blanks allowed)
    s = ai_trim(s);
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
}  // namespace

std::vector<GalacticAiTypeInfo> make_galactic_ai_type_info(const objects::GameObjectTypes& types) {
    std::vector<GalacticAiTypeInfo> out(types.raw().type_count());
    for (size_t t = 0; t < out.size(); ++t) {
        const auto ti = static_cast<objects::TypeIndex>(t);
        GalacticAiTypeInfo& o = out[t];
        for (const std::string_view c : types.get_list(ti, "CategoryMask")) o.categories.push_back(ai_upper_copy(c));
        o.hero = types.get_bool(ti, "Is_Named_Hero");
        o.transport = o.has_category("Transport");
        o.death_clone = types.get_bool(ti, "IsDeathClone");
        // A2 galactic (AI_GALACTIC_3 Â§2, Â§3, Â§6, Â§8)
        o.create_team = types.get_bool(ti, "Create_Team");
        o.cinematic_only = types.get_bool(ti, "Cinematic_Object_Only");
        o.transport_behavior = types.behavior_count(ti, "TRANSPORT") > 0;
        o.generic_hero = types.get_bool(ti, "Is_Generic_Hero");
        o.special_weapon_space = types.get_bool(ti, "Is_Special_Weapon_In_Space");
        o.lua_script = std::string(ai_trim(types.get_string(ti, "Lua_Script")));
        o.script_on_type = types.get_bool(ti, "Execute_Script_On_Type");
        o.retains_residual_influence = types.get_bool(ti, "Planet_Retains_Residual_Influence");  // P4 ai (default true)
        for (const char* tag : {"Company_Units", "Squadron_Units"}) {
            const std::vector<objects::TypeIndex> m = types.get_type_refs(ti, tag);
            if (!m.empty() && m.front() != objects::kNone) {
                o.representative = static_cast<uint32_t>(m.front());
                break;
            }
        }
        // STRATEGIC_GAPS Â§1.1 / Â§1.2 / Â§1.5 [H]
        o.dummy_base = types.behavior_count(ti, "DUMMY_STAR_BASE") > 0 || types.behavior_count(ti, "DUMMY_GROUND_BASE") > 0;
        o.super_category = o.has_category("Super");
        o.unit_ai = types.behavior_count(ti, "UNIT_AI") > 0;
        o.stealth_company = types.get_bool(ti, "Is_Stealth_Company");
        const std::vector<objects::TypeIndex> cu = types.get_type_refs(ti, "Company_Units");
        if (!cu.empty() && cu.front() != objects::kNone) {
            o.company_first = static_cast<uint32_t>(cu.front());
            o.company_first_create_team = types.get_bool(cu.front(), "Create_Team");
        }
    }
    // Flags +0x2A / +0x29 (0x377500): set on every Company_Units / Squadron_Units member of any type.
    for (size_t t = 0; t < out.size(); ++t) {
        const auto ti = static_cast<objects::TypeIndex>(t);
        for (const objects::TypeIndex m : types.get_type_refs(ti, "Company_Units"))
            if (m != objects::kNone && static_cast<size_t>(m) < out.size()) out[static_cast<size_t>(m)].company_member = true;
        for (const objects::TypeIndex m : types.get_type_refs(ti, "Squadron_Units"))
            if (m != objects::kNone && static_cast<size_t>(m) < out.size()) out[static_cast<size_t>(m)].squadron_member = true;
    }
    // Starting_Spawned_Units_Tech_0..5: (type, count) lists, repeated tags appended (the hangar / tactical reading).
    for (size_t t = 0; t < out.size(); ++t) {
        const auto ti = static_cast<objects::TypeIndex>(t);
        for (size_t n = 0; n < kSpawnTechSlots; ++n) {
            const std::span<const std::string_view> toks = types.get_list(ti, "Starting_Spawned_Units_Tech_" + std::to_string(n));
            for (size_t i = 0; i < toks.size(); ++i) {
                std::string_view name = ai_trim(toks[i]);
                int32_t count = 0;
                const size_t cut = name.find(',') != std::string_view::npos ? name.find(',') : name.find_last_of(" \t");
                bool ok = false;
                if (cut != std::string_view::npos) {  // "Type, N" in one token
                    ok = parse_count(name.substr(cut + 1), count);
                    name = ai_trim(name.substr(0, cut));
                } else if (i + 1 < toks.size()) {  // "Type" "N" as two tokens
                    ok = parse_count(toks[i + 1], count);
                    if (ok) ++i;
                }
                if (!ok || name.empty()) continue;
                const objects::TypeIndex m = types.resolve_name(name);
                if (m != objects::kNone && static_cast<size_t>(m) < out.size())
                    out[t].spawn_tech[n].push_back({static_cast<uint32_t>(m), count});
            }
        }
    }
    fill_category_masks(out);
    return out;
}

}  // namespace alamo::ai

// Galactic PotentialPlan (docs/re/AI_GALACTIC_2.md §1, §2.1-2.2): the nine galactic marshallers on the generic
// solver core (ai_plans.hpp run_pp_solver), the ETA / source-planet choice after every commit, the free-store
// entry reservations (+0x30) and the planet build-queue reservations of commit step 4.
// [H] rules are the doc's; [M] / [port] mark the data the port does not have (see the A2 open items).
#include "galactic_ai_impl.hpp"

#include "alamo/galactic/auto_resolve.hpp"

namespace alamo::ai {

namespace {

constexpr float kVeto = kAiBigFloat;
constexpr double kInf = kPpInfinite;
constexpr float kLevelFactorK = 1.0f;  // 0x140b16c8c = GameConstants +0x148C, ctor 1.0, no XML tag (AI_GALACTIC_3 §8) [H]

struct ContrastEntry {
    int32_t row = -1;    // contrast-table row (EnemyContrastTypes index); -1 = the untyped layer total
    bool space = false;  // layer flag (entry 0 space, entry 1 ground; typed space rows then ground rows)
    double target = 0.0;
};

struct PpRec {  // rec[i] (0xB8)
    uint32_t type = gal::kNoIndex;
    int32_t free_unit = -1;      // +4 (index into the eligible free-store units)
    int32_t start_team = -1;     // +0xC
    int32_t team_slot = -1;      // +0x10
    int32_t contrast_entry = -1; // +0x14
    float prod_score = 0.0f;     // +0x18
    double prod_t = 0.0;         // Production Setup C scratch
    std::vector<int32_t> teams;  // +0x20 slot indices
    std::vector<std::pair<int32_t, float>> pairs;  // +0x98 (entry, weight)
    bool is_hero = false;        // +0x54
    bool non_escort = false, escort = false;  // +0xB0 / +0xB1
    bool space = false, ground = false;       // +0x90 / +0x91 layer flags
};

struct PpSlot {  // 0x50 B
    uint32_t tf = 0, team = 0;
    std::vector<uint32_t> assigned;  // selection indices
    float cost_sum = 0.0f;           // +0x28
    uint64_t mask = 0;               // +0x30
    bool hero_attach = true, special = true;
    PlanetIndex stage = kInvalidPlanet;
    int32_t tf_count = 0;            // +0x48
    float tf_force = 0.0f;           // +0x4C
};

struct FsUnit {
    ObjectHandle h;
    uint32_t type = gal::kNoIndex;
    double key = 0.0;
    bool consumed = false;
};

// The PotentialPlanClass scratch of one solve.
struct GalPp {
    GalacticAiWorld::Impl& w;
    PlayerAi& pa;
    const PlanDefinition& def;
    uint32_t plan;
    uint32_t goal_type;
    int32_t target;
    int32_t player;
    int32_t category;
    float time_limit;
    int reach_level;
    bool first_solve = true;  // PP+0x104 == 0 (Init at proposal): FreeStore Setup A ignores +0x30

    std::vector<PpRec> recs;
    std::map<uint32_t, uint32_t> rec_of_type;  // dedup set (keyed by type index)
    std::vector<PpSlot> slots;
    std::vector<FsUnit> units;
    double max_key = 0.0;
    std::vector<PlanetIndex> stages;  // 0x140b3a660[tf]

    // PP fields
    float avail = 0.0f, rate = 0.0f, money = 0.0f, pct_money = 0.0f, gross = 0.0f, net = 0.0f, eta = 0.0f;
    // selection (PP+0x58 / +0x78 / +0xC0 / +0x108 / +0x180)
    std::vector<uint32_t> sel_rec;
    std::vector<int32_t> sel_unit, sel_slot, sel_entry;
    std::vector<PlanetIndex> sel_source;
    std::vector<uint8_t> sel_queue;
    std::vector<float> sel_btime;
    uint64_t selected_mask = 0;  // 0x140b3a538
    bool any_hero_attach = false, any_special = false, any_escort = false;  // 0x140b3a526 / 527 / 525
    bool main_met = false, escort_met = false;                              // 0x140b3a528 / 529
    int32_t ground_queued = 0, orbital_queued = 0;                          // 0x140b3a534 / 540
    // contrast
    float tolerance = 0.0f;
    std::vector<ContrastEntry> entries;
    std::vector<double> remaining;
    bool layer_active_space = false, layer_active_ground = false;  // PP+0x1A1 / +0x1A0
    // A2 galactic (AI_GALACTIC_3): PP+0x19C terrain (§6.1; empty = -1) and the remaining population 0x140b3a530
    // (§3.5, only with AI_Uses_Galactic_Pop_Cap).
    int32_t terrain = -1;
    bool pop_cap = false;
    double remaining_pop = 0.0;
    double committed_pop = 0.0;  // PP+0x1A8: the population of the units this PP produces

    GalPp(GalacticAiWorld::Impl& world, PlayerAi& p, uint32_t pl, uint32_t g, int32_t t)
        : w(world), pa(p), def(world.library.plans[pl]), plan(pl), goal_type(g), target(t), player(p.player),
          category(world.defs.goals[g].category), time_limit(world.defs.goals[g].time_limit),
          reach_level(t >= 0 ? static_cast<int>(world.defs.goals[g].reachability) : 5) {
        // PP Init 0x6c0d00 (§6.1): the target planet type's Terrain through the terrain enum; -1 without a target.
        if (has_target()) terrain = w.sim.planet_terrain(tplanet());  // A2 galactic hook
        pop_cap = w.sim.population_capped(player);  // AI: only with AI_Uses_Galactic_Pop_Cap (§3.5)
        if (pop_cap) remaining_pop = w.ai_remaining_pop(player);
    }
    // 0x5f22c0 (§6.2): T's effectiveness on the PP terrain (GalacticSim::terrain_effectiveness; missing -> 1.0).
    double effectiveness(uint32_t t) const { return terrain < 0 ? 1.0 : w.sim.terrain_effectiveness(tplanet(), t); }
    double pop_of(uint32_t t) const { return w.pop_of(t); }
    bool pop_allows(uint32_t t) const { return !pop_cap || pop_of(t) <= remaining_pop; }  // 0x72b3e0 / 0x72b6a0 [M]

    bool has_target() const { return target >= 0 && w.valid_planet(static_cast<PlanetIndex>(target)); }
    PlanetIndex tplanet() const { return has_target() ? static_cast<PlanetIndex>(target) : kInvalidPlanet; }
    const gal::TypeInfo* ti(uint32_t t) const { return w.type_info(t); }
    bool is_ground_structure(uint32_t t) const { const auto* x = ti(t); return x && x->kind == gal::UnitKind::GroundStructure; }
    bool is_orbital_structure(uint32_t t) const { const auto* x = ti(t); return x && x->kind == gal::UnitKind::StarBase; }  // 0x60 [M]
    bool is_structure(uint32_t t) const { return is_ground_structure(t) || is_orbital_structure(t); }
    bool space_layer(uint32_t t) const { const auto* x = ti(t); return x && (space_kind(x->kind) || x->kind == gal::UnitKind::StarBase); }
    bool ground_layer(uint32_t t) const { const auto* x = ti(t); return x && (ground_kind(x->kind) || x->kind == gal::UnitKind::GroundStructure); }
    bool hero(uint32_t t) const { return t < w.tinfo.size() && w.tinfo[t].hero; }
    bool affiliated(uint32_t t) const {
        const auto* x = ti(t);
        if (!x) return false;
        if (x->affiliation.empty()) return true;
        const uint32_t f = w.sim.players()[static_cast<size_t>(player)].faction;
        return std::find(x->affiliation.begin(), x->affiliation.end(), f) != x->affiliation.end();
    }
    int32_t player_tech() const { return w.sim.players()[static_cast<size_t>(player)].tech_level; }
    const TaskForceDef& tf_def(uint32_t tf) const { return def.task_forces[tf]; }
    const TeamDef& team_def(const PpSlot& s) const { return def.task_forces[s.tf].teams[s.team]; }
    bool slot_percent(const PpSlot& s) const { return team_def(s).percent; }
    int32_t slot_max(const PpSlot& s) const { return team_def(s).max; }

    uint32_t ensure_rec(uint32_t type) {
        const auto it = rec_of_type.find(type);
        if (it != rec_of_type.end()) return it->second;
        const auto i = static_cast<uint32_t>(recs.size());
        PpRec r;
        r.type = type;
        r.space = space_layer(type);
        r.ground = ground_layer(type);
        recs.push_back(std::move(r));
        rec_of_type.emplace(type, i);
        return i;
    }
    bool free_store_holds(uint32_t t) const {
        for (const ObjectHandle u : w.free_units(player))
            if (w.store.type(u) == t) return true;
        return false;
    }
    double power_eff(uint32_t r) const {  // P = power(T) x health of the chosen free unit [port: health 1]
        return static_cast<double>(w.power_of(recs[r].type));
    }

    // ---- ETA (0x6bfcf0 -> 0x6639f0, §2.1) [H; source list M] ----
    void recompute_eta() {
        sel_source.assign(sel_rec.size(), kInvalidPlanet);
        if (reach_level > 5) {
            eta = static_cast<float>(kInf);
            return;
        }
        const auto planets = w.sim.planets();
        // Source planets: the reachability object's list for the level (0x665160, AI_GALACTIC_3 §8): the flood from
        // the player's planets in flood output order, cached 10 s.
        const std::vector<PlanetIndex> list = pa.source_planets(reach_level);
        struct Pool {
            bool used = false;
            float time = 0.0f;
        };
        std::vector<Pool> pool_a(list.size()), pool_b(list.size());
        for (size_t k = 0; k < list.size(); ++k) {
            pool_a[k].time = w.planet_queue_time(list[k], 0);
            pool_b[k].time = w.planet_queue_time(list[k], 1);
        }
        float structure_sum = 0.0f;
        const gal::FactionRules* fr = nullptr;
        const uint32_t fi = w.sim.players()[static_cast<size_t>(player)].faction;
        if (fi < w.sim.rules().factions.size()) fr = &w.sim.rules().factions[fi];
        for (size_t i = 0; i < sel_rec.size(); ++i) {
            if (sel_unit[i] >= 0) continue;  // free-store entries are nulled
            const uint32_t t = recs[sel_rec[i]].type;
            const gal::TypeInfo* x = ti(t);
            if (!x) continue;
            const bool queue_a = x->build_cost > 0 && w.sim.queue_kind(t) == gal::kGroundQueue;  // A2 galactic: queue_kind
            sel_queue[i] = queue_a ? 0 : 1;
            std::vector<Pool>& pool = queue_a ? pool_a : pool_b;
            if (is_structure(t)) {
                const float bt = w.btime_at(player, t, tplanet());
                sel_source[i] = tplanet();
                sel_btime[i] = bt;
                structure_sum += bt;
                if (!list.empty() && list[0] == tplanet()) pool[0].time += bt;
                continue;
            }
            const gal::TypeInfo* tx = ti(w.travel_type_of(t));  // a company travels as its transport (SG 1.7)
            float speed = tx ? tx->hyperspace_speed : x->hyperspace_speed;
            // x the fleet speed multiplier M (STRATEGIC_GAPS §2.1), as the fleet's departure speed.
            const float fspeed = (fr ? fr->hyperspace_speed_factor : 1.0f) * w.sim.fleet_speed_multiplier();
            int32_t best = -1;
            float best_c = 0.0f;
            for (size_t k = 0; k < list.size(); ++k) {
                const PlanetIndex p = list[k];
                const gal::PlanetState& ps = planets[p];
                const bool surface = w.sim.surface_accessible(p);  // 0x375220
                if (!surface || !w.can_build_at(player, t, p)) continue;  // 0x3fa420 + 0x2804d0
                float c = pool[k].time;
                if (has_target() && speed > 0.0f) {
                    const Vec3 a = planets[static_cast<size_t>(target)].position, b = ps.position;
                    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                    c += std::sqrt(dx * dx + dy * dy + dz * dz) / (speed * fspeed * static_cast<float>(kAiFps));
                }
                const int32_t base = x->required_ground_base == 0 ? ps.starbase_level : w.sim.ground_base_level(p);
                const int32_t req = x->required_ground_base == 0 ? x->required_star_base : x->required_ground_base;
                c *= 1.0f + static_cast<float>(base - req) * kLevelFactorK;
                if (best < 0 || c < best_c) {
                    best = static_cast<int32_t>(k);
                    best_c = c;
                }
            }
            // No source: the build task waits on the market (§2.3) and the ETA ignores it [H]. Since the Production veto
            // (A2f t02 R1, same list and test as Setup C) this happens only in a MagicPlan (spawned, no queue).
            if (best < 0) continue;
            const PlanetIndex p = list[static_cast<size_t>(best)];
            const float bt = w.btime_at(player, t, p);
            pool[static_cast<size_t>(best)].used = true;
            pool[static_cast<size_t>(best)].time += bt;
            sel_source[i] = p;
            sel_btime[i] = bt;
        }
        float e = structure_sum;
        for (const Pool& p : pool_a)
            if (p.used) e = std::max(e, p.time);
        for (const Pool& p : pool_b)
            if (p.used) e = std::max(e, p.time);
        eta = e;
    }

    // ---- contrast helpers (0x6c5de0 / 0x72ac70 / 0x6c4b20) ----
    double enemy_strength(bool space, uint64_t mask) const {
        // 0x5f2430 (galactic_ai_enemy_strength): the members-only leaf Target.EnemyForce.<layer>TotalUnnormalized, plus the
        // structures' own AI_Combat_Power in the untyped totals only; a typed row is the leaf with the row's mask [M].
        return galactic_ai_enemy_strength(w.sim, w.tinfo, static_cast<PlanetIndex>(target), player, space, mask);
    }
    float weight(uint32_t t, int32_t row) const {  // 0x5f1f90 x the terrain factor 0x5f22c0 when PP+0x19C != -1 (§6.3)
        if (!w.cfg.contrast || row < 0) return 0.0f;
        const float c = w.cfg.contrast->contrast(static_cast<size_t>(row), w.type_name(t), w.mask_of(t));
        return terrain < 0 ? c : static_cast<float>(static_cast<double>(c) * effectiveness(t));
    }
    bool layer_satisfied(bool space) const {
        double total = 0.0, residual = 0.0;
        for (size_t e = 2; e < entries.size(); ++e) {
            if (entries[e].space != space) continue;
            total += entries[e].target;
            residual += std::max(remaining[e], 0.0);
        }
        if (total <= 0.0) return true;
        return residual / total <= static_cast<double>(tolerance);
    }
    void recompute_remaining() {
        remaining.resize(entries.size());
        for (size_t e = 0; e < entries.size(); ++e) remaining[e] = entries[e].target;
        for (size_t i = 0; i < sel_rec.size(); ++i) {
            const PpRec& r = recs[sel_rec[i]];
            const double P = power_eff(sel_rec[i]);
            // 0x6bf840: the selection's weighted contribution against its chosen entry, and its power against the
            // untyped total of its layer [M].
            const int32_t e = sel_entry[i];
            if (e >= 2 && static_cast<size_t>(e) < entries.size()) remaining[static_cast<size_t>(e)] -= static_cast<double>(weight(r.type, entries[static_cast<size_t>(e)].row)) * P;
            if (!entries.empty()) {
                if (r.space) remaining[0] -= P;
                if (r.ground && entries.size() > 1) remaining[1] -= P;
            }
        }
        main_met = (!layer_active_space || layer_satisfied(true)) && (!layer_active_ground || layer_satisfied(false));
        escort_met = main_met;  // [port] one requirement list (no separate escort list)
    }

    // RequiredCategories masks
    std::vector<uint64_t> required_masks() const {
        std::vector<uint64_t> out;
        for (const std::string& r : def.required_categories) {
            uint64_t m = 0;
            for (const std::string& n : ai_split(r, "|")) m |= ai_category_bit(n);
            out.push_back(m);
        }
        return out;
    }
};

// ---------------------------------------------------------------- marshallers

class FreeStoreM final : public PpMarshaller {
public:
    explicit FreeStoreM(GalPp& p) : p_(p) {}
    void setup_a() override {  // 0x7287a0 [H]
        p_.stages.assign(p_.def.task_forces.size(), p_.tplanet());
        if (!(p_.def.allow_free_store_units && !p_.def.magic_plan)) return;
        const std::vector<int32_t> hops = p_.has_target() ? p_.w.hops_to(p_.tplanet()) : std::vector<int32_t>{};
        for (const ObjectHandle u : p_.w.free_units(p_.player)) {
            const objects::ObjectId id = p_.w.store.id(u);
            // A candidate needs PP+0x104 == 0 (the first solve) or +0x30 == 0 (A1_READINGS §28.5).
            if (!p_.first_solve && p_.w.unit_reserved.count(id) != 0) continue;
            const PlanetIndex at = p_.w.planet_of_unit(u);
            if (at == kInvalidPlanet) continue;  // moving / in transit
            const gal::TypeInfo* x = p_.ti(p_.w.store.type(u));
            if (!x || structure_kind(x->kind)) continue;
            FsUnit f;
            f.h = u;
            f.type = p_.w.store.type(u);
            if (p_.has_target()) {
                double sum = 0.0;
                bool finite = false;
                const auto& planets = p_.w.sim.planets();
                for (size_t tf = 0; tf < p_.stages.size(); ++tf) {
                    const int32_t c = at < hops.size() ? hops[at] : -1;
                    if (c >= 0) {
                        sum += static_cast<double>(c) * static_cast<double>(c);
                        finite = true;
                    }
                    const double dx = static_cast<double>(planets[at].position.x) - planets[static_cast<size_t>(p_.target)].position.x;
                    const double dy = static_cast<double>(planets[at].position.y) - planets[static_cast<size_t>(p_.target)].position.y;
                    sum += dx * dx + dy * dy;
                }
                if (finite && !p_.stages.empty()) sum /= static_cast<double>(p_.stages.size());
                f.key = sum;
            }
            p_.max_key = std::max(p_.max_key, f.key);
            p_.units.push_back(f);
        }
    }
    float score(uint32_t i) override {  // 0x728560 [H]
        PpRec& r = p_.recs[i];
        if (!(p_.hero(r.type) || (p_.def.allow_free_store_units && !p_.def.magic_plan))) return 0.0f;
        if (r.free_unit >= 0) return 1.0f;
        int32_t best = -1;
        float best_s = 0.0f;
        for (size_t u = 0; u < p_.units.size(); ++u) {
            const FsUnit& f = p_.units[u];
            if (f.consumed || f.type != r.type) continue;
            const float s = (p_.max_key != 0.0 ? static_cast<float>(1.0 - f.key / p_.max_key) : 0.0f) + 2.0f;  // +2 galactic
            if (best < 0 || s > best_s) {
                best = static_cast<int32_t>(u);
                best_s = s;
            }
        }
        if (best < 0) return 0.0f;
        r.free_unit = best;
        return best_s;
    }
    void commit(uint32_t i) override {  // 0x728fd0 [H]
        PpRec& r = p_.recs[i];
        if (r.free_unit < 0) {
            p_.sel_unit.back() = -1;
            return;
        }
        p_.sel_unit.back() = r.free_unit;
        p_.units[static_cast<size_t>(r.free_unit)].consumed = true;
        p_.net -= p_.w.cost_none(r.type);
        r.free_unit = -1;
    }

private:
    GalPp& p_;
};

class BudgetM final : public PpMarshaller {
public:
    explicit BudgetM(GalPp& p) : p_(p) {}
    void setup_a() override { p_.gross = p_.net = p_.def.additional_cost; }  // 0x7284d0
    float score(uint32_t i) override {  // 0x728330 [H]
        const PpRec& r = p_.recs[i];
        if (r.free_unit >= 0 || p_.def.magic_plan) return 0.0f;
        float c = 0.0f, available = 0.0f;
        if (p_.eta == 0.0f) {
            float t = 0.0f;
            if (!p_.has_target() || p_.is_structure(r.type)) {
                t = 0.5f * p_.w.btime_none(r.type);
                c = p_.w.cost_none(r.type);
            } else {
                c = p_.w.cost_at(p_.player, r.type, p_.tplanet());
                t = p_.w.btime_at(p_.player, r.type, p_.tplanet());
            }
            if (p_.time_limit >= 0.0f && p_.time_limit <= t) t = p_.time_limit;
            available = p_.money + t * p_.rate;
        } else {
            available = p_.money;
            c = p_.w.cost_none(r.type);
        }
        return available < c + p_.net ? kVeto : 0.0f;
    }
    void commit(uint32_t i) override {  // 0x7284f0
        if (p_.def.magic_plan) return;
        const float c = p_.w.cost_none(p_.recs[i].type);
        p_.gross += c;
        p_.net += c;
    }
    void finalize(bool& success) override {  // 0x728310
        if (p_.gross < p_.def.minimum_cost + p_.def.additional_cost) success = false;
    }

private:
    GalPp& p_;
};

class TechTreeM final : public PpMarshaller {
public:
    explicit TechTreeM(GalPp& p) : p_(p) {}
    float score(uint32_t i) override {  // 0x729190 [H]
        const PpRec& r = p_.recs[i];
        const gal::TypeInfo* x = p_.ti(r.type);
        const int32_t tech = x ? x->tech_level : 0;
        if (r.free_unit < 0 && tech > p_.player_tech()) return kVeto;
        return static_cast<float>(tech) / 5.0f * 2.0f;
    }

private:
    GalPp& p_;
};

class PlanM final : public PpMarshaller {
public:
    explicit PlanM(GalPp& p) : p_(p) {}
    bool permanent_veto() const override { return true; }
    void setup_a() override {  // 0x72a3b0 [H]
        p_.slots.clear();
        for (uint32_t tf = 0; tf < p_.def.task_forces.size(); ++tf) {
            const TaskForceDef& d = p_.def.task_forces[tf];
            p_.any_hero_attach = p_.any_hero_attach || !d.deny_hero_attach;
            p_.any_special = p_.any_special || !d.deny_special_weapon_attach;
            p_.any_escort = p_.any_escort || d.escort;
            for (uint32_t team = 0; team < d.teams.size(); ++team) {
                PpSlot s;
                s.tf = tf;
                s.team = team;
                s.hero_attach = !d.deny_hero_attach;
                s.special = !d.deny_special_weapon_attach;
                s.stage = tf < p_.stages.size() ? p_.stages[tf] : p_.tplanet();
                p_.slots.push_back(std::move(s));
            }
        }
        for (PpRec& r : p_.recs) r.teams.clear();
    }
    void setup_b() override {  // 0x729550 [H]
        for (size_t si = 0; si < p_.slots.size(); ++si) {
            const PpSlot& s = p_.slots[si];
            for (uint32_t t : p_.w.team_type_list(p_.plan, s.tf, s.team)) {
                // Behaviour 0x5C DUMMY_STAR_BASE (STRATEGIC_GAPS §1.2) = the StarBase kind here [M]; a 0x5B
                // DUMMY_GROUND_BASE variant (0x3f8c40) is not modelled [port: no team names one].
                const bool generic = p_.is_orbital_structure(t);
                if (generic) {
                    // Replaced by the planet-specific variant (0x3f8fd0 / 0x3f8c40 on the target) [M].
                    t = p_.has_target() ? p_.w.next_starbase_type(p_.player, p_.tplanet()) : gal::kNoIndex;
                    if (t == gal::kNoIndex) continue;
                }
                if (!(p_.affiliated(t) || generic || p_.free_store_holds(t))) continue;
                PpRec& r = p_.recs[p_.ensure_rec(t)];
                if (std::find(r.teams.begin(), r.teams.end(), static_cast<int32_t>(si)) == r.teams.end())
                    r.teams.push_back(static_cast<int32_t>(si));
            }
        }
    }
    void setup_c() override {  // 0x729cc0 [H]: synced int draw per candidate with >= 2 teams (rejection sampling)
        for (PpRec& r : p_.recs)
            r.start_team = r.teams.empty() ? -1 : p_.w.rng.synced().range_int(0, static_cast<int32_t>(r.teams.size()) - 1);
    }
    float score(uint32_t i) override {  // 0x72a170 [H]
        PpRec& r = p_.recs[i];
        r.team_slot = -1;
        const auto n = static_cast<int32_t>(r.teams.size());
        for (int32_t k = 0; k < n; ++k) {
            const int32_t s = r.teams[static_cast<size_t>((r.start_team + k) % n)];
            const PpSlot& slot = p_.slots[static_cast<size_t>(s)];
            if (p_.slot_percent(slot) || static_cast<int32_t>(slot.assigned.size()) < p_.slot_max(slot)) {
                r.team_slot = s;
                break;
            }
        }
        for (const int32_t s : r.teams) {
            const PpSlot& slot = p_.slots[static_cast<size_t>(s)];
            const TeamDef& td = p_.team_def(slot);
            if (!td.percent) {
                if (static_cast<int32_t>(slot.assigned.size()) < td.max)
                    return 1.0f - static_cast<float>(slot.assigned.size()) / static_cast<float>(td.max);
            } else {
                if (!p_.def.ignore_target) return 1.0f;
                if (td.percent_value / 100.0f * p_.pct_money > p_.w.cost_none(r.type) + slot.cost_sum) return 1.0f;
            }
        }
        if (p_.any_hero_attach && r.is_hero) return 0.0f;
        return kVeto;
    }
    bool place(uint32_t sel, uint32_t rec, bool respect_met) {
        const PpRec& r = p_.recs[rec];
        const auto n = static_cast<int32_t>(r.teams.size());
        for (int32_t k = 0; k < n; ++k) {
            const int32_t s = r.teams[static_cast<size_t>((std::max(r.start_team, 0) + k) % n)];
            PpSlot& slot = p_.slots[static_cast<size_t>(s)];
            const bool escort = p_.tf_def(slot.tf).escort;
            if (respect_met && ((escort && p_.escort_met) || (!escort && p_.main_met))) continue;
            if (!(p_.slot_percent(slot) || static_cast<int32_t>(slot.assigned.size()) < p_.slot_max(slot))) continue;
            slot.assigned.push_back(sel);
            slot.cost_sum += p_.w.cost_none(r.type);
            slot.mask |= p_.w.mask_of(r.type);
            for (PpSlot& o : p_.slots)
                if (o.tf == slot.tf) {
                    ++o.tf_count;
                    o.tf_force += p_.w.power_of(r.type);
                }
            p_.sel_slot[sel] = s;
            return true;
        }
        return false;
    }
    void commit(uint32_t i) override {  // 0x72a720 -> 0x729830 [H]
        const auto sel = static_cast<uint32_t>(p_.sel_rec.size() - 1);
        p_.selected_mask |= p_.w.mask_of(p_.recs[i].type);
        if (!place(sel, i, true)) place(sel, i, false);
    }
    void finalize(bool& success) override {  // 0x72a060 [H]
        bool required_tf = false;
        for (const TaskForceDef& d : p_.def.task_forces) required_tf = required_tf || d.required;
        if (p_.sel_rec.empty() && !required_tf) success = false;
        for (const PpSlot& s : p_.slots) {
            const TeamDef& td = p_.team_def(s);
            const TaskForceDef& d = p_.tf_def(s.tf);
            if (!td.percent && static_cast<int32_t>(s.assigned.size()) < td.min) success = false;
            if (s.tf_force < d.minimum_total_force) success = false;
            if (s.tf_count < d.minimum_total_size) success = false;
        }
        for (const uint64_t m : p_.required_masks())
            if ((m & p_.selected_mask) == 0) success = false;
    }

private:
    GalPp& p_;
};

class ProductionM final : public PpMarshaller {
public:
    explicit ProductionM(GalPp& p) : p_(p) {}
    bool permanent_veto() const override { return true; }
    void setup_c() override {  // 0x725c50 [H]
        for (PpRec& r : p_.recs) {
            const uint32_t t = r.type;
            if (!(p_.has_target() && p_.is_structure(t))) {
                // "travel estimate" 0x664210(target reachability, T, level) (AI_GALACTIC_3 §8) [H]: not a travel time
                // but the mean, over the level's source planets (0x665160) with a usable surface and a production
                // queue that can build T, of btime(T, planet) + that planet's queue time; no planet -> 1e18.
                double sum = 0.0;
                int32_t n = 0;
                const uint32_t q = p_.w.sim.queue_kind(t) == gal::kGroundQueue ? 0u : 1u;
                for (const PlanetIndex k : p_.pa.source_planets(std::min(p_.reach_level, 5))) {
                    const bool surface = p_.w.sim.surface_accessible(k);  // 0x375220
                    if (!surface || !p_.w.can_build_at(p_.player, t, k)) continue;
                    sum += static_cast<double>(p_.w.btime_at(p_.player, t, k)) + static_cast<double>(p_.w.planet_queue_time(k, q));
                    ++n;
                }
                const double est = n > 0 ? sum / static_cast<double>(n) : kInf;
                r.prod_t = est >= kInf ? kInf : est - static_cast<double>(p_.w.btime_none(t));
            } else {
                r.prod_t = p_.w.can_build_at(p_.player, t, p_.tplanet()) ? -static_cast<double>(p_.w.btime_none(t)) : kInf;
            }
        }
        double n = 0.0, sum = 0.0, m = 0.0, M = 0.0;
        bool first = true;
        for (const PpRec& r : p_.recs) {
            if (r.prod_t >= kInf) continue;
            n += 1.0;
            sum += r.prod_t;
            m = first ? r.prod_t : std::min(m, r.prod_t);
            M = first ? r.prod_t : std::max(M, r.prod_t);
            first = false;
        }
        double spread = 0.0, avg_norm = 0.0, k = 0.0;
        if (M > 0.0 && n > 0.0) {
            spread = M - m;
            if (std::fabs(spread) < 1e-4) spread = 0.0;
            if (spread != 0.0) avg_norm = (sum / n - m) / spread;
            k = std::min(spread / 120.0, 2.0);
        }
        for (PpRec& r : p_.recs)
            r.prod_score = spread == 0.0 ? 0.0f
                                         : static_cast<float>(std::clamp(-((r.prod_t - m) / spread - avg_norm) * k / 2.0, -0.5, 0.5));
    }
    float score(uint32_t i) override {  // 0x726010 [H]
        PpRec& r = p_.recs[i];
        if (r.free_unit >= 0) return 0.5f;
        if (p_.is_ground_structure(r.type)) {
            if (!p_.has_target()) return kVeto;
            const PlanetIndex tp = p_.tplanet();
            const gal::PlanetState& ps = p_.w.sim.planets()[tp];
            if (!p_.w.sim.surface_accessible(tp)) return kVeto;  // 0x375220
            // Per-planet cap (type +0x87C Build_Max_Instances_Per_Planet, when >= 0): the instances on the planet
            // (0x3f94f0) [H].
            if (const gal::TypeInfo* x = p_.ti(r.type); x && x->max_instances_per_planet >= 0) {
                int32_t here = 0;
                for (const ObjectHandle s : ps.structures)
                    here += p_.w.store.alive(s) && p_.w.store.type(s) == r.type ? 1 : 0;
                if (here >= x->max_instances_per_planet) return kVeto;
            }
            // Used ground slots 0x3f8af0 + this PP's queued ground structures (0x140b3a534) < the slot max 0x372740
            // (Special_Structures_Land; galactic_slots.hpp) [H].
            const int32_t slots = p_.w.ground_slot_max(tp);
            if (slots >= 0 && p_.w.ground_slots_used(tp) + p_.ground_queued >= slots) return kVeto;
        } else if (p_.is_orbital_structure(r.type)) {
            if (!p_.has_target()) return kVeto;
            const gal::PlanetState& ps = p_.w.sim.planets()[static_cast<size_t>(p_.target)];
            const auto& ms = p_.w.sim.map().planets.max_space_base;
            const int32_t slots = static_cast<size_t>(p_.target) < ms.size() && ms[static_cast<size_t>(p_.target)] >= 0
                                      ? ms[static_cast<size_t>(p_.target)] : 5;  // 0x372760 [M]
            if (ps.starbase_level + p_.orbital_queued >= slots) return kVeto;
        }
        // Production-disabled list P +0x1E0 (story DISABLE_BUILDABLE, query 0x282510; REMAINING_GAPS §1.5): read by AI
        // production. AI_GALACTIC_2 lists it in the MagicPlan veto only [H]; [port] it vetoes produced candidates of
        // every plan, so the AI never queues a type the story switched off (OI2 ai).
        if (p_.w.sim.production_disabled(p_.player, r.type)) return kVeto;
        if (!p_.def.magic_plan) {
            // [port] A2f t02 R1: a produced candidate no source planet can build (Setup C t = 1e18: no planet of the
            // level's list passes 0x2804d0, or a structure 0x2804d0 refuses at the target) is vetoed (permanent). AI_GALACTIC_2
            // §1.4 lists no such veto (it would score 0 / -0.5); the port vetoes it so a plan never waits on the market for
            // a type nobody can build (zero cost, Build_Initially_Locked never unlocked, Required_Special_Structures missing).
            if (r.prod_t >= kInf) return kVeto;
            // AI_GALACTIC_3 §3.4-3.5 [H]: with AI_Uses_Galactic_Pop_Cap (off in shipped data) a unit whose pop exceeds
            // the remaining population is vetoed (permanent: Production erases it).
            if (p_.pop_cap && p_.pop_of(r.type) > p_.remaining_pop) r.prod_score = kVeto;
            return r.prod_score;
        }
        // MagicPlan (galactic): not spawnable -> veto; else one synced float in [0, 1] (0x5324d0).
        const gal::TypeInfo* x = p_.ti(r.type);
        // Not spawnable (AI_GALACTIC_2 §1.4 list; AI_GALACTIC_3 §4.1) [H]: a non-hero with cost < 1, a transport, tech > 5,
        // a hero the player already has; structures (0x5F / 0x60) are spawnable; flags +0x2A / +0x29 (company / squadron
        // members), 0x375270 ("Super" category) and generic 0x5C / 0x5B types (STRATEGIC_GAPS §1.2, §1.5) [H]. [port] Not
        // modelled: unique / instance counts (+0xF60 / +0xF80).
        if (!x || (!p_.hero(r.type) && x->build_cost < 1) || x->tech_level > 5) return kVeto;
        // The player's locks [H]: locked list 0x282550, and Build_Initially_Locked (+0xC1) without an unlock 0x282580
        // (GalacticSim::build_locked); the production-disabled list 0x282510 is tested above.
        if (p_.w.sim.build_locked(p_.player, r.type)) return kVeto;
        if (r.type < p_.w.tinfo.size()) {
            const GalacticAiTypeInfo& a = p_.w.tinfo[r.type];
            if (a.transport || a.transport_behavior || a.company_member || a.squadron_member || a.super_category || a.dummy_base)
                return kVeto;
        }
        if (p_.hero(r.type))
            for (const ObjectHandle u : p_.w.player_units(p_.player))
                if (p_.w.store.type(u) == r.type) return kVeto;
        return p_.w.rng.synced().inline_unit_float();
    }
    void commit(uint32_t i) override {  // 0x726390
        const PpRec& r = p_.recs[i];
        if (p_.sel_unit.back() >= 0) return;
        if (p_.pop_cap) {  // remaining pop -= pop(T) for a produced unit (§1.4 Commit 0x726390)
            p_.remaining_pop -= p_.pop_of(r.type);
            p_.committed_pop += p_.pop_of(r.type);
        }
        if (p_.is_ground_structure(r.type)) ++p_.ground_queued;
        if (p_.is_orbital_structure(r.type)) ++p_.orbital_queued;
    }

private:
    GalPp& p_;
};

// Hero marshaller (AI_GALACTIC_3 §2) [H]. Hero records need an AI script; their Commit's escort accumulation is
// dead code in the shipped binary (§2.4), so rec+0x40 stays 0 and a hero record never scores finite in a non-magic
// plan: it is vetoed (non-permanent) every iteration. Heroes therefore join plans only as plain units (every TF
// DenyHeroAttach, or no script / record) or through MagicPlans with a team. [port] The record's Lua calls
// (PlayerObject, MinPlanAttachCost, MaxPlanAttachCost, Evaluate_Attack_Ability, Get_Escort_Ability_Weights) are not
// run: none of their values can change a score while rec+0x40 is 0, and the hero object scripts are not hosted.
class HeroM final : public PpMarshaller {
public:
    explicit HeroM(GalPp& p) : p_(p) {}
    void setup_b() override {  // 0x726410: extra hero candidates
        if (!(p_.any_hero_attach || p_.any_special)) return;
        for (const ObjectHandle u : p_.w.free_units(p_.player)) {  // free-store heroes / special weapons
            const uint32_t t = p_.w.store.type(u);
            const uint32_t h = rep(t);
            if ((named(h) || special(h)) && allowed(h)) p_.ensure_rec(t);
        }
        // (pool entries: tactical only) commander teams: Field_Com_* (not space), Fleet_Com_* (not land) -> both in
        // galactic, each when affiliated with the player's faction.
        if (p_.any_hero_attach)
            for (const char* n : {"Field_Com_Rebel_Team", "Field_Com_Empire_Team", "Fleet_Com_Rebel_Team", "Fleet_Com_Empire_Team"}) {
                const uint32_t t = p_.w.sim.rules().find_type(n);
                if (t != gal::kNoIndex && p_.affiliated(t)) p_.ensure_rec(t);
            }
    }
    void setup_c() override {  // 0x726710: which candidates are hero records
        for (PpRec& r : p_.recs) {
            r.is_hero = false;
            const uint32_t h = rep(r.type);
            if (!(named(h) || generic(h) || special(h))) continue;  // (+ behaviour 0x37: land mode only)
            if (!allowed(h)) continue;
            // A unit: the first eligible free-store unit of the type (galactic: no pool); none -> the type itself.
            bool unit = false;
            for (const FsUnit& f : p_.units) unit = unit || f.type == r.type;
            // The script: the unit's per-object script (its type's or representative's Lua_Script [M]), else the type
            // instance, which exists only with Execute_Script_On_Type.
            const GalacticAiTypeInfo* a = info(r.type);
            const GalacticAiTypeInfo* b = info(h);
            const bool script = unit ? ((a && !a->lua_script.empty()) || (b && !b->lua_script.empty()))
                                     : ((a && !a->lua_script.empty() && a->script_on_type) || (b && !b->lua_script.empty() && b->script_on_type));
            if (!script) continue;
            r.is_hero = true;
        }
    }
    float score(uint32_t i) override {  // 0x726f40
        const PpRec& r = p_.recs[i];
        if (!r.is_hero) return 0.0f;
        if (r.teams.empty() || !p_.def.magic_plan) {
            if (!allowed(rep(r.type))) return 0.0f;
            // (attached free unit -> 1e18; galactic units are never attached in the port.) rec+0x40 == 0 (§2.4): the
            // finite branch needs rec+0x40 > 0, so the record is vetoed.
            return kVeto;
        }
        // MagicPlan with a team, no free unit: one magic plan per hero.
        for (const uint32_t s : p_.sel_rec)
            if (p_.recs[s].type == r.type) return kVeto;
        for (const auto& [id, pi] : p_.w.plans) {
            if (pi.player != p_.player || pi.ended || pi.plan >= p_.w.library.plans.size() || !p_.w.library.plans[pi.plan].magic_plan) continue;
            for (const int32_t tf : pi.tfs) {
                const auto it = p_.w.tfs.find(tf);
                if (it == p_.w.tfs.end()) continue;
                for (const ObjectHandle u : it->second.units)
                    if (p_.w.store.alive(u) && p_.w.store.type(u) == r.type) return kVeto;
                for (const uint32_t wt : it->second.wanted)
                    if (wt == r.type) return kVeto;
            }
        }
        return 0.0f;
    }

private:
    const GalacticAiTypeInfo* info(uint32_t t) const { return t < p_.w.tinfo.size() ? &p_.w.tinfo[t] : nullptr; }
    uint32_t rep(uint32_t t) const {  // 0x6c00c0
        const GalacticAiTypeInfo* x = info(t);
        return x && x->representative < p_.w.tinfo.size() ? x->representative : t;
    }
    bool named(uint32_t t) const { const auto* x = info(t); return x && x->hero; }
    bool generic(uint32_t t) const { const auto* x = info(t); return x && x->generic_hero; }
    bool special(uint32_t t) const { const auto* x = info(t); return x && x->special_weapon_space; }
    bool allowed(uint32_t h) const {  // §2.1 item 2
        return (p_.any_hero_attach || !(named(h) || generic(h))) && (p_.any_special || !special(h));
    }
    GalPp& p_;
};

// TerrainType marshaller 0x727c40 (AI_GALACTIC_3 §6.3) [H]: on a target planet, veto a type the planet type restricts
// (Planet_Restricted_Unit_Categories overlap or Planet_Restricted_Unit_Types); otherwise 0 with PP+0x19C = -1, else
// the terrain effectiveness (1.0 everywhere in shipped data).
class TerrainM final : public PpMarshaller {
public:
    explicit TerrainM(GalPp& p) : p_(p) {}
    float score(uint32_t i) override {
        if (!p_.has_target()) return 0.0f;
        const uint32_t t = p_.recs[i].type;
        if (p_.w.sim.planet_restricts(p_.tplanet(), t)) return kVeto;  // 0x373490 (A2 galactic hook)
        return p_.terrain < 0 ? 0.0f : static_cast<float>(p_.effectiveness(t));
    }

private:
    GalPp& p_;
};

class SurvivalM final : public PpMarshaller {
public:
    explicit SurvivalM(GalPp& p) : p_(p) {}
    // 0x727cd0 -> 0x588a90 (STRATEGIC_GAPS §1.1) [H]: the AI player's per-type survival ratio (type-name CRC key, company
    // leader substitution), 1.0 when the type was never recorded. Fed through GalacticAiWorld::record_object_removal /
    // record_auto_resolve_survival.
    float score(uint32_t i) override {
        return static_cast<float>(p_.pa.ctx.learning.survival_ratio(p_.w.survival_key(p_.recs[i].type)));
    }

private:
    GalPp& p_;
};

class ContrastM final : public PpMarshaller {
public:
    explicit ContrastM(GalPp& p) : p_(p) {}
    void setup_a() override {  // 0x72bee0 -> 0x6c5de0 [H]
        p_.entries.clear();
        p_.tolerance = 0.0f;
        bool percent = false;
        for (const TaskForceDef& d : p_.def.task_forces) percent = percent || d.percent_based();
        if (percent)  // PP+0x3C: the money a percent TF may spend
            p_.pct_money = std::max(p_.def.minimum_cost + p_.def.additional_cost,
                                    p_.avail + std::max(p_.time_limit, 0.0f) * p_.rate);
        if (!p_.has_target() || p_.def.ignore_target) {
            p_.remaining.clear();
            return;
        }
        const int32_t f = p_.pa.ctx.learning.recent_plan_failures(p_.plan, p_.target, p_.w.now);
        const float fp = static_cast<float>(f) * p_.def.per_failure_contrast_adjust;
        p_.tolerance = 1.0f - (fp + p_.def.min_contrast_scale) / (fp + p_.def.max_contrast_scale);
        const double scale = static_cast<double>(p_.def.max_contrast_scale + fp) * static_cast<double>(p_.pa.contrast_mult);
        const double space_total = p_.enemy_strength(true, ~uint64_t{0});
        const double ground_total = p_.enemy_strength(false, ~uint64_t{0});
        p_.entries.push_back({-1, true, space_total});
        p_.entries.push_back({-1, false, ground_total});
        const size_t rows = p_.w.cfg.contrast ? p_.w.cfg.contrast->size() : 0;
        for (const bool space : {true, false})
            for (size_t j = 0; j < rows; ++j) {
                const double total = space ? space_total : ground_total;
                const double v = total > 0.0 ? p_.enemy_strength(space, p_.w.cfg.contrast->rows[j].enemy_mask) * scale : 0.0;
                p_.entries.push_back({static_cast<int32_t>(j), space, v});
            }
        p_.remaining.resize(p_.entries.size());
        for (size_t e = 0; e < p_.entries.size(); ++e) p_.remaining[e] = p_.entries[e].target;
    }
    void setup_c() override {  // 0x72aad0 [H]
        if (p_.def.ignore_target || p_.entries.empty()) return;
        for (PpRec& r : p_.recs) {
            for (const int32_t s : r.teams) {
                const bool esc = p_.tf_def(p_.slots[static_cast<size_t>(s)].tf).escort;
                r.escort = r.escort || esc;
                r.non_escort = r.non_escort || !esc;
            }
            r.pairs.clear();
            for (size_t e = 2; e < p_.entries.size(); ++e) {
                if (!(p_.remaining[e] > 0.0)) continue;
                const float wgt = p_.weight(r.type, p_.entries[e].row);
                r.pairs.emplace_back(static_cast<int32_t>(e), wgt);
                if (wgt > 0.0f) {
                    if (p_.entries[e].space) p_.layer_active_space = true;
                    else p_.layer_active_ground = true;
                }
            }
        }
        p_.recompute_remaining();
    }
    float score(uint32_t i) override {  // 0x72b940 [H]
        PpRec& r = p_.recs[i];
        if (p_.def.ignore_target) return p_.w.rng.synced().inline_range_float(0.0f, p_.w.power_of(r.type) / 500.0f);
        if (r.is_hero || p_.entries.empty()) return 0.0f;
        double v = -1.0;
        r.contrast_entry = -1;
        const double P = p_.power_eff(i);
        const bool free = r.free_unit >= 0;
        for (const auto& [e, wgt] : r.pairs) {
            const auto ei = static_cast<size_t>(e);
            const double R = p_.remaining[ei];
            if (!(free || (p_.entries[ei].target * static_cast<double>(p_.tolerance) < R && p_.pop_allows(r.type)))) continue;
            const double wp = static_cast<double>(wgt) * P;
            const double d = std::max(R - wp, 0.0), m = std::max(R, wp);
            const double s = m > 0.0 ? (1.0 - (d / m) * (d / m)) * static_cast<double>(wgt) : 0.0;
            if (s > v) {
                v = s;
                r.contrast_entry = e;
            }
        }
        if (v > 0.0) return static_cast<float>(v);
        const bool layer_ok = (!r.space || p_.layer_satisfied(true)) && (!r.ground || p_.layer_satisfied(false));
        if (!layer_ok) return kVeto;
        if (p_.def.minimum_cost + p_.def.additional_cost > p_.gross) return 0.0f;
        if ((r.space && p_.remaining.size() > 0 && p_.remaining[0] > 0.0) || (r.ground && p_.remaining.size() > 1 && p_.remaining[1] > 0.0))
            return 0.0f;
        for (const uint64_t m : p_.required_masks())
            if ((m & p_.selected_mask) == 0 && (m & p_.w.mask_of(r.type)) != 0) return 0.0f;
        for (const int32_t s : r.teams) {
            const PpSlot& slot = p_.slots[static_cast<size_t>(s)];
            const TeamDef& td = p_.team_def(slot);
            const TaskForceDef& d = p_.tf_def(slot.tf);
            if ((!td.percent && static_cast<int32_t>(slot.assigned.size()) < td.min) || slot.tf_force < d.minimum_total_force ||
                slot.tf_count < d.minimum_total_size)
                return 0.0f;
        }
        return kVeto;  // what normally ends the solve
    }
    void commit(uint32_t) override {  // 0x72bf80 -> 0x6c4b20 [H]
        if (p_.def.ignore_target || p_.entries.empty()) return;
        p_.recompute_remaining();
    }
    void finalize(bool& success) override {  // 0x72af80 [H]
        if (p_.def.ignore_target || p_.entries.empty()) return;
        for (const bool space : {true, false}) {
            if (!(space ? p_.layer_active_space : p_.layer_active_ground)) continue;
            const size_t e = space ? 0 : 1;
            if (p_.entries[e].target > 0.0 && p_.remaining[e] / p_.entries[e].target > static_cast<double>(p_.tolerance)) success = false;
            if (!p_.layer_satisfied(space)) success = false;
        }
    }

private:
    GalPp& p_;
};

}  // namespace

// ================================================================ Impl: team expansion, solve, reservations

// Team spec lhs (0x66fcc0, §1.3) [H]: a category mask when every name is a GameObjectCategoryType name (types in
// type order: flags +0x67 / +0x29 / +0x21 clear, no behaviour 0x5C / 0x5B, mask overlap, the TF unit filter); else a
// '|' list of type names. Excluded types (-Name) are removed.
const std::vector<uint32_t>& GalacticAiWorld::Impl::team_type_list(uint32_t plan, uint32_t tf, uint32_t team) {
    const auto key = std::make_tuple(plan, tf, team);
    const auto it = team_types.find(key);
    if (it != team_types.end()) return it->second;
    const TaskForceDef& d = library.plans[plan].task_forces[tf];
    const TeamDef& t = d.teams[team];
    const auto& rules = sim.rules();
    std::vector<uint32_t> out;
    uint64_t mask = 0;
    bool is_mask = !t.names.empty();
    for (const std::string& n : t.names) {
        const uint64_t b = ai_category_bit(n);
        if (b == 0 && !ai_iequal(n, "None")) is_mask = false;
        mask |= b;
    }
    uint64_t filter = 0;
    for (const std::string& n : d.masks) filter |= ai_category_bit(n);
    if (is_mask) {
        for (size_t ty = 0; ty < rules.types.size(); ++ty) {
            const gal::TypeInfo& x = rules.types[ty];
            const bool unit_like = x.kind == gal::UnitKind::SpaceUnit || x.kind == gal::UnitKind::Squadron ||
                                   x.kind == gal::UnitKind::GroundUnit || x.kind == gal::UnitKind::GroundStructure;
            const bool clone = ty < tinfo.size() && tinfo[ty].death_clone;
            // AI_GALACTIC_2 §1.3 + STRATEGIC_GAPS §1.2 [H]: flags +0x67 Create_Team, +0x29 squadron member, +0x21
            // Cinematic_Object_Only clear, and no behaviour 0x5C / 0x5B (DUMMY_STAR_BASE / DUMMY_GROUND_BASE).
            const bool flagged = ty < tinfo.size() && (tinfo[ty].create_team || tinfo[ty].squadron_member ||
                                                       tinfo[ty].cinematic_only || tinfo[ty].dummy_base);
            // [port] kept on top: the galactic unit kinds the port's sim produces, no IsDeathClone, no TRANSPORT
            // behaviour (a free-store transport is a company's carrier in the port, not a plan unit). The former
            // "cost > 0 unless a hero" rule (no such rule in §1.3) is gone since A2f t02: zero-cost types are listed as
            // in the original; the Production veto on Setup C t = 1e18 (R1) keeps a produced one out of the plan, a
            // free-store one may still join.
            const bool port_out = ty < tinfo.size() && tinfo[ty].transport_behavior;
            if (!unit_like || clone || flagged || port_out) continue;
            const uint64_t m = mask_of(static_cast<uint32_t>(ty));
            if ((mask & m) == 0) continue;
            if (filter != 0 && (filter & m) != filter) continue;
            out.push_back(static_cast<uint32_t>(ty));
        }
    } else {
        for (const std::string& n : t.names) {
            const uint32_t ty = rules.find_type(n);
            // flags +0x67 / +0x29 / +0x21 clear [H] (+ TRANSPORT behaviour [port], see above)
            if (ty == gal::kNoIndex || (ty < tinfo.size() && (tinfo[ty].create_team || tinfo[ty].squadron_member ||
                                                             tinfo[ty].cinematic_only || tinfo[ty].transport_behavior)))
                continue;
            if (std::find(out.begin(), out.end(), ty) == out.end()) out.push_back(ty);
        }
    }
    for (const std::string& x : d.excluded_types) {
        const uint32_t ty = rules.find_type(x);
        out.erase(std::remove(out.begin(), out.end(), ty), out.end());
    }
    return team_types.emplace(key, std::move(out)).first->second;
}

bool GalacticAiWorld::Impl::solve_potential_plan(PlayerAi& pa, uint32_t plan, uint32_t goal_type, int32_t target,
                                                 PotentialPlanRec& out, bool first_solve) {
    GalPp pp(*this, pa, plan, goal_type, target);
    pp.first_solve = first_solve;
    // Reset 0x6c3080: PP+0x30 = category available, +0x34 = its rate, +0x38 = +0x30.
    pp.avail = pa.ctx.budget.available(pp.category);
    pp.rate = pa.ctx.budget.rate(pp.category);
    pp.money = pp.avail;
    FreeStoreM fs(pp);
    BudgetM budget(pp);
    TechTreeM tech(pp);
    PlanM planm(pp);
    ProductionM prod(pp);
    HeroM hero(pp);
    TerrainM terrain(pp);
    ContrastM contrast(pp);
    SurvivalM survival(pp);
    PpMarshaller* ms[] = {&fs, &budget, &tech, &planm, &prod, &hero, &terrain, &contrast, &survival};  // §1.1 galactic order
    PpSolverIo io;
    io.candidate_count = [&pp] { return pp.recs.size(); };
    io.type_of = [&pp](uint32_t c) { return pp.recs[c].type; };
    io.on_select = [&pp](uint32_t c) {
        pp.sel_rec.push_back(c);
        pp.sel_unit.push_back(-1);
        pp.sel_slot.push_back(pp.recs[c].team_slot);
        pp.sel_entry.push_back(pp.recs[c].contrast_entry);
        pp.sel_source.push_back(kInvalidPlanet);
        pp.sel_queue.push_back(1);
        pp.sel_btime.push_back(0.0f);
    };
    io.after_commit = [&pp] {
        pp.recompute_eta();
        if (pp.eta > 0.0f) {
            float horizon = pp.eta;
            if (pp.time_limit >= 0.0f && pp.time_limit <= pp.eta) horizon = pp.time_limit;
            pp.money = horizon * pp.rate + pp.avail;
        }
    };
    static constexpr const char* kNames[] = {"FreeStore", "Budget", "TechTree", "Plan", "Production", "Hero", "TerrainType", "Contrast", "Survival"};
    size_t last_veto = 99;
    io.on_veto = [&last_veto](uint32_t, size_t m) { last_veto = m; };
    std::vector<uint32_t> selection;
    const bool ok = run_pp_solver(ms, io, selection, cfg.solver_max_picks);
    const PlanDefinition& def = library.plans[plan];
    if (!ok) {  // port diagnostics (never hashed; the Finalize checks are pure, so re-running them is free of effects)
        std::string failed;
        for (size_t k = 0; k < std::size(ms); ++k) {
            bool s = true;
            ms[k]->finalize(s);
            if (!s) failed += std::string(failed.empty() ? "" : "+") + kNames[k];
        }
        std::string why = "  pp " + def.name + ": fail " + failed + " (" + std::to_string(pp.recs.size()) + " candidates, " +
                          std::to_string(selection.size()) + " selected, last veto " + (last_veto < 9 ? kNames[last_veto] : "-") +
                          ", gross " + std::to_string(static_cast<int64_t>(pp.gross)) + ")";
        ++stats[why];
        return false;
    }
    out = PotentialPlanRec{};
    out.player = pa.player;
    out.plan = plan;
    out.goal_type = goal_type;
    out.target = target;
    out.gross = pp.gross;
    out.net = pp.net;
    out.eta = pp.eta;
    out.contrast_ok = true;
    out.pop = pp.committed_pop;
    for (size_t i = 0; i < pp.sel_rec.size(); ++i) {
        PpSelection s;
        s.type = pp.recs[pp.sel_rec[i]].type;
        if (pp.sel_unit[i] >= 0) s.unit = pp.units[static_cast<size_t>(pp.sel_unit[i])].h;
        const int32_t slot = pp.sel_slot[i];
        s.tf = slot >= 0 && static_cast<size_t>(slot) < pp.slots.size() ? static_cast<int32_t>(pp.slots[static_cast<size_t>(slot)].tf) : 0;
        s.source = pp.sel_source[i];
        s.queue = pp.sel_queue[i];
        s.btime = pp.sel_btime[i];
        if (s.unit) out.reserved_units.push_back(store.id(s.unit));  // PP+0x108
        out.sel.push_back(s);
    }
    return true;
}

// The galactic free store is derived: the player's live units no task force holds (free_units).
bool GalacticAiWorld::Impl::on_free_store(int32_t player, objects::ObjectId id) const {
    const ObjectHandle u = store.find(id);
    if (!store.alive(u) || store.owner(u) != player || tf_owns(u)) return false;
    for (const ObjectHandle f : player_units(player))
        if (f == u) return true;
    return false;
}

// 0x6c4a50 (A1_READINGS §28.6 step 3): the new proposal's PP is solved again (PP+0x104 >= 1: units another PP reserved
// are skipped, PP+0x108 rebuilt); a failed solve = the plan cannot be activated. [M] 0x6c4a50 runs the same solver
// 0x6c3da0, so the re-solve draws its own synced numbers in Resolve_Category order. The goal keeps Init's cost / ETA
// (+0x78 / +0x7c) [M: 0x6c4a50 is not read to copy them]. A proposal's PP has no queue or unit reservation yet (both
// start at its accept), so the record is replaced.
bool GalacticAiWorld::Impl::resolve_pp(PlayerAi& pa, const Goal& g) {
    const auto it = pps.find(g.pp_handle);
    if (it == pps.end()) return false;
    PotentialPlanRec rec;
    if (!solve_potential_plan(pa, it->second.plan, g.type, g.target, rec, false)) {
        ++stats[defs.goals[g.type].name + " re-solve failed (" + library.plans[it->second.plan].name + ")"];
        return false;
    }
    rec.contrast_ok = it->second.contrast_ok;
    it->second = std::move(rec);
    return true;
}

// 0x6c2d60 -> 0x6c2b20 (A1_READINGS §28.4): +0x30 = this PP for every PP+0x108 unit still on the free store,
// overwriting any other PP's value (no check).
void GalacticAiWorld::Impl::reserve_pp_units(int32_t handle) {
    const auto it = pps.find(handle);
    if (it == pps.end()) return;
    for (const objects::ObjectId id : it->second.reserved_units)
        if (on_free_store(it->second.player, id)) unit_reserved[id] = handle;
}

// 0x6c0be0 at Commit start (§28.6 step 2): the PP's +0x30 entries are cleared until 0x6c2d60 re-asserts them.
void GalacticAiWorld::Impl::suspend_pp_units(int32_t handle) {
    const auto it = pps.find(handle);
    if (it == pps.end()) return;
    for (const objects::ObjectId id : it->second.reserved_units) {
        const auto r = unit_reserved.find(id);
        if (r != unit_reserved.end() && r->second == handle) unit_reserved.erase(r);
    }
}

// 0x6c45e0 (from 0x6c47c0): invalid when a PP+0x108 unit still on the free store is reserved by another PP.
bool GalacticAiWorld::Impl::pp_stolen(int32_t handle) const {
    const auto it = pps.find(handle);
    if (it == pps.end()) return false;
    for (const objects::ObjectId id : it->second.reserved_units) {
        const auto r = unit_reserved.find(id);
        if (r != unit_reserved.end() && r->second != handle && on_free_store(it->second.player, id)) return true;
    }
    return false;
}

// 0x6c27d0 (PP, unit) for a unit leaving a task force of plan `plan_id`: +0x30 = 0 if it is the plan's PP, and the unit
// leaves PP+0x108 (later re-asserts skip it). The galactic free store takes it back by itself (derived).
void GalacticAiWorld::Impl::pp_unit_left(int32_t plan_id, ObjectHandle u) {
    const auto pit = plans.find(plan_id);
    if (pit == plans.end() || pit->second.pp < 0) return;
    const auto it = pps.find(pit->second.pp);
    if (it == pps.end()) return;
    const objects::ObjectId id = store.id(u);
    const auto r = unit_reserved.find(id);
    if (r != unit_reserved.end() && r->second == pit->second.pp) unit_reserved.erase(r);
    auto& v = it->second.reserved_units;
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
}

void GalacticAiWorld::Impl::release_pp(int32_t handle) {
    const auto it = pps.find(handle);
    if (it == pps.end()) return;
    for (const objects::ObjectId id : it->second.reserved_units) {
        const auto r = unit_reserved.find(id);
        if (r != unit_reserved.end() && r->second == handle) unit_reserved.erase(r);
    }
    it->second.reserved_units.clear();
    for (auto& pq : planet_queues)  // 0x6c2320
        for (PlanetBuildQueue& q : pq)
            q.reservations.erase(std::remove_if(q.reservations.begin(), q.reservations.end(),
                                                [handle](const QueueReservation& r) { return r.pp == handle; }),
                                 q.reservations.end());
    it->second.queues_reserved = false;
}

void GalacticAiWorld::Impl::reserve_pp_queues(int32_t handle) {
    const auto it = pps.find(handle);
    if (it == pps.end() || it->second.queues_reserved) return;  // PP+0x4E guard
    it->second.queues_reserved = true;
    for (const PpSelection& s : it->second.sel) {
        if (s.unit || s.source == kInvalidPlanet || s.source >= planet_queues.size()) continue;
        planet_queues[s.source][s.queue].reservations.push_back({handle, s.type, s.btime});
    }
}

// The AIPlanetBuildQueue time of a planet queue (0x53bb20(..., 1, 0)) [M]: the sim queue's remaining time (head:
// its remaining seconds, others their build time) + claimed AI tasks not yet producing + committed reservations.
float GalacticAiWorld::Impl::planet_queue_time(PlanetIndex p, uint32_t q) const {
    if (!valid_planet(p) || q > 1) return 0.0f;
    float t = 0.0f;
    const auto queue = sim.queue_of(p, q);  // A2 galactic: GalacticSim::queue_of
    for (size_t i = 0; i < queue.size(); ++i) t += i == 0 ? queue[i].remaining : queue[i].build_time;
    const PlanetBuildQueue& pq = planet_queues[p][q];
    for (const int32_t id : pq.tasks) {
        const auto it = builds.find(id);
        if (it != builds.end() && it->second.state == BuildState::Queued)
            t += btime_at(it->second.player, it->second.type, p);
    }
    for (const QueueReservation& r : pq.reservations) t += r.time;
    return t;
}

}  // namespace alamo::ai

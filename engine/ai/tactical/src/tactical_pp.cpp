// A2 tactical - the tactical PotentialPlan marshallers (tactical_pp.hpp). Rules: docs/re/AI_GALACTIC_2.md §1.2-1.4
// (shared marshallers, tactical branches) and docs/re/AI_GALACTIC_3.md §1-3 (Reinforcement, TacticalBuild, tactical
// hero attach, tactical population cap). [M] / [L] / [port] mark what the docs do not fix.
#include "alamo/ai/tactical/tactical_pp.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/auto_resolve.hpp"

namespace alamo::ai::tactical {

namespace {

constexpr float kVeto = kAiBigFloat;

struct ContrastEntry {
    int32_t row = -1;   // contrast-table row; -1 = the untyped layer total
    bool space = false; // layer
    double target = 0.0;
    bool escort = false;  // q6 t19: entry of the escort list (AI_TACTICAL §7.7.1, flag 1)
};

struct PpRec {  // rec[i]
    int32_t type = -1;
    int32_t free_unit = -1;   // +4: index into units
    int32_t pool = -1;        // Reinforcement: index into the pool
    int32_t pad = -1;         // TacticalBuild: index into the pads
    int32_t start_team = -1;  // +0xC
    int32_t team_slot = -1;   // +0x10
    int32_t contrast_entry = -1;
    std::vector<int32_t> teams;
    std::vector<std::pair<int32_t, float>> pairs;
    bool is_hero = false;
    bool non_escort = false, escort = false;
    bool space = false, ground = false;
};

struct PpSlot {
    uint32_t tf = 0, team = 0;
    std::vector<uint32_t> assigned;
    float cost_sum = 0.0f;
    uint64_t mask = 0;
    int32_t tf_count = 0;
    float tf_force = 0.0f;
};

struct FsUnit {
    int32_t index = -1;  // into input.free_units
    double key = 0.0;
    bool consumed = false;
};

bool space_category(uint64_t m, const std::function<uint64_t(std::string_view)>& cm) {
    if (!cm) return false;
    return (m & (cm("Fighter") | cm("Bomber") | cm("Corvette") | cm("Frigate") | cm("Capital") | cm("Super") | cm("SpaceHero"))) != 0;
}

struct TacPp {
    const TacticalPpInput& in;
    const PlanDefinition& def;
    const std::vector<PerceptionType>& types;

    std::vector<PpRec> recs;
    std::map<int32_t, uint32_t> rec_of_type;
    std::vector<PpSlot> slots;
    std::vector<std::vector<int32_t>> slot_types;  // per slot: the team's type list
    std::vector<FsUnit> units;
    double max_key = 0.0;
    std::vector<int32_t> pool_left;  // Reinforcement: entry counts left (0 = cleared)
    bool pool_open = false;          // Reinforcement Setup A ran (pool built)
    float committed_pop = 0.0f;      // 0x140b3a52c
    std::vector<uint8_t> pad_used;   // TacticalBuild: pads taken by commits (removed from the open-pad list)

    std::vector<uint32_t> sel_rec;
    std::vector<int32_t> sel_unit, sel_pool, sel_pad, sel_slot, sel_entry;
    uint64_t selected_mask = 0;
    bool any_hero_attach = false, any_special = false, any_escort = false;
    bool main_met = false, escort_met = false;
    float gross = 0.0f;  // PP+0x40: 0 in tactical (no Budget marshaller, §1.2)
    float net = 0.0f;    // PP+0x44: TacticalBuild purchases
    float tolerance = 0.0f;
    // Contrast lists (AI_TACTICAL §7.7.1): the main list at [0, esc_base), then - q6 t19, only when some TF of the plan
    // is an EscortForce (0x140b3a525) - the escort list at [esc_base, end) with the same layout (space total, ground
    // total, typed rows). No escort list: esc_base = entries.size().
    std::vector<ContrastEntry> entries;
    std::vector<double> remaining;
    bool has_escort_list = false;
    size_t esc_base = 0;
    bool layer_active_space = false, layer_active_ground = false;

    explicit TacPp(const TacticalPpInput& i) : in(i), def(*i.def), types(*i.types) {}

    const PerceptionType* ti(int32_t t) const { return t >= 0 && static_cast<size_t>(t) < types.size() ? &types[static_cast<size_t>(t)] : nullptr; }
    uint64_t mask_of(int32_t t) const { const auto* x = ti(t); return x ? x->category : 0; }
    float power_of(int32_t t) const { const auto* x = ti(t); return x ? x->combat_power : 0.0f; }
    float cost_of(int32_t t) const { const auto* x = ti(t); return x ? x->build_cost : 0.0f; }
    float pop_of(int32_t t) const { const auto* x = ti(t); return x ? x->population : 0.0f; }
    bool hero(int32_t t) const { const auto* x = ti(t); return x && x->is_hero; }
    bool affiliated(int32_t t) const {
        const auto* x = ti(t);
        if (!x) return false;
        if (x->affiliation.empty()) return true;
        for (const std::string& f : x->affiliation)
            if (iequal(f, in.faction)) return true;
        return false;
    }
    bool is_space_type(int32_t t) const {
        return in.space_categories ? (mask_of(t) & in.space_categories) != 0 : space_category(mask_of(t), in.category_mask);
    }
    const TaskForceDef& tf_def(uint32_t tf) const { return def.task_forces[tf]; }
    const TeamDef& team_def(const PpSlot& s) const { return def.task_forces[s.tf].teams[s.team]; }

    uint32_t ensure_rec(int32_t type) {
        const auto it = rec_of_type.find(type);
        if (it != rec_of_type.end()) return it->second;
        const auto i = static_cast<uint32_t>(recs.size());
        PpRec r;
        r.type = type;
        r.space = is_space_type(type);
        r.ground = !r.space;
        recs.push_back(std::move(r));
        rec_of_type.emplace(type, i);
        return i;
    }
    bool free_store_holds(int32_t t) const {
        for (const PpFreeUnit& u : in.free_units)
            if (u.type == t && !u.reserved) return true;
        return false;
    }
    double power_eff(uint32_t r) const {
        double p = static_cast<double>(power_of(recs[r].type));
        if (recs[r].free_unit >= 0) p *= static_cast<double>(in.free_units[static_cast<size_t>(units[static_cast<size_t>(recs[r].free_unit)].index)].health);
        return p;
    }
    std::vector<uint64_t> required_masks() const {
        std::vector<uint64_t> out;
        for (const std::string& r : def.required_categories) {
            uint64_t m = 0;
            for (const std::string& n : ai_split(r, "|")) m |= in.category_mask ? in.category_mask(n) : 0;
            out.push_back(m);
        }
        return out;
    }

    // ---- contrast (0x72ac70 / 0x6c4b20) ----
    float weight(int32_t t, int32_t row) const {
        if (!in.contrast || row < 0) return 0.0f;
        const auto* x = ti(t);
        return x ? in.contrast->contrast(static_cast<size_t>(row), x->name, x->category) : 0.0f;
    }
    bool layer_satisfied(bool space, bool escort = false) const {
        double total = 0.0, residual = 0.0;
        for (size_t e = 0; e < entries.size(); ++e) {
            if (entries[e].row < 0 || entries[e].space != space || entries[e].escort != escort) continue;
            total += entries[e].target;
            residual += std::max(remaining[e], 0.0);
        }
        if (total <= 0.0) return true;
        return residual / total <= static_cast<double>(tolerance);
    }
    // 0x6bff00: every active layer of the list passes 0x72ac70.
    bool list_met(bool escort) const {
        return (!layer_active_space || layer_satisfied(true, escort)) && (!layer_active_ground || layer_satisfied(false, escort));
    }
    bool escort_entry(size_t e) const { return has_escort_list && e >= esc_base; }
    // The entry of the same row / layer in the main (escort = false) or escort list; -1 none.
    int32_t entry_in(int32_t e, bool escort) const {
        if (e < 0 || static_cast<size_t>(e) >= entries.size() || (escort && !has_escort_list)) return -1;
        const size_t off = escort_entry(static_cast<size_t>(e)) ? static_cast<size_t>(e) - esc_base : static_cast<size_t>(e);
        return static_cast<int32_t>((escort ? esc_base : 0) + off);
    }
    // Selection i subtracts from one list (0x6bf840): its contrast entry's row (weight x P) and the layer total (P).
    void subtract(size_t i, bool escort) {
        const PpRec& r = recs[sel_rec[i]];
        const double P = static_cast<double>(power_of(r.type)) *
                         (sel_unit[i] >= 0 ? static_cast<double>(in.free_units[static_cast<size_t>(units[static_cast<size_t>(sel_unit[i])].index)].health) : 1.0);
        const int32_t e = entry_in(sel_entry[i], escort);
        if (e >= 0 && entries[static_cast<size_t>(e)].row >= 0)
            remaining[static_cast<size_t>(e)] -= static_cast<double>(weight(r.type, entries[static_cast<size_t>(e)].row)) * P;
        const size_t base = escort ? esc_base : 0;
        if (entries.size() > base) {
            if (r.space) remaining[base] -= P;
            if (r.ground && entries.size() > base + 1) remaining[base + 1] -= P;
        }
    }
    // 0x6c4b20 (AI_GALACTIC_TOKENS §9.10): remaining lists from scratch, then the flags. No escort list: every selection
    // subtracts from the main list (selection order) and the escort flag is set (empty escort remaining list). With an
    // escort list (q6 t19, tactical): the task-force slots in order, each assigned selection into the escort list when
    // its slot is an escort slot or the main contrast is already met at that point of the walk (re-tested after each
    // main-list unit, false at the start), else into the main list; then the unplaced selections into the main list.
    void recompute_remaining() {
        remaining.resize(entries.size());
        for (size_t e = 0; e < entries.size(); ++e) remaining[e] = entries[e].target;
        if (!has_escort_list) {
            for (size_t i = 0; i < sel_rec.size(); ++i) subtract(i, false);
            main_met = list_met(false);
            escort_met = true;
            return;
        }
        std::vector<uint8_t> placed(sel_rec.size(), 0);
        bool met = false;
        for (const PpSlot& sl : slots) {
            const bool slot_escort = tf_def(sl.tf).escort;
            for (const uint32_t i : sl.assigned) {
                if (i >= sel_rec.size()) continue;
                placed[i] = 1;
                const bool escort = slot_escort || met;
                subtract(i, escort);
                if (!escort) met = list_met(false);
            }
        }
        for (size_t i = 0; i < sel_rec.size(); ++i)
            if (!placed[i]) subtract(i, false);
        main_met = list_met(false);
        escort_met = list_met(true);
    }
};

// ---------------------------------------------------------------- marshallers

class FreeStoreM final : public PpMarshaller {
public:
    explicit FreeStoreM(TacPp& p) : p_(p) {}
    void setup_a() override {  // 0x7287a0
        if (!(p_.def.allow_free_store_units && !p_.def.magic_plan)) return;
        for (size_t i = 0; i < p_.in.free_units.size(); ++i) {
            const PpFreeUnit& u = p_.in.free_units[i];
            if (u.reserved) continue;                         // entry +0x30
            if (u.health < p_.in.health_threshold) continue;  // Health_Low_Percent_Threshold (AI_GALACTIC_3 §8)
            FsUnit f;
            f.index = static_cast<int32_t>(i);
            // Tactical key (AI_GALACTIC_3 §1.2, A1_READINGS §25.3): the host's stage key when a stage cost was
            // finite, else the 2-D distance² from the unit to the goal target; no target -> key 0. The "in transit"
            // unit (0x3a9760, key 0) is not modelled [port].
            if (p_.in.has_target) {
                if (u.stage_key >= 0.0) {
                    f.key = u.stage_key;
                } else {
                    const double dx = static_cast<double>(u.position.x) - p_.in.target_position.x;
                    const double dy = static_cast<double>(u.position.y) - p_.in.target_position.y;
                    f.key = dx * dx + dy * dy;
                }
            }
            p_.max_key = std::max(p_.max_key, f.key);
            p_.units.push_back(f);
        }
    }
    float score(uint32_t i) override {  // 0x728560
        PpRec& r = p_.recs[i];
        // The solver resets rec+4 / +8 / +0x14 per candidate every iteration before any Score (§1.2); FreeStore is
        // first in both tactical lists, so the reset happens here.
        r.free_unit = -1;
        r.pool = -1;
        r.contrast_entry = -1;
        if (!(p_.hero(r.type) || (p_.def.allow_free_store_units && !p_.def.magic_plan))) return 0.0f;  // type +0x13
        int32_t best = -1;
        float best_s = 0.0f;
        for (size_t u = 0; u < p_.units.size(); ++u) {
            const FsUnit& f = p_.units[u];
            if (f.consumed || p_.in.free_units[static_cast<size_t>(f.index)].type != r.type) continue;
            // No +2 outside galactic; the +2 of unit +0x3A2 bit 2 is not modelled [port: flag unknown].
            const float s = p_.max_key != 0.0 ? static_cast<float>(1.0 - f.key / p_.max_key) : 0.0f;
            if (best < 0 || s > best_s) {
                best = static_cast<int32_t>(u);
                best_s = s;
            }
        }
        if (best < 0) return 0.0f;
        r.free_unit = best;
        return best_s;
    }
    void commit(uint32_t i) override {  // 0x728fd0
        PpRec& r = p_.recs[i];
        if (r.free_unit < 0) return;
        p_.sel_unit.back() = r.free_unit;
        p_.units[static_cast<size_t>(r.free_unit)].consumed = true;
        r.free_unit = -1;
    }

private:
    TacPp& p_;
};

class TechTreeM final : public PpMarshaller {
public:
    explicit TechTreeM(TacPp& p) : p_(p) {}
    float score(uint32_t i) override {  // 0x729190: the veto is galactic only
        const PerceptionType* x = p_.ti(p_.recs[i].type);
        const int32_t tech = x ? x->tech_level : 0;
        return static_cast<float>(tech) / 5.0f * 2.0f;
    }

private:
    TacPp& p_;
};

class PlanM final : public PpMarshaller {
public:
    explicit PlanM(TacPp& p) : p_(p) {}
    bool permanent_veto() const override { return true; }
    void setup_a() override {  // 0x72a3b0
        p_.slots.clear();
        p_.slot_types.clear();
        for (uint32_t tf = 0; tf < p_.def.task_forces.size(); ++tf) {
            const TaskForceDef& d = p_.def.task_forces[tf];
            p_.any_hero_attach = p_.any_hero_attach || !d.deny_hero_attach;
            p_.any_special = p_.any_special || !d.deny_special_weapon_attach;
            p_.any_escort = p_.any_escort || d.escort;
            for (uint32_t team = 0; team < d.teams.size(); ++team) {
                PpSlot s;
                s.tf = tf;
                s.team = team;
                p_.slots.push_back(std::move(s));
                p_.slot_types.push_back(pp_team_types(d, d.teams[team], p_.types, p_.in.category_mask));
            }
        }
        for (PpRec& r : p_.recs) r.teams.clear();
    }
    void setup_b() override {  // 0x729550 (no galactic upgrade variants)
        for (size_t si = 0; si < p_.slots.size(); ++si)
            for (const int32_t t : p_.slot_types[si]) {
                if (!(p_.affiliated(t) || p_.free_store_holds(t))) continue;
                PpRec& r = p_.recs[p_.ensure_rec(t)];
                if (std::find(r.teams.begin(), r.teams.end(), static_cast<int32_t>(si)) == r.teams.end()) r.teams.push_back(static_cast<int32_t>(si));
            }
    }
    void setup_c() override {  // 0x729cc0: one synced int per candidate with >= 2 teams
        for (PpRec& r : p_.recs)
            r.start_team = r.teams.empty() ? -1 : p_.in.rng->range_int(0, static_cast<int32_t>(r.teams.size()) - 1);
    }
    float score(uint32_t i) override {  // 0x72a170
        PpRec& r = p_.recs[i];
        r.team_slot = -1;
        const auto n = static_cast<int32_t>(r.teams.size());
        for (int32_t k = 0; k < n; ++k) {
            const int32_t s = r.teams[static_cast<size_t>((r.start_team + k) % n)];
            const PpSlot& slot = p_.slots[static_cast<size_t>(s)];
            if (p_.team_def(slot).percent || static_cast<int32_t>(slot.assigned.size()) < p_.team_def(slot).max) {
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
                // [M] tactical PP+0x3C: the player's credits (no budget category)
                if (td.percent_value / 100.0f * p_.in.credits > p_.cost_of(r.type) + slot.cost_sum) return 1.0f;
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
            // First pass (0x729830, AI_GALACTIC_TOKENS §9.10): a non-escort slot is skipped when main is met and escort is
            // not, an escort slot when escort is met and main is not. The second pass ignores the flags.
            if (respect_met && ((!escort && p_.main_met && !p_.escort_met) || (escort && p_.escort_met && !p_.main_met))) continue;
            if (!(p_.team_def(slot).percent || static_cast<int32_t>(slot.assigned.size()) < p_.team_def(slot).max)) continue;
            slot.assigned.push_back(sel);
            slot.cost_sum += p_.cost_of(r.type);
            slot.mask |= p_.mask_of(r.type);
            for (PpSlot& o : p_.slots)
                if (o.tf == slot.tf) {
                    ++o.tf_count;
                    o.tf_force += p_.power_of(r.type);
                }
            p_.sel_slot[sel] = s;
            return true;
        }
        return false;
    }
    void commit(uint32_t i) override {  // 0x72a720 -> 0x729830
        const auto sel = static_cast<uint32_t>(p_.sel_rec.size() - 1);
        p_.selected_mask |= p_.mask_of(p_.recs[i].type);
        if (!place(sel, i, true)) place(sel, i, false);
    }
    void finalize(bool& success) override {  // 0x72a060
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
    TacPp& p_;
};

// Reinforcement (AI_GALACTIC_3 §1.3-1.5) [H]: pool units score 2.0 and replace a free unit of the same type unless that
// unit is already at the stage (key < 300); no pool entry and no free unit -> veto (not permanent). No RNG.
class ReinforcementM final : public PpMarshaller {
public:
    explicit ReinforcementM(TacPp& p) : p_(p) {}
    float remaining_pop() const {  // 0x2acb10: cap - max(used, 0); no cap = unlimited
        return p_.in.pop_cap < 0.0f ? 1e30f : p_.in.pop_cap - std::max(p_.in.pop_used, 0.0f);
    }
    void setup_a() override {  // 0x727940
        p_.pool_left.assign(p_.in.pool.size(), 0);
        p_.pool_open = false;
        // mode != galactic, not MagicPlan, room for one unit (0x2b03e0 with no type: (int)(1.0 + 0.5) <= remaining).
        if (p_.def.magic_plan || !(1.0f <= remaining_pop())) return;
        p_.pool_open = true;
        for (size_t k = 0; k < p_.in.pool.size(); ++k) {
            const PpPoolEntry& e = p_.in.pool[k];
            if (!e.reserved && e.type >= 0) p_.pool_left[k] = std::max(1, e.count);  // empty entry when reserved
        }
    }
    bool matches(const PpPoolEntry& e, int32_t t) const {
        if (e.member_type < 0) return t == e.type;  // E has no Company_Units
        return t == e.member_type || ((e.member_create_team || p_.hero(t)) && t == e.type);
    }
    float score(uint32_t i) override {  // 0x7276a0
        PpRec& r = p_.recs[i];
        const PerceptionType* x = p_.ti(r.type);
        if (p_.def.magic_plan || (x && x->under_construction) || !p_.def.allow_free_store_units) return 0.0f;
        const bool f = r.free_unit >= 0;
        bool any = false;
        for (const int32_t c : p_.pool_left) any = any || c > 0;
        const bool usable = p_.pool_open && p_.in.reinforcements_enabled && any &&
                            !(remaining_pop() < p_.pop_of(r.type) + p_.committed_pop);
        if (!usable) return f ? 0.0f : kVeto;
        const int32_t my_tf = r.team_slot >= 0 ? static_cast<int32_t>(p_.slots[static_cast<size_t>(r.team_slot)].tf) : -1;
        for (size_t j = 0; j < p_.in.pool.size(); ++j) {
            if (p_.pool_left[j] <= 0 || !matches(p_.in.pool[j], r.type)) continue;
            bool other_tf = false;  // used by an earlier selection of a different task force -> skip
            for (size_t k = 0; k < p_.sel_pool.size() && !other_tf; ++k) {
                if (p_.sel_pool[k] != static_cast<int32_t>(j)) continue;
                const int32_t s = p_.sel_slot[k];
                const int32_t tf = s >= 0 ? static_cast<int32_t>(p_.slots[static_cast<size_t>(s)].tf) : -1;
                other_tf = tf != my_tf;
            }
            if (other_tf) continue;
            if (f && p_.units[static_cast<size_t>(r.free_unit)].key < 300.0) return 0.0f;  // keep the nearby free unit
            r.free_unit = -1;  // drop the free unit: FreeStore's commit will not consume it
            r.pool = static_cast<int32_t>(j);
            return 2.0f;
        }
        return f ? 0.0f : kVeto;
    }
    void commit(uint32_t i) override {  // 0x727b30
        PpRec& r = p_.recs[i];
        p_.sel_pool.back() = r.pool;  // PP+0x120
        if (r.pool < 0) return;
        const auto j = static_cast<size_t>(r.pool);
        const PpPoolEntry& e = p_.in.pool[j];
        if (p_.pool_left[j] == std::max(1, e.count)) p_.committed_pop += e.population;  // first use: charged once
        --p_.pool_left[j];
    }

private:
    TacPp& p_;
};

// Hero (AI_GALACTIC_3 §2) [H]. Setup C makes hero records; Score: a record in a non-magic plan never gets a finite
// score (the Commit's escort accumulation is dead code, §2.4) -> veto (not permanent) every iteration; so heroes join
// only plans where every TF is DenyHeroAttach (no record: plain units), as non-records, or through MagicPlans.
// [port] Setup B's extra candidates (free-store / pool heroes without a team, commander teams) and the hero scripts'
// MinPlanAttachCost / Evaluate_Attack_Ability / Get_Escort_Ability_Weights calls are not run: the extra candidates
// have no team and are vetoed every iteration (no selection effect); the script calls only set script globals.
class HeroM final : public PpMarshaller {
public:
    explicit HeroM(TacPp& p) : p_(p) {}
    bool allowed(const PerceptionType& h) const {  // §2.1 item 2
        return (p_.any_hero_attach || !(h.is_hero || h.generic_hero)) && (p_.any_special || !h.special_weapon);
    }
    void setup_c() override {  // 0x726710
        for (PpRec& r : p_.recs) {
            r.is_hero = false;
            const PerceptionType* h = p_.ti(r.type);  // [port] H = T (company representatives not resolved)
            if (!h || !(h->is_hero || h->generic_hero || h->special_weapon) || !allowed(*h)) continue;
            bool unit = false;  // a free-store unit of T, else a pool entry of T
            for (const PpFreeUnit& u : p_.in.free_units) unit = unit || (!u.reserved && u.type == r.type);
            for (const PpPoolEntry& e : p_.in.pool) unit = unit || (!e.reserved && e.type == r.type);
            if (!unit && p_.in.campaign) continue;
            if (!h->has_ai_script) continue;  // item 4: no script -> not a hero record
            r.is_hero = true;
        }
    }
    float score(uint32_t i) override {  // 0x726f40
        const PpRec& r = p_.recs[i];
        if (!r.is_hero) return 0.0f;
        if (r.teams.empty() || !p_.def.magic_plan) {
            const PerceptionType* h = p_.ti(r.type);
            if (!h || !allowed(*h)) return 0.0f;
            return kVeto;  // rec+0x40 stays 0 (§2.4): never a finite score
        }
        if (r.free_unit < 0)  // MagicPlan with a team: veto when T is already selected [port: other magic plans not checked]
            for (const uint32_t k : p_.sel_rec)
                if (p_.recs[k].type == r.type) return kVeto;
        return 0.0f;
    }

private:
    TacPp& p_;
};

class ContrastM final : public PpMarshaller {
public:
    explicit ContrastM(TacPp& p) : p_(p) {}
    void setup_a() override {  // 0x72bee0 -> 0x6c5de0
        p_.entries.clear();
        p_.tolerance = 0.0f;
        p_.has_escort_list = false;
        p_.esc_base = 0;
        if (!p_.in.has_target || p_.def.ignore_target || !p_.in.enemy_strength) {
            p_.remaining.clear();
            return;
        }
        const float fp = static_cast<float>(p_.in.recent_failures) * p_.def.per_failure_contrast_adjust;
        p_.tolerance = 1.0f - (fp + p_.def.min_contrast_scale) / (fp + p_.def.max_contrast_scale);
        const double scale = static_cast<double>(p_.def.max_contrast_scale + fp) * static_cast<double>(p_.in.contrast_multiplier);
        const bool space_mode = p_.in.mode == TacticalMode::Space;
        // [M] tactical layers: the mode's own layer holds every enemy; the other layer is empty.
        const double total = p_.in.enemy_strength(~uint64_t{0});
        const double space_total = space_mode ? total : 0.0;
        const double ground_total = space_mode ? 0.0 : total;
        p_.entries.push_back({-1, true, space_total});
        p_.entries.push_back({-1, false, ground_total});
        const size_t rows = p_.in.contrast ? p_.in.contrast->size() : 0;
        for (const bool space : {true, false})
            for (size_t j = 0; j < rows; ++j) {
                const double t = space ? space_total : ground_total;
                const double v = t > 0.0 ? p_.in.enemy_strength(p_.in.contrast->rows[j].enemy_mask) * scale : 0.0;
                p_.entries.push_back({static_cast<int32_t>(j), space, v});
            }
        p_.esc_base = p_.entries.size();
        // q6 t19 (AI_TACTICAL §7.7.1, AI_GALACTIC_TOKENS §9.10): the escort list (0x5f1d00 flag 1) when some TF of the
        // plan is an EscortForce (Plan Setup A ran first): the same layout over escort_strength (unit target: the
        // enemies around it, the target excluded; region: the main path). No escort_strength: the main path.
        if (p_.any_escort) {
            const std::function<double(uint64_t)>& es = p_.in.escort_strength ? p_.in.escort_strength : p_.in.enemy_strength;
            p_.has_escort_list = true;
            const double et = es(~uint64_t{0});
            const double es_space = space_mode ? et : 0.0, es_ground = space_mode ? 0.0 : et;
            p_.entries.push_back({-1, true, es_space, true});
            p_.entries.push_back({-1, false, es_ground, true});
            for (const bool space : {true, false})
                for (size_t j = 0; j < rows; ++j) {
                    const double t = space ? es_space : es_ground;
                    const double v = t > 0.0 ? es(p_.in.contrast->rows[j].enemy_mask) * scale : 0.0;
                    p_.entries.push_back({static_cast<int32_t>(j), space, v, true});
                }
        }
        p_.remaining.resize(p_.entries.size());
        for (size_t e = 0; e < p_.entries.size(); ++e) p_.remaining[e] = p_.entries[e].target;
    }
    void setup_c() override {  // 0x72aad0
        if (p_.def.ignore_target || p_.entries.empty()) return;
        for (PpRec& r : p_.recs) {
            for (const int32_t s : r.teams) {
                const bool esc = p_.tf_def(p_.slots[static_cast<size_t>(s)].tf).escort;
                r.escort = r.escort || esc;
                r.non_escort = r.non_escort || !esc;
            }
            r.pairs.clear();
            for (size_t e = 0; e < p_.entries.size(); ++e) {
                if (p_.entries[e].row < 0 || !(p_.remaining[e] > 0.0)) continue;
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
    float score(uint32_t i) override {  // 0x72b940
        PpRec& r = p_.recs[i];
        if (p_.def.ignore_target) return p_.in.rng->inline_range_float(0.0f, p_.power_of(r.type) / 500.0f);  // 0x532480
        if (r.is_hero || p_.entries.empty()) return 0.0f;
        double v = -1.0;
        r.contrast_entry = -1;
        const double P = p_.power_eff(i);
        for (const auto& [e, wgt] : r.pairs) {  // tactical: every pair is considered
            // q6 t19 (AI_GALACTIC_2 §1.4): the main list only for a candidate with a non-escort team, the escort list when
            // the candidate has an escort team or the main contrast is met.
            if (p_.escort_entry(static_cast<size_t>(e)) ? !(r.escort || p_.main_met) : (r.escort && !r.non_escort)) continue;
            const double R = p_.remaining[static_cast<size_t>(e)];
            const double wp = static_cast<double>(wgt) * P;
            const double d = std::max(R - wp, 0.0), m = std::max(R, wp);
            const double s = m > 0.0 ? (1.0 - (d / m) * (d / m)) * static_cast<double>(wgt) : 0.0;
            if (s > v) {
                v = s;
                r.contrast_entry = e;
            }
        }
        if (v > 0.0) return static_cast<float>(v * 2.0);  // x2 in tactical modes
        // Gate: main side [port: the candidate's own layers, kept] with a non-escort team, or (q6 t19) the escort flag
        // with an escort team.
        const bool layer_ok = (!r.space || p_.layer_satisfied(true)) && (!r.ground || p_.layer_satisfied(false));
        const bool main_side = layer_ok && (r.non_escort || !r.escort);
        if (!main_side && !(p_.escort_met && r.escort)) return kVeto;
        if (p_.def.minimum_cost + p_.def.additional_cost > p_.gross) return 0.0f;
        if (main_side &&
            ((r.space && !p_.remaining.empty() && p_.remaining[0] > 0.0) || (r.ground && p_.remaining.size() > 1 && p_.remaining[1] > 0.0)))
            return 0.0f;
        if (p_.has_escort_list) {  // the escort list's untyped totals (0x140b3a525 set)
            const size_t b = p_.esc_base;
            if ((r.space && p_.remaining[b] > 0.0) || (r.ground && p_.remaining.size() > b + 1 && p_.remaining[b + 1] > 0.0)) return 0.0f;
        }
        for (const uint64_t m : p_.required_masks())
            if ((m & p_.selected_mask) == 0 && (m & p_.mask_of(r.type)) != 0) return 0.0f;
        for (const int32_t s : r.teams) {
            const PpSlot& slot = p_.slots[static_cast<size_t>(s)];
            const TeamDef& td = p_.team_def(slot);
            const TaskForceDef& d = p_.tf_def(slot.tf);
            if ((!td.percent && static_cast<int32_t>(slot.assigned.size()) < td.min) || slot.tf_force < d.minimum_total_force ||
                slot.tf_count < d.minimum_total_size)
                return 0.0f;
        }
        return kVeto;
    }
    void commit(uint32_t) override {
        if (p_.def.ignore_target || p_.entries.empty()) return;
        p_.recompute_remaining();
    }
    void finalize(bool& success) override {  // 0x72af80
        if (p_.def.ignore_target || p_.entries.empty()) return;
        for (const bool escort : {false, true}) {  // q6 t19: both lists (entries 0 and 1 of each)
            if (escort && !p_.has_escort_list) continue;
            const size_t base = escort ? p_.esc_base : 0;
            for (const bool space : {true, false}) {
                if (!(space ? p_.layer_active_space : p_.layer_active_ground)) continue;
                const size_t e = base + (space ? 0 : 1);
                if (p_.entries[e].target > 0.0 && p_.remaining[e] / p_.entries[e].target > static_cast<double>(p_.tolerance)) success = false;
                if (!p_.layer_satisfied(space, escort)) success = false;
            }
        }
    }

private:
    TacPp& p_;
};

// TacticalBuild (AI_GALACTIC_3 §1.7-1.9) [H]: pad structures (behaviour 0x2A) need a goal target and an open pad that
// accepts them; other types only in build plans (AllowFreeStoreUnits = false, not magic) and only when a builder can
// build them now; both must be affordable cumulatively (PP+0x44 vs credits). Every veto is permanent. No RNG.
class TacticalBuildM final : public PpMarshaller {
public:
    explicit TacticalBuildM(TacPp& p) : p_(p) {}
    bool permanent_veto() const override { return true; }
    void setup_a() override { p_.pad_used.assign(p_.in.pads.size(), 0); }  // 0x727fb0: the host lists open pads only
    bool pad_ok(size_t k, int32_t t) const {
        const PpPad& pd = p_.in.pads[k];
        return !pd.reserved && !p_.pad_used[k] && std::find(pd.accepts.begin(), pd.accepts.end(), t) != pd.accepts.end();
    }
    float score(uint32_t i) override {  // 0x727d30
        const PpRec& r = p_.recs[i];
        const PerceptionType* x = p_.ti(r.type);
        const bool uc = x && x->under_construction;
        if (!uc && (p_.def.allow_free_store_units || p_.def.magic_plan)) return 0.0f;
        const float c = x ? x->tactical_cost : 0.0f;  // pad price 0x3744e0 / tactical price 0x496d80 (host)
        if (!(c > 0.0f) || c + p_.net > p_.in.credits) return kVeto;
        if (!uc) return p_.in.can_build_now && p_.in.can_build_now(r.type) ? 1.0f : kVeto;
        if (!p_.in.has_target) return kVeto;
        for (size_t k = 0; k < p_.in.pads.size(); ++k)
            if (pad_ok(k, r.type)) return 1.0f;
        return kVeto;
    }
    void commit(uint32_t i) override {  // 0x728000
        const PpRec& r = p_.recs[i];
        const PerceptionType* x = p_.ti(r.type);
        if (!p_.def.magic_plan && p_.sel_unit.back() < 0 && p_.sel_pool.back() < 0) p_.net += x ? x->tactical_cost : 0.0f;
        if (!x || !x->under_construction) return;
        // The open pad accepting T nearest (2-D) to the goal target; strict <, first wins; removed from the list.
        int32_t best = -1;
        double best_d = 0.0;
        for (size_t k = 0; k < p_.in.pads.size(); ++k) {
            if (!pad_ok(k, r.type)) continue;
            const double dx = static_cast<double>(p_.in.pads[k].position.x) - p_.in.target_position.x;
            const double dy = static_cast<double>(p_.in.pads[k].position.y) - p_.in.target_position.y;
            const double d = dx * dx + dy * dy;
            if (best < 0 || d < best_d) {
                best = static_cast<int32_t>(k);
                best_d = d;
            }
        }
        if (best < 0) return;
        p_.sel_pad.back() = best;
        p_.pad_used[static_cast<size_t>(best)] = 1;
    }

private:
    TacPp& p_;
};

// Forwards every call; records which marshaller's Finalize cleared success (diagnostics only).
class TrackM final : public PpMarshaller {
public:
    TrackM(PpMarshaller& m, const char* name, std::string& failed) : m_(m), name_(name), failed_(failed) {}
    void setup_a() override { m_.setup_a(); }
    void setup_b() override { m_.setup_b(); }
    void setup_c() override { m_.setup_c(); }
    float score(uint32_t c) override { return m_.score(c); }
    void commit(uint32_t c) override { m_.commit(c); }
    void finalize(bool& success) override {
        const bool before = success;
        m_.finalize(success);
        if (before && !success) failed_ = name_;
    }
    bool permanent_veto() const override { return m_.permanent_veto(); }

private:
    PpMarshaller& m_;
    const char* name_;
    std::string& failed_;
};

}  // namespace

std::vector<int32_t> pp_team_types(const TaskForceDef& d, const TeamDef& t, const std::vector<PerceptionType>& types,
                                   const std::function<uint64_t(std::string_view)>& category_mask) {
    std::vector<int32_t> out;
    uint64_t mask = 0;
    bool is_mask = !t.names.empty();
    for (const std::string& n : t.names) {
        const uint64_t b = category_mask ? category_mask(n) : 0;
        if (b == 0) is_mask = false;
        mask |= b;
    }
    uint64_t filter = 0;
    for (const std::string& n : d.masks) filter |= category_mask ? category_mask(n) : 0;
    if (is_mask) {
        for (size_t ty = 0; ty < types.size(); ++ty) {
            const PerceptionType& x = types[ty];
            if (x.pp_excluded) continue;  // flags +0x67 / +0x29 / +0x21, 0x5C / 0x5B (host-filled) [H]
            if ((mask & x.category) == 0) continue;
            if (filter != 0 && (filter & x.category) != filter) continue;
            out.push_back(static_cast<int32_t>(ty));
        }
    } else {
        for (const std::string& n : t.names)
            for (size_t ty = 0; ty < types.size(); ++ty)
                if (iequal(types[ty].name, ai_trim(n))) {
                    if (!types[ty].pp_excluded && std::find(out.begin(), out.end(), static_cast<int32_t>(ty)) == out.end())
                        out.push_back(static_cast<int32_t>(ty));
                    break;
                }
    }
    for (const std::string& x : d.excluded_types)
        for (size_t ty = 0; ty < types.size(); ++ty)
            if (iequal(types[ty].name, ai_trim(x))) out.erase(std::remove(out.begin(), out.end(), static_cast<int32_t>(ty)), out.end());
    return out;
}

bool solve_tactical_pp(const TacticalPpInput& in, TacticalPpResult& out) {
    out = TacticalPpResult{};
    if (!in.def || !in.types || !in.rng) return false;
    TacPp pp(in);
    FreeStoreM fs(pp);
    PlanM planm(pp);
    ReinforcementM reinf(pp);
    TechTreeM tech(pp);
    HeroM heroes(pp);
    ContrastM contrast(pp);
    TacticalBuildM build(pp);
    std::vector<PpMarshaller*> ms;
    std::vector<const char*> names;
    for (const PpMarshallerKind k : pp_marshaller_order(in.mode == TacticalMode::Land ? AiMode::Land : AiMode::Space)) {
        names.push_back(pp_marshaller_name(k));
        switch (k) {
            case PpMarshallerKind::FreeStore: ms.push_back(&fs); break;
            case PpMarshallerKind::Plan: ms.push_back(&planm); break;
            case PpMarshallerKind::Reinforcement: ms.push_back(&reinf); break;
            case PpMarshallerKind::TechTree: ms.push_back(&tech); break;
            case PpMarshallerKind::Hero: ms.push_back(&heroes); break;
            case PpMarshallerKind::Contrast: ms.push_back(&contrast); break;
            case PpMarshallerKind::TacticalBuild: ms.push_back(&build); break;
            default: names.pop_back(); break;
        }
    }
    PpSolverIo io;
    io.candidate_count = [&pp] { return pp.recs.size(); };
    io.type_of = [&pp](uint32_t c) { return static_cast<uint32_t>(pp.recs[c].type); };
    io.on_select = [&pp](uint32_t c) {
        pp.sel_rec.push_back(c);
        pp.sel_unit.push_back(-1);
        pp.sel_pool.push_back(-1);
        pp.sel_pad.push_back(-1);
        pp.sel_slot.push_back(pp.recs[c].team_slot);
        pp.sel_entry.push_back(pp.recs[c].contrast_entry);
    };
    std::string failed;
    std::vector<TrackM> tracked;
    tracked.reserve(ms.size());
    for (size_t k = 0; k < ms.size(); ++k) tracked.emplace_back(*ms[k], names[k], failed);
    std::vector<PpMarshaller*> list;
    for (TrackM& t : tracked) list.push_back(&t);
    std::vector<uint32_t> selection;
    out.ok = run_pp_solver(list, io, selection, in.max_iterations);
    out.candidates = pp.recs.size();
    out.last_veto = failed.empty() ? std::string("-") : failed;
    for (size_t i = 0; i < pp.sel_rec.size(); ++i) {
        TacticalPpPick k;
        k.type = pp.recs[pp.sel_rec[i]].type;
        if (pp.sel_unit[i] >= 0) {
            k.source = PpSource::FreeStore;
            k.ref = in.free_units[static_cast<size_t>(pp.units[static_cast<size_t>(pp.sel_unit[i])].index)].id;
        } else if (pp.sel_pool[i] >= 0) {
            k.source = PpSource::Reinforcement;
            k.ref = in.pool[static_cast<size_t>(pp.sel_pool[i])].key;
        } else if (pp.sel_pad[i] >= 0) {
            k.source = PpSource::Build;
            k.ref = in.pads[static_cast<size_t>(pp.sel_pad[i])].id;
        }
        const int32_t slot = pp.sel_slot[i];
        if (slot >= 0 && static_cast<size_t>(slot) < pp.slots.size()) {
            k.tf = static_cast<int32_t>(pp.slots[static_cast<size_t>(slot)].tf);
            k.team = static_cast<int32_t>(pp.slots[static_cast<size_t>(slot)].team);
        }
        out.picks.push_back(k);
    }
    return out.ok;
}

}  // namespace alamo::ai::tactical

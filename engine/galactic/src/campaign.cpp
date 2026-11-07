#include "alamo/galactic/campaign.hpp"

#include <algorithm>

#include "alamo/objects/behaviors.hpp"

namespace alamo::galactic {

using objects::SpawnParams;
namespace beh = objects::beh;

namespace {

void note(std::vector<std::string>* diag, std::string msg) {
    if (diag) diag->push_back(std::move(msg));
}

template <class T>
void erase_value(std::vector<T>& v, const T& x) {
    v.erase(std::remove(v.begin(), v.end(), x), v.end());
}

}  // namespace

GalacticSim::GalacticSim(const GalacticRules& rules, objects::ObjectStore& store, SimRng& rng)
    : rules_(&rules), store_(&store), rng_(&rng) {}

// ---- helpers -----------------------------------------------------------------------------------------------

const FactionRules& GalacticSim::faction_of(int32_t player) const {
    static const FactionRules kNone{};
    if (!valid_player(player)) return kNone;
    const uint32_t f = players_[static_cast<size_t>(player)].faction;
    return f < rules_->factions.size() ? rules_->factions[f] : kNone;
}

bool GalacticSim::benefits(int32_t player) const { return faction_of(player).benefits_from_corruption; }

// ---- diplomacy: the player relation table (player +0x370; PHASE2_ANSWERS Q5, L1_S4_REMAINING "Relations") -----------

// Defaults [inferred]: different players are enemies unless one of them is an Is_Neutral faction (the former stand-in).
Relation GalacticSim::default_relation(int32_t a, int32_t b) const {
    if (a == b) return Relation::Self;
    if (faction_of(a).is_neutral || faction_of(b).is_neutral) return Relation::Neutral;
    return Relation::Enemy;
}

Relation GalacticSim::relation(int32_t a, int32_t b) const {
    if (!valid_player(a) || !valid_player(b)) return Relation::Neutral;
    if (a == b) return Relation::Self;
    const size_t n = players_.size();
    if (relations_.size() != n * n) return default_relation(a, b);
    return static_cast<Relation>(relations_[static_cast<size_t>(a) * n + static_cast<size_t>(b)]);
}

void GalacticSim::set_relation(int32_t a, int32_t b, Relation r) {
    if (!valid_player(a) || !valid_player(b) || a == b || r == Relation::Self || static_cast<uint8_t>(r) > 3) return;
    const size_t n = players_.size();
    if (relations_.size() != n * n) {  // materialise the defaults on the first change
        relations_.assign(n * n, 0);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
                relations_[i * n + j] = static_cast<uint8_t>(default_relation(static_cast<int32_t>(i), static_cast<int32_t>(j)));
    }
    relations_[static_cast<size_t>(a) * n + static_cast<size_t>(b)] = static_cast<uint8_t>(r);
    relations_[static_cast<size_t>(b) * n + static_cast<size_t>(a)] = static_cast<uint8_t>(r);
}

bool GalacticSim::allied(int32_t a, int32_t b) const {
    const Relation r = relation(a, b);
    return valid_player(a) && valid_player(b) && (r == Relation::Self || r == Relation::Ally);
}

bool GalacticSim::hostile(int32_t a, int32_t b) const { return relation(a, b) == Relation::Enemy; }

bool GalacticSim::diplomacy_state_default() const {
    if (!relations_.empty()) return false;
    for (const PlayerState& p : players_)
        if (!p.unlocked_types.empty()) return false;
    return true;
}

void GalacticSim::hash_diplomacy(StateHasher& h) const {
    h.u32(0x44495031u);  // section marker "DIP1"
    h.u32(static_cast<uint32_t>(relations_.size()));
    for (uint8_t r : relations_) h.u8(r);
    for (const PlayerState& p : players_) {
        h.u32(static_cast<uint32_t>(p.unlocked_types.size()));
        for (uint32_t t : p.unlocked_types) h.u32(t);
    }
}

float GalacticSim::credit_multiplier(int32_t player) const {
    const uint32_t d = players_[static_cast<size_t>(player)].difficulty;
    return d < rules_->difficulties.size() ? rules_->difficulties[d].credit_multiplier : 1.0f;
}

const DifficultyRules* GalacticSim::ai_difficulty(int32_t player) const {
    if (!valid_player(player)) return nullptr;
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    return ps.ai && !ps.human && ps.difficulty < rules_->difficulties.size() ? &rules_->difficulties[ps.difficulty] : nullptr;
}

int32_t GalacticSim::player_of_faction(std::string_view faction) const {
    const uint32_t f = rules_->find_faction(faction);
    for (size_t i = 0; i < players_.size(); ++i)
        if (players_[i].faction == f) return static_cast<int32_t>(i);
    return kNoPlayer;
}

uint32_t GalacticSim::fleet_of(ObjectHandle object) const {
    if (!store_->exists(object) || object.slot() >= fleet_by_slot_.size()) return kNoFleet;
    const uint32_t f = fleet_by_slot_[object.slot()];
    return (f < fleets_.size() && fleets_[f].alive && fleets_[f].object == object) ? f : kNoFleet;
}

PlanetIndex GalacticSim::planet_of(ObjectHandle object) const {
    if (!store_->exists(object) || object.slot() >= planet_by_slot_.size()) return kInvalidPlanet;
    const PlanetIndex p = planet_by_slot_[object.slot()];
    return (p < planets_.size() && planets_[p].object == object) ? p : kInvalidPlanet;
}

std::vector<uint32_t> GalacticSim::fleets_at(PlanetIndex planet, int32_t player) const {
    std::vector<uint32_t> out;
    if (planet >= planets_.size()) return out;
    for (uint32_t f : planets_[planet].orbit)
        if (fleets_[f].alive && fleets_[f].owner == player) out.push_back(f);
    return out;
}

int32_t GalacticSim::ground_base_level(PlanetIndex planet) const {
    // Ground base level (planet data +0x130) is not modelled as its own counter: it is the number of live ground-family
    // structures on the surface (is_ground_structure_type, 0x5F); orbital structures do not count [M] (A2f t08;
    // STRATEGIC_GAPS §1.3 step 18 reads the same surface count, "0 when no ground base").
    int32_t n = 0;
    for (ObjectHandle s : planets_[planet].structures)
        if (store_->alive(s) && is_ground_structure_type(rules_->types[static_cast<size_t>(store_->type(s))])) ++n;
    return n;
}

int32_t GalacticSim::planets_owned(int32_t player) const {
    int32_t n = 0;
    for (const PlanetState& p : planets_)
        if (p.is_planet && p.owner == player) ++n;
    return n;
}

bool GalacticSim::pays_as_you_go(int32_t player) const {
    // 0x289050 [H]: every non-human player pays as it goes; humans follow Pay_As_You_Go.
    const PlayerState& p = players_[static_cast<size_t>(player)];
    return !p.human || rules_->constants.pay_as_you_go;
}

uint32_t GalacticSim::route_between(PlanetIndex a, PlanetIndex b) const {
    if (a >= graph_.size()) return kNoRoute;
    for (uint32_t e = graph_.edge_begin(a); e < graph_.edge_end(a); ++e)
        if (graph_.neighbour(e) == b) return graph_.route(e);
    return kNoRoute;
}

PlayerView GalacticSim::player_view(int32_t player) const {
    PlayerView v;
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    v.is_human = ps.human;
    v.has_ai_system = ps.ai;
    v.benefits_from_corruption = benefits(player);
    const size_t n = planets_.size();
    v.corrupted.resize(n);
    v.unowned_or_neutral.resize(n);
    v.held_by_ally.resize(n);
    v.hidden.resize(n);
    // AI branch of Is_Planet_Passable_For_Player (0x3f2510): for a mover with an AI system, flag 1 vs flag 0 follows the
    // PERCEIVED owner (Variable_Target.TargetPlayerID resolved to a player whose faction is not Is_Neutral, GALACTIC_ECONOMY
    // 6.4). The sim keeps no fogged memory, so the stand-in is the TRUE owner [port]: a player, not neutral (the same bit
    // as unowned_or_neutral, inverted) [M]. The AI's own view (engine/ai GalacticPerception::fill_view) overlays the
    // remembered owner and ai_eval_failed. Evaluation never fails here (ai_eval_failed stays empty = none) [port].
    if (v.has_ai_system) v.ai_perceived_non_neutral.resize(n);
    // GRC 1.1: the LOCAL player's raw revealed bytes (hop gate) and the mode's auto-reveal (Is_Planet_Revealed).
    v.fog_disabled = auto_reveal_;
    v.revealed.resize(n);
    for (size_t i = 0; i < n; ++i) v.revealed.set(i, planets_[i].revealed);
    for (size_t i = 0; i < n; ++i) {
        const PlanetState& p = planets_[i];
        if (!p.is_planet) {
            v.hidden.set(i);
            continue;
        }
        v.corrupted.set(i, p.corruption != kCorruptionNone);
        v.unowned_or_neutral.set(i, !valid_player(p.owner) || faction_of(p.owner).is_neutral);
        if (v.has_ai_system) v.ai_perceived_non_neutral.set(i, valid_player(p.owner) && !faction_of(p.owner).is_neutral);
        v.held_by_ally.set(i, allied(player, p.owner) && p.control > 0.0f);  // 0x4ad9f0: relation 0 (self / ally)
        v.hidden.set(i, p.hidden);
    }
    // GRC 2: the LOCAL player's visibility mask (+0x1a8, the original reads the local mask whatever the mover: quirk kept)
    // bits 0x1 Planet_Ownership and 0x10 Fleet_Presence. No local player: left empty (= all visible).
    if (valid_player(local_player_)) {
        v.ownership_visible.resize(n);
        v.local_orbit_visible.resize(n);
        for (size_t i = 0; i < n; ++i) {
            if (!planets_[i].is_planet) continue;
            const uint32_t m = visibility_mask(static_cast<PlanetIndex>(i), local_player_);
            v.ownership_visible.set(i, (m & kVisPlanetOwnership) != 0);
            v.local_orbit_visible.set(i, (m & kVisFleetPresence) != 0);
        }
    }
    // S10: planets in the conflict list (only set while conflicts are attached; empty => none).
    for (size_t i = 0; i < n; ++i)
        if (planets_[i].in_battle) {
            if (v.in_battle.empty()) v.in_battle.resize(n);
            v.in_battle.set(i);
        }
    return v;
}

// ---- setup ---------------------------------------------------------------------------------------------------

bool GalacticSim::start(const GalacticSetup& setup, std::vector<std::string>* diagnostics) {
    map_ = setup.map;
    const size_t n = map_.planets.size();
    players_.clear();
    relations_.clear();
    planets_.assign(n, PlanetState{});
    fleets_.clear();
    fleet_by_slot_.clear();
    planet_by_slot_.clear();
    stealthed_.clear();  // OI galactic
    slot_choices_ = false;
    claims_.clear();
    victory_hold_ = 0;  // story t11
    frame_ = pause_ = last_cycle_frame_ = next_cycle_frame_ = cycle_counter_ = 0;
    linked_battle_pending_ = false;
    local_player_ = kNoPlayer;  // GRC: pinned below
    auto_reveal_ = setup.planet_auto_reveal;
    reveal_sfx_ = false;

    for (const PlayerSetup& ps : setup.players) {
        PlayerState p;
        p.faction = ps.faction;
        p.human = ps.human;
        p.ai = ps.ai;
        p.difficulty = ps.difficulty;
        p.account.credits = ps.credits;
        p.tech_level = ps.tech_level;
        p.max_tech_level = ps.max_tech_level;
        players_.push_back(p);
    }
    last_income_.assign(players_.size(), 0.0f);
    neutral_player_ = kNoPlayer;
    for (size_t i = 0; i < players_.size(); ++i)
        if (faction_of(static_cast<int32_t>(i)).is_neutral) {
            neutral_player_ = static_cast<int32_t>(i);
            break;
        }
    if (players_.empty()) {
        note(diagnostics, "galactic: setup has no players");
        return false;
    }
    // REMAINING_GAPS 1.4 campaign start (0x292590): Set_Tech_Level(start, sweep) per player, in player order.
    if (setup.tech_sweep_at_start)
        for (size_t i = 0; i < players_.size(); ++i) {
            PlayerState& p = players_[i];
            const int32_t start = p.tech_level;
            p.tech_level = 0;  // the start level is applied through the setter (clamp + sweep)
            set_tech_level(static_cast<int32_t>(i), start, true);
        }

    // Planet objects, row order.
    for (size_t i = 0; i < n; ++i) {
        const uint32_t t = i < setup.planet_type.size() ? setup.planet_type[i] : kNoIndex;
        if (t == kNoIndex || t >= rules_->types.size() || !(map_.planets.flags[i] & kPlanetHasPlanetBehavior)) continue;
        PlanetState& p = planets_[i];
        const TypeInfo& ti = rules_->types[t];
        p.is_planet = true;
        p.type = t;
        p.position = map_.planets.position[i];
        p.base_value = ti.credit_value;
        p.black_market_influences = ti.black_market_influences;
        SpawnParams sp;
        sp.type = t;
        sp.owner = kNoPlayer;  // planets report their owner from the data pack (OBJECT_RUNTIME.md)
        sp.position = p.position;
        p.object = store_->create(sp);
        store_->add_behavior(p.object, beh::Planet);
        if (planet_by_slot_.size() <= p.object.slot()) planet_by_slot_.resize(p.object.slot() + 1, kInvalidPlanet);
        planet_by_slot_[p.object.slot()] = static_cast<PlanetIndex>(i);
    }
    for (size_t r = 0; r < map_.routes.size(); ++r) {
        planets_[map_.routes.point_a[r]].links.push_back(static_cast<uint32_t>(r));
        planets_[map_.routes.point_b[r]].links.push_back(static_cast<uint32_t>(r));
    }
    graph_ = PlanetGraph::build(map_);
    derived_.assign(n, Derived{});

    // Leaning: explicit, else the first starting force's player, else neutral [L].
    for (size_t i = 0; i < n; ++i)
        planets_[i].leaning = i < setup.leaning.size() ? setup.leaning[i] : kNoPlayer;
    if (setup.leaning.empty())
        for (const StartingForce& f : setup.forces)
            if (f.planet < n && planets_[f.planet].leaning == kNoPlayer) planets_[f.planet].leaning = f.player;
    for (PlanetState& p : planets_)
        if (p.leaning == kNoPlayer) p.leaning = neutral_player_;

    for (const StartingForce& f : setup.forces) {
        if (!valid_player(f.player) || !valid_planet(f.planet) || f.type >= rules_->types.size()) {
            note(diagnostics, "galactic: starting force skipped (bad player/planet/type)");
            continue;
        }
        if (!create_unit(f.player, f.planet, f.type))
            note(diagnostics, "galactic: starting force " + rules_->types[f.type].name + " has no galactic role");
    }
    // First ownership pass (D +0x1c9 = 0 -> owner and control set directly), then the per-player cap
    // (player setup 0x292050).
    for (PlanetIndex i = 0; i < n; ++i)
        if (planets_[i].is_planet) apply_ownership(i, derive_owner(i));
    recompute_credit_caps();
    // GRC 1.4: the local player (first human, player order [port]) and its Home_Location planet's revealed byte. The
    // other owned planets are revealed by the first pre-step.
    for (size_t i = 0; i < players_.size() && local_player_ == kNoPlayer; ++i)
        if (players_[i].human) local_player_ = static_cast<int32_t>(i);
    if (local_player_ != kNoPlayer && static_cast<size_t>(local_player_) < setup.players.size()) {
        const PlanetIndex home = setup.players[static_cast<size_t>(local_player_)].home;
        if (valid_planet(home) && planets_[home].is_planet) planets_[home].revealed = true;
    }
    // q2f t02 [H]: the mode's frame-0 fiscal cycle runs before the galaxy is built (Q2 traces: no objects at mode frame
    // 0, F fields last 0 / next 1350 / cycle 1 there, credits unchanged until mode frame 1351), so it pays nobody, feeds
    // no AI income tick and rerolls no black-market planet. Only its bookkeeping (GE 1.2 steps 2-4) is left.
    last_cycle_frame_ = 0;
    next_cycle_frame_ = econ::fiscal_period_frames(rules_->constants.fiscal_cycle_seconds);
    cycle_counter_ = 1;
    return true;
}

// ---- credits --------------------------------------------------------------------------------------------------

float GalacticSim::add_credits(int32_t player, float amount, bool /*notify_ai: AI budget (engine/ai)*/) {
    if (!valid_player(player)) return 0.0f;
    PlayerState& p = players_[static_cast<size_t>(player)];
    return econ::add_credits(p.account, amount, p.ai, credit_multiplier(player));
}

void GalacticSim::recompute_credit_caps() {
    // 0x288f40 for every player in index order [H]. The "uncapped" game-mode test is [L]: assumed false.
    for (size_t i = 0; i < players_.size(); ++i) {
        econ::CreditAccount& a = players_[i].account;
        a.cap = econ::credit_cap(planets_owned(static_cast<int32_t>(i)), rules_->constants.credit_cap_per_planet);
        a.credits = std::min(a.credits, a.cap);
    }
}

// ---- income ---------------------------------------------------------------------------------------------------

int32_t GalacticSim::planet_base(const PlanetState& p) const {
    if (p.type >= rules_->types.size()) return 0;
    if (p.destroyed) return rules_->types[p.type].destroyed_credit_value;
    return p.base_value;
}

void GalacticSim::set_income_entry(PlanetState& p, uint8_t kind, float value) {
    for (IncomeEntry& e : p.income)
        if (e.kind == kind) {
            e.value = value;
            return;
        }
    IncomeEntry e;
    e.kind = kind;
    e.value = value;
    p.income.push_back(e);  // new entries go to the tail [H]
}

void GalacticSim::remove_income_entry(PlanetState& p, uint8_t kind) {
    p.income.erase(std::remove_if(p.income.begin(), p.income.end(), [kind](const IncomeEntry& e) { return e.kind == kind; }),
                   p.income.end());
}

float GalacticSim::planet_income(PlanetIndex planet, bool gross) {
    if (!valid_planet(planet)) return 0.0f;
    PlanetState& p = planets_[planet];
    const int32_t now = frame_;
    const EconomyConstants& c = rules_->constants;
    // 1. halted [H]
    if (now <= p.halt_until) return 0.0f;
    // 2. expire breakdown entries (expiry < now) [H]
    p.income.erase(std::remove_if(p.income.begin(), p.income.end(), [now](const IncomeEntry& e) { return e.expiry < now; }),
                   p.income.end());
    // 3. refresh the four standard entries [H]
    const int32_t b = planet_base(p);
    const TypeInfo& ti = rules_->types[p.type];
    set_income_entry(p, kIncomeBase, econ::income_base(b, p.black_market));
    bool keep = false;
    const ForceSide owner_side = valid_player(p.owner) ? faction_of(p.owner).force_alignment : ForceSide::Neutral;
    const float force = econ::income_force(b, owner_side, ti.the_force, ti.force_strength, c.credit_alignment_bonus,
                                           c.credit_alignment_penalty, &keep);
    if (keep) set_income_entry(p, kIncomeForce, force);
    else remove_income_entry(p, kIncomeForce);
    // Political: only when the owner is not the first Is_Neutral faction's player [H]. ASSUMPTION [L]: the
    // entry is removed when the value is 0 (GE: "some kinds remove their entry when the value is 0").
    const float political = p.owner != neutral_player_ ? econ::income_political(b, p.control, c.political_income_curve) : 0.0f;
    if (political != 0.0f) set_income_entry(p, kIncomePolitical, political);
    else remove_income_entry(p, kIncomePolitical);
    // Trade routes: links in link order; the neighbour must be held by the owner (or an ally: no alliance
    // model yet [M]) and the factor > 0 [H]. Removal at 0 [L] as above.
    float gain = 0.0f;
    for (uint32_t r : p.links) {
        const PlanetIndex nb = map_.routes.point_a[r] == planet ? map_.routes.point_b[r] : map_.routes.point_a[r];
        const float factor = map_.routes.credit_gain_factor[r];
        if (valid_planet(nb) && valid_player(p.owner) && planets_[nb].owner == p.owner && factor > 0.0f) gain += factor;
    }
    const float trade = econ::income_trade(b, gain);
    if (trade != 0.0f) set_income_entry(p, kIncomeTrade, trade);
    else remove_income_entry(p, kIncomeTrade);
    // 4. sum front to back [H]
    float sum = 0.0f;
    for (const IncomeEntry& e : p.income) sum += e.value;
    // 5./6. clamp, siphon [H]
    bool siphon = false;
    if (!gross && p.siphon_player != kNoPlayer) {
        if (p.corruption == kCorruptionNone && p.siphon_until < now) {  // siphon ended (0x3ff1f0)
            p.siphon_player = kNoPlayer;
            p.siphon_amount = 0.0f;
        } else {
            siphon = true;
        }
    }
    float siphoned = 0.0f;
    const float net = econ::income_finish(sum, p.min_income_set, p.min_income, p.max_income_set, p.max_income, siphon,
                                          p.siphon_fraction, &siphoned);
    if (siphon && p.siphon_fraction >= 0.0f && p.siphon_fraction <= 1.0f) p.siphon_amount = siphoned;
    return net;
}

float GalacticSim::planet_income_preview(PlanetIndex planet) const {
    // The summation order is part of the AI token values (and so of the AI / campaign CRCs): keep it.
    if (planet >= planets_.size()) return 0.0f;
    const PlanetState& p = planets_[planet];
    if (!p.is_planet || p.type >= rules_->types.size()) return 0.0f;
    if (frame_ <= p.halt_until) return 0.0f;
    const EconomyConstants& c = rules_->constants;
    const int32_t b = p.destroyed ? rules_->types[p.type].destroyed_credit_value : p.base_value;
    float sum = 0.0f;
    for (const IncomeEntry& e : p.income)
        if (e.kind == kIncomeAbility && e.expiry >= frame_) sum += e.value;
    sum += econ::income_base(b, p.black_market);
    bool keep = false;
    const ForceSide owner_side =
        valid_player(p.owner) && players_[static_cast<size_t>(p.owner)].faction < rules_->factions.size()
            ? rules_->factions[players_[static_cast<size_t>(p.owner)].faction].force_alignment
            : ForceSide::Neutral;
    const float force = econ::income_force(b, owner_side, rules_->types[p.type].the_force, rules_->types[p.type].force_strength,
                                           c.credit_alignment_bonus, c.credit_alignment_penalty, &keep);
    if (keep) sum += force;
    if (p.owner != neutral_player_) sum += econ::income_political(b, p.control, c.political_income_curve);
    float gain = 0.0f;
    for (const uint32_t rt : p.links) {
        const PlanetIndex nb = map_.routes.point_a[rt] == planet ? map_.routes.point_b[rt] : map_.routes.point_a[rt];
        const float factor = map_.routes.credit_gain_factor[rt];
        if (nb < planets_.size() && planets_[nb].is_planet && valid_player(p.owner) && planets_[nb].owner == p.owner && factor > 0.0f)
            gain += factor;
    }
    sum += econ::income_trade(b, gain);
    float siphoned = 0.0f;
    return econ::income_finish(sum, p.min_income_set, p.min_income, p.max_income_set, p.max_income, false, 0.0f, &siphoned);
}

float GalacticSim::player_income(int32_t player, bool consume) {
    if (!valid_player(player)) return 0.0f;
    const FactionRules& fr = faction_of(player);
    if (fr.is_neutral || !fr.can_control_planets || !fr.is_playable) return 0.0f;  // [H]
    float s = 0.0f;
    for (PlanetIndex i = 0; i < planets_.size(); ++i) {
        if (!planets_[i].is_planet) continue;
        if (planets_[i].owner == player) {
            s += planet_income(i, false);
        } else if (planets_[i].corruption != kCorruptionNone && fr.benefits_from_corruption) {
            const float pct = econ::corruption_income_pct(rules_->constants, planets_[i].corruption);
            if (pct > 0.0f) {
                const float g = planet_income(i, true) * pct;
                if (g > 0.0f) s += g;
            }
        }
        if (planets_[i].siphon_player == player) {
            planet_income(i, false);  // refreshes the siphon amount first [H]
            s += planets_[i].siphon_amount;
        }
        if (fr.benefits_from_corruption && planets_[i].piracy_pool > 0.0f) {
            s += planets_[i].piracy_pool;
            if (consume) planets_[i].piracy_pool = 0.0f;  // first such player takes the pool [H]
        }
    }
    return s;
}

void GalacticSim::fiscal_cycle() {
    // GE 1.2 [H]
    last_income_.assign(players_.size(), 0.0f);  // A2 galactic: the raw cycle income per player
    // A2 gal t03 (A2GAL_READINGS 1.1) [H]: the AI income tick fires per player inside the payment loop, right after
    // that player's Add_Credits, with the raw Player_Income I (before the AI Credit_Multiplier, the zero floor and the
    // cap clamp), only for a non-human AI player, and never on a zero-income cycle. The accrual (0x614340) belongs to
    // engine/ai: it listens through the income hooks; listeners only read the sim.
    const std::vector<std::pair<uint32_t, IncomeHook>> hooks = income_hooks_;  // a listener may register another
    IncomeTick tick;
    tick.period_seconds = rules_->constants.fiscal_cycle_seconds;  // T = Fiscal_Cycle_Time_In_Secs (AI_GALACTIC_3 §8)
    tick.day = cycle_counter_ + 1;  // the day this cycle completes (the counter itself increments after the loop)
    tick.frame = frame_;
    for (size_t i = 0; i < players_.size(); ++i) {
        const float income = player_income(static_cast<int32_t>(i), true);
        if (income == 0.0f) continue;  // no Add_Credits, no AI tick [H]
        add_credits(static_cast<int32_t>(i), income, false);
        last_income_[i] = income;
        if (!players_[i].ai || players_[i].human) continue;
        tick.player = static_cast<int32_t>(i);
        tick.gain = income;
        for (const auto& h : hooks) h.second(tick);
    }
    last_cycle_frame_ = next_cycle_frame_;
    next_cycle_frame_ += econ::fiscal_period_frames(rules_->constants.fiscal_cycle_seconds);
    ++cycle_counter_;
    // Black-market reroll, planet order, one draw per qualifying planet. OPEN ITEM [M] (GE 7 item 2): the
    // original draws from the LCG at 0x140a13e24; MAIN_LOOP 3.x calls it "a per-planet random value from the
    // synchronized RNG", so the port uses the session's synced SimRng.
    for (PlanetState& p : planets_) {
        if (!p.is_planet || !p.black_market_influences) continue;
        p.black_market = p.destroyed ? 1.0f
                                     : rng_->range_float(rules_->constants.black_market_min, rules_->constants.black_market_max);
    }
    GalacticEvent ev;
    ev.kind = GalacticEventKind::GalacticDayPassed;
    ev.value = cycle_counter_;
    emit(ev);
}

// ---- ownership ----------------------------------------------------------------------------------------------

int32_t GalacticSim::derive_owner(PlanetIndex planet) const {
    // GE 4.1 precedence [M].
    const PlanetState& p = planets_[planet];
    for (ObjectHandle s : p.structures)  // 1. ground base (level = structure count [M])
        if (store_->alive(s)) return store_->owner(s);
    for (ObjectHandle u : p.surface)  // 2. first non-hidden ground unit (OI galactic: galactic stealth, SG 2.2)
        if (store_->alive(u) && !hidden(u)) return store_->owner(u);
    if (p.starbase_level > 0 && valid_player(p.starbase_owner)) return p.starbase_owner;  // 3.
    for (uint32_t q = 0; q < 2; ++q)  // 4. active production (head of ground, then space) [M]
        if (!p.queue[q].empty()) return p.queue[q][0].player;
    // 5. orbital control: only when the surface test 0x375220 fails [H], which includes a destroyed planet (STRATEGIC_GAPS
    // 1.3 step 24; BT §3.3a probe: Kuat destroyed by the TSW, the Rebels retreated, the Death Star alone in orbit -> Empire).
    if (!surface_accessible(planet))
        for (uint32_t f : p.orbit)
            if (fleets_[f].alive && !fleets_[f].moving) return fleets_[f].owner;
    if (valid_player(p.leaning)) return p.leaning;  // 6. political leaning [L: updated on capture]
    return neutral_player_;                         // 7.
}

void GalacticSim::apply_ownership(PlanetIndex planet, int32_t derived) {
    PlanetState& p = planets_[planet];
    const EconomyConstants& c = rules_->constants;
    // Target control [L]: 1.0 + Political_Control_Gain of routes to same-owner planets with control >= 1
    // (vanilla gains are 0), clamped to [0, 5] (hard-coded 5 [M]).
    float target = 1.0f;
    for (uint32_t r : p.links) {
        const PlanetIndex nb = map_.routes.point_a[r] == planet ? map_.routes.point_b[r] : map_.routes.point_a[r];
        if (valid_planet(nb) && planets_[nb].owner == derived && planets_[nb].control >= 1.0f && p.control >= 1.0f)
            target += map_.routes.political_control_gain[r];
    }
    p.target_control = std::clamp(target, 0.0f, 5.0f);
    if (!p.initialised) {
        p.initialised = true;
        p.owner = derived;
        p.control = p.target_control;
        if (benefits(derived) && p.corruption == kCorruptionNone) {  // starts corrupted, type 3 [H]
            p.corruption = kCorruptionRacketeering;
            p.corrupting_player = derived;
            corruption_unlocks(planet, kCorruptionNone, kNoPlayer);  // campaign start 0x28dbe0
            rebuild_corruption_pairs(planet);                         // GRC 4.3: instant, then 0x3ff230
        }
        return;
    }
    if (derived != p.owner) {
        const int32_t old = p.owner;
        p.owner = derived;
        p.control = 0.0f;  // flips at once with control 0 [M]
        on_owner_change(planet, old);
    }
    // Ramp (0x4adab0), one elapsed frame per service.
    const TypeInfo& ti = rules_->types[p.type];
    const ForceSide side = valid_player(p.owner) ? faction_of(p.owner).force_alignment : ForceSide::Neutral;
    const float m = econ::influence_alignment_factor(side, ti.the_force, ti.force_strength, c.influence_alignment_bonus,
                                                     c.influence_alignment_penalty);
    const float before = p.control;
    p.control = econ::political_ramp(p.control, p.target_control,
                                     econ::political_delta(1, m, c.political_control_change_seconds));
    // GE 4.2: dropping from >= 1 to < 1 calls 0x403490 for both queues ([M] "re-checks the head"): no
    // observable effect is modelled yet.
    (void)before;
}

void GalacticSim::on_owner_change(PlanetIndex planet, int32_t old_owner) {
    PlanetState& p = planets_[planet];
    const int32_t now_owner = p.owner;
    p.leaning = now_owner;  // ASSUMPTION [L]: the leaning follows applied captures (planets do not revert)
    // GE 4.2 order [H]: visibility (not modelled), store event + story event (S4 stream); 4. credit caps.
    {
        GalacticEvent ev;
        ev.kind = GalacticEventKind::PlanetOwnerChanged;
        ev.planet = planet;
        ev.player = now_owner;
        ev.other_player = old_owner;
        emit(ev);
    }
    recompute_credit_caps();
    // 5. conflict list (battles are tactical). 6. capture bonus to a playable new owner [M amount].
    if (!p.destroyed && faction_of(now_owner).is_playable)
        add_credits(now_owner, static_cast<float>(rules_->types[p.type].capture_bonus), true);
    // 8. corruption (GRC 4.3 [H]): 10 s transitions begun directly (0x4b72e0 / 0x4b7260, no 0x3ff230 rebuild). Gain
    // to type 3 when the new owner benefits, the old did not and no gain runs (after Cancel, the planet clean);
    // removal when the old owner benefited and the new one (or no owner) does not (after Cancel, still corrupted).
    // The type, unlocks and the CorruptionChanged event follow at completion (planet service).
    const bool old_b = benefits(old_owner), new_b = benefits(now_owner);
    if (new_b && !old_b && !p.corruption_protected) {
        cancel_corruption_transition(planet);
        if (p.corruption == kCorruptionNone) begin_gain_raw(planet, 10.0f, kCorruptionRacketeering, now_owner);
    } else if (!new_b && old_b) {
        cancel_corruption_transition(planet);
        if (p.corruption != kCorruptionNone) begin_removal_raw(planet, 10.0f);
    }
    // 9. a siphon by the new owner ends.
    if (p.siphon_player == now_owner) {
        p.siphon_player = kNoPlayer;
        p.siphon_amount = 0.0f;
    }
    // GE 2.8: the space queue is re-evaluated at level 0 [M]. The ground queue's entries of other players are dropped
    // without refund ([L]: 0x2bff60 not traced).
    reevaluate_queue(planet, kSpaceQueue, 0);
    // a2j t06 (A2J_T03_INCOME.md §2) [M]: the starbase owner D +0x13c is reset only together with the starbase handle
    // (0x4bda40, BATTLE_TRANSITIONS §3.3a; Change_Owner 0x3f7280 and the revolt 0x3f98a0 call it, GE 2.8 / 4.2). The
    // derived-owner change 0x3fb040 (GE 4.2 "On an owner change") does not touch it, so a starbase that outlives the
    // owner change keeps its owner. Without a starbase object the field is already -1 (every destroy path clears it).
    if (!store_->alive(p.starbase)) p.starbase_owner = kNoPlayer;
    auto& g = p.queue[kGroundQueue];
    const bool head_gone = !g.empty() && g[0].player != now_owner;
    for (const BuildEntry& e : g)
        if (e.player != now_owner) emit_production(GalacticEventKind::ProductionCanceled, planet, kGroundQueue, e, 0);
    g.erase(std::remove_if(g.begin(), g.end(), [now_owner](const BuildEntry& e) { return e.player != now_owner; }),
            g.end());
    if (head_gone && !g.empty()) restart_head(g[0]);
}

// ---- production -----------------------------------------------------------------------------------------------

int32_t GalacticSim::queue_kind_of(const TypeInfo& t) const {
    // GE 2.1 [H]: ground = Required_Ground_Base_Level != 0 or a ground structure; space = needs a starbase
    // level or is a level-1 base type; anything else cannot be queued.
    // OI galactic: the dummy behaviour (0x5F ground structure, 0x5C starbase) when known (SG 1.3 step 5).
    if (t.required_ground_base != 0 || is_ground_structure_type(t)) return kGroundQueue;
    if (t.required_star_base != 0 || (is_star_base_type(t) && t.base_level == 1)) return kSpaceQueue;
    return -1;
}

bool GalacticSim::prerequisites_met(int32_t player, PlanetIndex planet, const TypeInfo& t, bool at_enqueue) const {
    // Re-evaluation (at_enqueue = false): affiliation, player tech level, locks. Enqueue / buildable use the full
    // buildability order (build_check, OI galactic, SG 1.3).
    if (at_enqueue) return build_check(player, planet, static_cast<uint32_t>(&t - rules_->types.data())).ok;
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (!t.affiliation.empty() && std::find(t.affiliation.begin(), t.affiliation.end(), ps.faction) == t.affiliation.end())
        return false;
    // Tech level (REMAINING_GAPS finding 3: no unlocked-list bypass; unlocks only release Build_Initially_Locked).
    if (t.tech_level > ps.tech_level) return false;
    // P4 sim: Lock_Tech list (hack step 1 / story LOCK_UNIT / Lua Lock_Tech) and Build_Initially_Locked.
    if (build_locked(player, static_cast<uint32_t>(&t - rules_->types.data()))) return false;
    (void)planet;
    return true;
}

float GalacticSim::build_time_for(int32_t player, PlanetIndex planet, uint32_t type) const {
    const TypeInfo& t = rules_->types[type];
    const PlanetState& p = planets_[planet];
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    const FactionRules& fr = faction_of(player);
    econ::BuildTimeInputs in;
    in.build_time_seconds = t.build_time_seconds;
    in.production_speed_factor = rules_->constants.production_speed_factor;
    in.planet_bonus = p.build_time_bonus;
    in.tech_factor_applies = t.build_cost >= 1 && t.kind != UnitKind::GroundStructure;
    in.starbase_level = p.starbase_level;
    in.tech_level = t.tech_level;
    in.base_vs_tech = &rules_->constants.base_vs_tech;
    if (ps.ai && !ps.human && ps.difficulty < rules_->difficulties.size())
        in.ai_multiplier = rules_->difficulties[ps.difficulty].galactic_build_time_multiplier;
    if (!fr.benefits_from_corruption) in.corruption_multiplier = fr.corruption_time_mult[corrupted(planet) ? 1 : 0];
    if (t.reduced_by_factories) {
        int32_t count = 0;
        for (uint32_t req : t.required_special_structures) {
            for (ObjectHandle s : p.structures)
                if (store_->alive(s) && store_->type(s) == req) ++count;
            if (store_->alive(p.starbase) && store_->type(p.starbase) == req) ++count;
        }
        in.factories = std::max(1, count);
    }
    in.cheat = ps.cheat;
    return econ::build_time(in);
}

int32_t GalacticSim::build_cost_for(int32_t player, PlanetIndex planet, uint32_t type) const {
    const TypeInfo& t = rules_->types[type];
    const FactionRules& fr = faction_of(player);
    const float m_corr = fr.benefits_from_corruption ? 1.0f : fr.corruption_price_mult[corrupted(planet) ? 1 : 0];
    const float m_tech = t.advances_tech != 0 ? std::max(0.0001f, players_[static_cast<size_t>(player)].tech_cost_mult) : 1.0f;
    return econ::build_cost(t.build_cost, 1.0f - planets_[planet].build_cost_bonus, m_corr, m_tech);
}

void GalacticSim::restart_head(BuildEntry& e) const {
    e.start = frame_;
    e.duration = econ::timer_frames(e.build_time);
    e.remaining = econ::timer_remaining_seconds(e.start, e.duration, frame_);
}

RejectReason GalacticSim::check_enqueue(int32_t player, PlanetIndex planet, uint32_t type, int32_t* cost_out) const {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!valid_planet(planet)) return RejectReason::InvalidTarget;
    if (type >= rules_->types.size()) return RejectReason::Malformed;
    const TypeInfo& t = rules_->types[type];
    if (t.build_cost < 1) return RejectReason::NotAllowed;  // [H]
    const int32_t q = queue_kind_of(t);
    if (q < 0) return RejectReason::NotAllowed;
    // OI galactic: buildability 0x2804d0 in the SG 1.3 order (steps 1-5 -> NotAllowed, 10 -> NotOwner, else
    // PrerequisitesUnmet).
    const BuildCheck bc = build_check(player, planet, type, false);
    if (!bc.ok) return bc.step <= 5 ? RejectReason::NotAllowed : bc.step == 10 ? RejectReason::NotOwner : RejectReason::PrerequisitesUnmet;
    const auto& queue = planets_[planet].queue[static_cast<size_t>(q)];
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (ps.human && static_cast<int32_t>(queue.size()) >= rules_->constants.max_build_queue) return RejectReason::QueueFull;
    // 3. population legality 0x2ac320 (A2 galactic, AI_GALACTIC_3 §3.5): humans, or AI with AI_Uses_Galactic_Pop_Cap.
    if (population_capped(player) && t.population > population_room(player)) return kRejectPopulationCap;
    const int32_t cost = build_cost_for(player, planet, type);
    const bool payg = pays_as_you_go(player);
    if (!payg && ps.account.credits < static_cast<float>(cost)) return RejectReason::CannotAfford;  // [M]
    if (cost_out) *cost_out = cost;
    return RejectReason::None;
}

RejectReason GalacticSim::enqueue(int32_t player, PlanetIndex planet, uint32_t type) {
    int32_t cost = 0;
    const RejectReason why = check_enqueue(player, planet, type, &cost);
    if (why != RejectReason::None) return why;
    const int32_t q = queue_kind_of(rules_->types[type]);
    auto& queue = planets_[planet].queue[static_cast<size_t>(q)];
    const bool payg = pays_as_you_go(player);
    BuildEntry e;
    e.type = type;
    e.build_time = build_time_for(player, planet, type);
    e.player = player;
    e.cost = static_cast<float>(cost);
    restart_head(e);  // timer starts at the current frame for T [H]
    if (!payg) add_credits(player, -e.cost, true);
    queue.push_back(e);
    emit_production(GalacticEventKind::ProductionBegin, planet, static_cast<uint32_t>(q), e, -1);
    return RejectReason::None;
}

RejectReason GalacticSim::cancel(int32_t player, PlanetIndex planet, uint32_t queue, uint32_t index, bool refund) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!valid_planet(planet) || queue > 1) return RejectReason::InvalidTarget;
    auto& qv = planets_[planet].queue[queue];
    if (index >= qv.size()) return RejectReason::InvalidTarget;
    if (qv[index].player != player) return RejectReason::NotOwner;
    const BuildEntry e = qv[index];
    qv.erase(qv.begin() + static_cast<std::ptrdiff_t>(index));
    if (refund)  // through Add_Credits with notify: AI refunds get Credit_Multiplier again (quirk) [H]
        add_credits(e.player, econ::refund_amount(pays_as_you_go(e.player), e.remaining, e.build_time, e.cost), true);
    if (index == 0 && !qv.empty()) restart_head(qv[0]);
    emit_production(GalacticEventKind::ProductionCanceled, planet, queue, e, refund ? 1 : 0);
    return RejectReason::None;
}

void GalacticSim::reevaluate_queue(PlanetIndex planet, uint32_t queue, int32_t level) {
    if (!valid_planet(planet) || queue > 1) return;
    auto& qv = planets_[planet].queue[queue];
    for (size_t i = qv.size(); i-- > 0;) {
        const BuildEntry& e = qv[i];
        const TypeInfo& t = rules_->types[e.type];
        const int32_t required = queue == kGroundQueue ? t.required_ground_base : t.required_star_base;
        if (required <= level && valid_player(e.player) && prerequisites_met(e.player, planet, t, false)) continue;
        emit_production(GalacticEventKind::ProductionCanceled, planet, queue, e, 0);
        qv.erase(qv.begin() + static_cast<std::ptrdiff_t>(i));  // no refund [M]
        if (i == 0) {
            if (!qv.empty()) restart_head(qv[0]);
            break;  // deleting the head stops the walk [H]
        }
    }
}

void GalacticSim::pop_head(PlanetIndex planet, uint32_t queue) {
    auto& qv = planets_[planet].queue[queue];
    qv.erase(qv.begin());
    if (!qv.empty()) restart_head(qv[0]);
}

bool GalacticSim::enemy_fleet_in_orbit(PlanetIndex planet, int32_t player) const {
    for (uint32_t f : planets_[planet].orbit) {
        const FleetState& fl = fleets_[f];
        if (!fl.alive || !hostile(fl.owner, player)) continue;  // visibility not modelled: all visible
        for (ObjectHandle m : fl.contents)
            if (store_->alive(m) && rules_->types[store_->type(m)].ai_combat_power > 0.0f) return true;
    }
    return false;
}

bool GalacticSim::contested_surface(PlanetIndex planet) const {
    // GE 2.5 step 3 presence checks 0x3f9270 / 0x3f9220 [L]: two different players on the surface.
    int32_t first = kNoPlayer;
    for (ObjectHandle u : planets_[planet].surface) {
        if (!store_->alive(u)) continue;
        if (first == kNoPlayer) first = store_->owner(u);
        else if (store_->owner(u) != first) return true;
    }
    return false;
}

void GalacticSim::service_production(PlanetIndex planet) {
    PlanetState& p = planets_[planet];
    const int32_t now = frame_;
    for (uint32_t q = 0; q < 2; ++q) {
        auto& qv = p.queue[q];
        if (qv.empty()) continue;
        BuildEntry& e = qv[0];
        const TypeInfo& t = rules_->types[e.type];
        // 1. starbase upgrade head without an owned starbase: dropped, no refund, the service returns [H/M].
        if (t.kind == UnitKind::StarBase && t.base_level > 1 && p.starbase_owner != e.player) {
            emit_production(GalacticEventKind::ProductionCanceled, planet, q, e, 0);
            pop_head(planet, q);
            return;
        }
        // 2. starbase in production with an enemy fleet in orbit (planet not in battle) [H].
        if (t.kind == UnitKind::StarBase && !p.in_battle && enemy_fleet_in_orbit(planet, e.player)) {
            emit_production(GalacticEventKind::ProductionCanceled, planet, q, e, 0);
            pop_head(planet, q);
            return;
        }
        const bool payg = pays_as_you_go(e.player);
        const float rem = econ::timer_remaining_seconds(e.start, e.duration, now);
        const bool done = payg ? rem == 0.0f : econ::timer_due(e.start, e.duration, now);
        if (done) {
            // Ground cap stall [L]: the head waits.
            if (q == kGroundQueue && static_cast<int32_t>(p.surface.size()) >= rules_->constants.max_ground_forces_on_planet &&
                contested_surface(planet))
                continue;
            const BuildEntry finished = e;
            pop_head(planet, q);  // pop, then create; the next entry's timer starts now
            complete_build(planet, finished);
            continue;
        }
        if (!payg) {
            e.remaining = rem;  // countdown advances (speech at 60 s / 10 s is UI)
            continue;
        }
        // Pay as you go: installment for the time that passed [H]. An AI player pays through its AI budget (0x53b900,
        // q2f t01 [H]: the hook decides how much, the credits floor at 0); anyone else needs credits >= a.
        const float a = econ::payg_installment(e.remaining, rem, e.build_time, e.cost);
        const PlayerState& owner = players_[static_cast<size_t>(e.player)];
        float pay = -1.0f;
        if (installment_hook_ && owner.ai && !owner.human)
            pay = installment_hook_(planet, q, e, a);
        else if (owner.account.credits >= a)
            pay = a;
        if (pay >= 0.0f) {
            add_credits(e.player, -pay, false);
            e.remaining = rem;
        } else {
            e.start = now;  // re-armed from now with the cached remaining time: paused (0x499090)
            e.duration = econ::timer_frames(e.remaining);
        }
    }
}

// ProductionBehavior::Service (AI_GALACTIC_3 §5): the AI market of the planet (A2 galactic hook), then its queues.
void GalacticSim::planet_production(PlanetIndex planet) {
    if (production_hook_) production_hook_(planet);
    if (valid_planet(planet)) service_production(planet);
}

void GalacticSim::complete_build(PlanetIndex planet, const BuildEntry& e) {
    const TypeInfo& t = rules_->types[e.type];
    const uint32_t q = static_cast<uint32_t>(std::max(queue_kind_of(t), 0));
    count_build(e.player, e.type);  // OI galactic: 0x2821b0, the lifetime build count (SG 1.3 step 22)
    if (t.advances_tech != 0) {  // tech upgrade (0x288980)
        // REMAINING_GAPS 2: the level becomes the item's value (clamped to the max tech), with the unlock sweep.
        emit_production(GalacticEventKind::ProductionCompleted, planet, q, e, 0);
        set_tech_level(e.player, t.advances_tech, true);
        return;
    }
    last_spawn_ = {};
    const bool created = create_unit(e.player, planet, e.type);
    const int32_t made = created && store_->exists(last_spawn_) ? static_cast<int32_t>(store_->id(last_spawn_)) : 0;
    emit_production(GalacticEventKind::ProductionCompleted, planet, q, e, made);
    if (created && t.kind == UnitKind::StarBase) {
        GalacticEvent ev;
        ev.kind = GalacticEventKind::StarbaseLevelChanged;
        ev.player = e.player;
        ev.planet = planet;
        ev.type = e.type;
        ev.value = planets_[planet].starbase_level;
        emit(ev);
    }
    if (!created && t.kind != UnitKind::StarBase && t.kind != UnitKind::GroundStructure)
        add_credits(e.player, econ::refund_amount(pays_as_you_go(e.player), e.remaining, e.build_time, e.cost), true);
    // (The paid cost stored on the object, +0x32c, feeds sell-back and score: not modelled.)
}

// ---- objects -------------------------------------------------------------------------------------------------

ObjectHandle GalacticSim::spawn(int32_t player, PlanetIndex planet, uint32_t type, ObjectHandle container) {
    SpawnParams sp;
    sp.type = type;
    sp.owner = player;
    sp.position = planets_[planet].position;
    sp.container = container;
    last_spawn_ = store_->create(sp);
    oi_on_spawn(last_spawn_);  // OI galactic: Galactic_Automatic stealth from creation (SG 2.2)
    return last_spawn_;
}

bool GalacticSim::create_unit(int32_t player, PlanetIndex planet, uint32_t type) {
    PlanetState& p = planets_[planet];
    const TypeInfo& t = rules_->types[type];
    switch (t.kind) {
    case UnitKind::StarBase: {
        if (store_->alive(p.starbase)) store_->destroy(p.starbase);  // an upgrade replaces the base
        p.starbase = spawn(player, planet, type, p.object);
        p.starbase_level = t.base_level;
        p.starbase_owner = player;
        return true;
    }
    case UnitKind::GroundStructure:
        p.structures.push_back(spawn(player, planet, type, p.object));
        return true;
    case UnitKind::GroundUnit:
        // OI galactic (SG 1.7): a company with a Company_Transport_Unit is placed as a transport (surface or a new
        // orbit fleet); without one the former surface placement stays [port].
        if (t.company_transport < rules_->types.size()) return place_company(player, planet, type);
        p.surface.push_back(spawn(player, planet, type, {}));
        return true;
    case UnitKind::SpaceUnit:
    case UnitKind::Squadron: {
        const uint32_t f = orbit_fleet(player, planet);
        const ObjectHandle h = spawn(player, planet, type, {});
        fleets_[f].contents.push_back(h);
        fleets_[f].carried.push_back(0);
        return true;
    }
    default:
        return false;
    }
}

uint32_t GalacticSim::create_fleet(int32_t player, PlanetIndex planet) {
    SpawnParams sp;
    sp.type = objects::kNone;
    sp.owner = player;
    sp.position = planets_[planet].position;
    FleetState f;
    f.alive = true;
    f.object = store_->create(sp);
    store_->add_behavior(f.object, beh::Fleet);
    store_->add_behavior(f.object, beh::Locomotor);
    f.owner = player;
    f.planet = planet;
    f.position = planets_[planet].position;
    const uint32_t idx = static_cast<uint32_t>(fleets_.size());
    if (fleet_by_slot_.size() <= f.object.slot()) fleet_by_slot_.resize(f.object.slot() + 1, kNoFleet);
    fleet_by_slot_[f.object.slot()] = idx;
    fleets_.push_back(std::move(f));
    planets_[planet].orbit.push_back(idx);
    settle_slots(planet);  // the owner's lowest free orbit slot
    return idx;
}

uint32_t GalacticSim::orbit_fleet(int32_t player, PlanetIndex planet) {
    for (uint32_t f : planets_[planet].orbit)
        if (fleets_[f].alive && fleets_[f].owner == player && !fleets_[f].moving) return f;
    return create_fleet(player, planet);
}

void GalacticSim::remove_from_orbit(PlanetIndex planet, uint32_t fleet) {
    if (planet >= planets_.size()) return;
    erase_value(planets_[planet].orbit, fleet);
    fleets_[fleet].slot = -1;
    settle_slots(planet);  // an unslotted fleet may take the slot just freed
}

void GalacticSim::kill_fleet(uint32_t fleet) {
    FleetState& f = fleets_[fleet];
    if (!f.alive) return;
    remove_from_orbit(f.planet, fleet);
    f.alive = false;
    f.moving = false;
    store_->destroy(f.object);
}

PlanetIndex GalacticSim::location_of(ObjectHandle object) const {
    const uint32_t fl = fleet_of(object);
    if (fl != kNoFleet) return fleets_[fl].planet;
    for (PlanetIndex i = 0; i < planets_.size(); ++i) {
        const PlanetState& p = planets_[i];
        if (p.starbase == object || std::find(p.structures.begin(), p.structures.end(), object) != p.structures.end() ||
            std::find(p.surface.begin(), p.surface.end(), object) != p.surface.end())
            return i;
    }
    for (const FleetState& f : fleets_)
        if (f.alive && std::find(f.contents.begin(), f.contents.end(), object) != f.contents.end()) return f.planet;
    return kInvalidPlanet;
}

void GalacticSim::emit_production(GalacticEventKind kind, PlanetIndex planet, uint32_t queue, const BuildEntry& e,
                                  int32_t extra) {
    if (!emit_events_) return;
    GalacticEvent ev;
    ev.kind = kind;
    ev.player = e.player;
    ev.planet = planet;
    ev.type = e.type;
    ev.value = static_cast<int32_t>(queue);
    if (kind == GalacticEventKind::ProductionCanceled) ev.other_player = extra;
    if (kind == GalacticEventKind::ProductionCompleted) ev.object = extra;
    emit(ev);
}

bool GalacticSim::destroy_object(ObjectHandle object) {
    if (!store_->alive(object)) return false;
    auto destroyed = [this](ObjectHandle h, PlanetIndex at, bool fleet) {
        if (!emit_events_ || !store_->alive(h)) return;
        GalacticEvent ev;
        ev.kind = GalacticEventKind::ObjectDestroyed;
        ev.object = static_cast<int32_t>(store_->id(h));
        ev.type = fleet ? kNoIndex : static_cast<uint32_t>(store_->type(h));
        ev.player = store_->owner(h);
        ev.planet = at;
        emit(ev);
    };
    const uint32_t fl = fleet_of(object);
    if (fl != kNoFleet) {
        for (ObjectHandle m : fleets_[fl].contents) destroyed(m, fleets_[fl].planet, false);
        destroyed(object, fleets_[fl].planet, true);
        for (ObjectHandle m : fleets_[fl].contents) store_->destroy(m);
        kill_fleet(fl);
        return true;
    }
    destroyed(object, emit_events_ ? location_of(object) : kInvalidPlanet, false);
    for (PlanetIndex pi = 0; pi < planets_.size(); ++pi) {
        PlanetState& p = planets_[pi];
        if (p.starbase == object) {
            p.starbase = {};
            p.starbase_level = 0;
            GalacticEvent ev;
            ev.kind = GalacticEventKind::StarbaseLevelChanged;
            ev.player = p.starbase_owner;
            ev.planet = pi;
            ev.value = 0;
            emit(ev);
            p.starbase_owner = kNoPlayer;
        }
        erase_value(p.structures, object);
        erase_value(p.surface, object);
    }
    for (uint32_t i = 0; i < fleets_.size(); ++i) {
        FleetState& f = fleets_[i];
        if (!f.alive) continue;
        bool removed = false;
        for (size_t k = 0; k < f.contents.size() && !removed; ++k)
            if (f.contents[k] == object) {
                f.contents.erase(f.contents.begin() + static_cast<std::ptrdiff_t>(k));
                f.carried.erase(f.carried.begin() + static_cast<std::ptrdiff_t>(k));
                removed = true;
            }
        if (removed && f.contents.empty()) kill_fleet(i);
    }
    return store_->destroy(object);
}

RejectReason GalacticSim::launch_units(int32_t player, PlanetIndex planet, std::span<const ObjectHandle> units) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!valid_planet(planet)) return RejectReason::InvalidTarget;
    PlanetState& p = planets_[planet];
    for (ObjectHandle u : units)
        if (std::find(p.surface.begin(), p.surface.end(), u) == p.surface.end() || store_->owner(u) != player)
            return RejectReason::InvalidUnit;
    if (units.empty()) return RejectReason::None;
    const uint32_t f = orbit_fleet(player, planet);
    for (ObjectHandle u : units) {
        erase_value(p.surface, u);
        fleets_[f].contents.push_back(u);
        fleets_[f].carried.push_back(0);
        launch_reveal(planet, u);
    }
    return RejectReason::None;
}

RejectReason GalacticSim::invade(int32_t player, PlanetIndex planet, std::span<const ObjectHandle> units) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (!valid_planet(planet)) return RejectReason::InvalidTarget;
    if (!surface_accessible(planet)) return RejectReason::NotAllowed;  // Invade 0x6ce600 / Land_Units 0x6cec80: 0x375220
    PlanetState& p = planets_[planet];
    bool defended = false;
    for (ObjectHandle s : p.structures)
        if (store_->alive(s) && hostile(store_->owner(s), player)) defended = true;
    for (ObjectHandle s : p.surface)
        if (store_->alive(s) && hostile(store_->owner(s), player)) defended = true;
    if (defended && !conflicts_) return RejectReason::NotAllowed;  // needs a battle [L]
    std::vector<ObjectHandle> landing;
    for (uint32_t f : fleets_at(planet, player)) {
        if (fleets_[f].moving) continue;
        for (ObjectHandle m : fleets_[f].contents) {
            if (!store_->alive(m) || rules_->types[store_->type(m)].kind != UnitKind::GroundUnit) continue;
            if (units.empty() || std::find(units.begin(), units.end(), m) != units.end()) landing.push_back(m);
        }
    }
    if (landing.empty()) return RejectReason::InvalidUnit;
    if (defended)  // S10: Land_Invade queues a land conflict (BATTLE_TRANSITIONS §1.3)
        return conflicts_->on_invade(player, planet, landing) ? RejectReason::None : RejectReason::NotAllowed;
    for (ObjectHandle m : landing) {
        for (uint32_t f : fleets_at(planet, player)) {
            FleetState& fl = fleets_[f];
            for (size_t k = 0; k < fl.contents.size(); ++k)
                if (fl.contents[k] == m) {
                    fl.contents.erase(fl.contents.begin() + static_cast<std::ptrdiff_t>(k));
                    fl.carried.erase(fl.carried.begin() + static_cast<std::ptrdiff_t>(k));
                    break;
                }
            if (fl.contents.empty()) kill_fleet(f);
        }
        p.surface.push_back(m);
    }
    GalacticEvent ev;
    ev.kind = GalacticEventKind::UnitsLanded;
    ev.player = player;
    ev.planet = planet;
    ev.value = static_cast<int32_t>(landing.size());
    emit(ev);
    return RejectReason::None;
}

// ---- fleets -------------------------------------------------------------------------------------------------

RejectReason GalacticSim::move_fleet(int32_t player, uint32_t fleet, PlanetIndex destination) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (fleet >= fleets_.size() || !fleets_[fleet].alive) return RejectReason::InvalidUnit;
    FleetState& f = fleets_[fleet];
    if (f.owner != player) return RejectReason::NotOwner;
    if (f.moving || f.planet == kInvalidPlanet) return RejectReason::NotAllowed;  // GE 3.1 step 1 [H]
    if (!valid_planet(destination)) return RejectReason::InvalidTarget;
    // Player orders path with every request flag set (move mode 5) [M]: the event carries no mode.
    PathRequest req;
    req.source = f.planet;
    req.destination = destination;
    const PlayerView view = player_view(player);
    std::vector<PlanetIndex> path;
    if (!find_fleet_path(graph_, view, req, scratch_, path)) return RejectReason::InvalidTarget;
    return begin_move(fleet, std::move(path));
}

RejectReason GalacticSim::move_fleet_path(int32_t player, uint32_t fleet, std::span<const PlanetIndex> path) {
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (fleet >= fleets_.size() || !fleets_[fleet].alive) return RejectReason::InvalidUnit;
    FleetState& f = fleets_[fleet];
    if (f.owner != player) return RejectReason::NotOwner;
    if (f.moving || f.planet == kInvalidPlanet) return RejectReason::NotAllowed;
    std::vector<PlanetIndex> p(path.begin(), path.end());
    if (p.empty() || p.front() != f.planet) p.insert(p.begin(), f.planet);
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        if (!valid_planet(p[i + 1])) return RejectReason::InvalidTarget;
        bool adjacent = false;
        for (uint32_t e = graph_.edge_begin(p[i]); e < graph_.edge_end(p[i]); ++e)
            if (graph_.neighbour(e) == p[i + 1]) adjacent = true;
        if (!adjacent) return RejectReason::InvalidTarget;
    }
    return begin_move(fleet, std::move(p));
}

RejectReason GalacticSim::move_fleet_direct(int32_t player, uint32_t fleet, PlanetIndex destination) {
    // BATTLE_TRANSITIONS §3.3a [H]: 0x2ba6c0 builds the path [P, destination] and hands it to the move-order handler
    // 0x39f380 -> departure 0x431c80 (GE 3.1). No A* / trade-route test: the hop is a straight line; depart_hop gives
    // it the route factor only when the two planets are joined (0x3f7b30), otherwise 1.0 (GE 3.2).
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    if (fleet >= fleets_.size() || !fleets_[fleet].alive) return RejectReason::InvalidUnit;
    const FleetState& f = fleets_[fleet];
    if (f.owner != player) return RejectReason::NotOwner;
    if (f.moving || f.planet == kInvalidPlanet) return RejectReason::NotAllowed;  // GE 3.1 step 1
    if (!valid_planet(destination)) return RejectReason::InvalidTarget;
    if (destination == f.planet) return RejectReason::NotAllowed;  // destination = current planet: stays
    return begin_move(fleet, {f.planet, destination});            // base speed 0 -> NotAllowed, stays in orbit
}

RejectReason GalacticSim::begin_move(uint32_t fleet, std::vector<PlanetIndex> path) {
    if (path.size() < 2) return RejectReason::None;  // already there
    const PlanetIndex origin = path[0];
    {
        // 2. carrier simulation [M split, L fill order]: holds = sum of Squadron_Capacity; non-hyperspace
        // squadrons by cost (descending), then hyperspace squadrons by speed (ascending) take one slot each;
        // non-hyperspace squadrons that do not fit stay behind in a new fleet.
        FleetState& f = fleets_[fleet];
        int32_t holds = 0;
        std::vector<uint32_t> nonhs, hs;
        for (uint32_t i = 0; i < f.contents.size(); ++i) {
            const TypeInfo& t = rules_->types[store_->type(f.contents[i])];
            holds += t.squadron_capacity;
            f.carried[i] = 0;
            if (t.kind == UnitKind::Squadron) (t.hyperspace ? hs : nonhs).push_back(i);
        }
        auto ti = [this, &f](uint32_t i) -> const TypeInfo& { return rules_->types[store_->type(f.contents[i])]; };
        std::stable_sort(nonhs.begin(), nonhs.end(), [&](uint32_t a, uint32_t b) { return ti(a).build_cost > ti(b).build_cost; });
        std::stable_sort(hs.begin(), hs.end(), [&](uint32_t a, uint32_t b) { return ti(a).hyperspace_speed < ti(b).hyperspace_speed; });
        std::vector<uint32_t> stay;
        for (uint32_t i : nonhs) {
            if (holds > 0) {
                f.carried[i] = 1;
                --holds;
            } else {
                stay.push_back(i);
            }
        }
        for (uint32_t i : hs)
            if (holds > 0) {
                f.carried[i] = 1;
                --holds;
            }
        if (!stay.empty() && stay.size() < f.contents.size()) {
            std::sort(stay.begin(), stay.end());
            std::vector<ObjectHandle> left;
            for (uint32_t i : stay) left.push_back(f.contents[i]);
            for (size_t k = stay.size(); k-- > 0;) {
                f.contents.erase(f.contents.begin() + static_cast<std::ptrdiff_t>(stay[k]));
                f.carried.erase(f.carried.begin() + static_cast<std::ptrdiff_t>(stay[k]));
            }
            const int32_t owner = f.owner;
            const uint32_t nf = create_fleet(owner, origin);  // may reallocate fleets_
            for (ObjectHandle h : left) {
                fleets_[nf].contents.push_back(h);
                fleets_[nf].carried.push_back(0);
            }
        }
    }
    FleetState& f = fleets_[fleet];
    // 3. base speed = min Hyperspace_Speed over hyperspace-capable, not carried members [H]. OI galactic (SG 1.7):
    // a company counts with its transport type (transports are not looked through).
    float slowest = 0.0f;
    bool any = false;
    for (size_t i = 0; i < f.contents.size(); ++i) {
        const TypeInfo& t = rules_->types[travel_type(f.contents[i])];
        if (!t.hyperspace || f.carried[i]) continue;
        slowest = any ? std::min(slowest, t.hyperspace_speed) : t.hyperspace_speed;
        any = true;
    }
    if (!any || !(slowest > 0.0f)) return RejectReason::NotAllowed;  // cannot jump
    // 4. v0 = s * Hyperspace_Speed_Factor * M (SG 2.1) [H]; M = 1.0 [port] (strategic.cpp).
    f.base_speed = econ::base_speed(slowest, faction_of(f.owner).hyperspace_speed_factor, fleet_speed_multiplier());
    f.path = std::move(path);
    f.path_index = 1;
    depart_hop(fleet, origin, f.path[1], true);
    // 7. piracy (FoC) [H]: Piracy-corrupted origin, a corrupting player not allied with the mover, the fleet not
    // stealthed (0x3a9760, OI galactic SG 2.2).
    PlanetState& o = planets_[origin];
    FleetState& g = fleets_[fleet];
    if (o.corruption == kCorruptionPiracy && valid_player(o.corrupting_player) && o.corrupting_player != g.owner &&
        !hidden(g.object)) {
        float value = 0.0f;
        for (ObjectHandle m : g.contents) value += rules_->types[store_->type(m)].piracy_value;
        o.piracy_pool += value;
    }
    return RejectReason::None;
}

void GalacticSim::depart_hop(uint32_t fleet, PlanetIndex from, PlanetIndex to, bool leave_orbit) {
    FleetState& f = fleets_[fleet];
    // 5. hop factor R (0x397470) and 6. hop speed [H].
    const uint32_t r = route_between(from, to);
    const float r_factor = econ::hop_factor(benefits(f.owner), corrupted(from), corrupted(to), r != kNoRoute,
                                            r != kNoRoute ? map_.routes.hs_speed_factor[r] : 0.0f,
                                            rules_->constants.corruption_hyperspace_bonus);
    f.hop_speed = econ::hop_speed(f.base_speed, r_factor);
    f.from = from;
    f.to = to;
    f.moving = true;
    if (emit_events_) {
        GalacticEvent ev;
        ev.kind = leave_orbit ? GalacticEventKind::FleetDeparted : GalacticEventKind::FleetBounced;
        ev.fleet = fleet;
        ev.object = static_cast<int32_t>(store_->id(f.object));
        ev.player = f.owner;
        ev.planet = from;
        ev.other_planet = to;
        emit(ev);
    }
    if (leave_orbit) {
        remove_from_orbit(from, fleet);
        f.planet = kInvalidPlanet;
        // The origin's production is re-evaluated with its current levels (0x4358c0) [H].
        reevaluate_queue(from, kGroundQueue, ground_base_level(from));
        reevaluate_queue(from, kSpaceQueue, planets_[from].starbase_level);
    }
    f.planet = kInvalidPlanet;
}

void GalacticSim::service_fleet(uint32_t fleet) {
    FleetState& f = fleets_[fleet];
    if (!f.alive || !f.moving) return;
    // Turning toward the target is not modelled: the facing snaps to the hop direction [M].
    const bool arrived = econ::fleet_step(f.position, planets_[f.to].position, f.hop_speed, &f.facing);
    store_->set_position(f.object, f.position);
    store_->set_facing(f.object, f.facing);
    if (arrived) arrive(fleet);
}

bool GalacticSim::orbit_conflict(PlanetIndex planet, int32_t mover) const {
    // Enter-orbit conflict test (0x4338d0) [M]: a hostile combat fleet in orbit or a hostile starbase.
    const PlanetState& p = planets_[planet];
    if (p.starbase_level > 0 && hostile(p.starbase_owner, mover)) return true;
    return enemy_fleet_in_orbit(planet, mover);
}

void GalacticSim::merge_fleet(uint32_t from, uint32_t into) {
    FleetState& a = fleets_[from];
    FleetState& b = fleets_[into];
    for (size_t i = 0; i < a.contents.size(); ++i) {
        b.contents.push_back(a.contents[i]);
        b.carried.push_back(0);
    }
    a.contents.clear();
    a.carried.clear();
    kill_fleet(from);  // (merge_fleets event: notification only)
}

void GalacticSim::arrive(uint32_t fleet) {
    FleetState& f = fleets_[fleet];
    const PlanetIndex at = f.to, prev = f.from;
    PlanetState& p = planets_[at];
    f.moving = false;
    // 1. inaccessible planet: turn back to the previous hop and stop there [H].
    if (p.hidden && (!players_[static_cast<size_t>(f.owner)].human || !p.in_battle)) {
        f.path = {at, prev};
        f.path_index = 1;
        depart_hop(fleet, at, prev, false);
        return;
    }
    // 2. enter orbit.
    f.planet = at;
    p.orbit.push_back(fleet);
    const bool conflict = orbit_conflict(at, f.owner);
    const bool intermediate = f.path_index + 1 < f.path.size();
    const bool pass = intermediate && (!conflict || benefits(f.owner));
    if (emit_events_) {
        GalacticEvent ev;
        ev.kind = GalacticEventKind::FleetArrived;
        ev.fleet = fleet;
        ev.object = static_cast<int32_t>(store_->id(f.object));
        ev.player = f.owner;
        ev.planet = at;
        ev.other_planet = prev;
        ev.value = pass ? kArrivePassThrough : intermediate ? kArriveIntercepted : kArriveFinal;
        emit(ev);
    }
    // 3. intermediate hop: continue unless intercepted; Underworld (benefits from corruption) runs through [H].
    if (pass) {
        ++f.path_index;
        depart_hop(fleet, at, f.path[f.path_index], true);
        return;
    }
    // 4. final hop or stopped: speed 0, path cleared, merge into a friendly fleet already in orbit [H].
    f.hop_speed = 0.0f;
    f.path.clear();
    f.path_index = 0;
    const int32_t mover = f.owner;
    // Playtest 3 t07: a slot requested for this planet (FleetManagement op 8 with a destination): the fleet merges into
    // the owner's fleet in that slot, or takes the slot when it is free. Without a request it merges into the first
    // friendly fleet in orbit and otherwise takes the lowest free slot.
    const int32_t want = f.want_planet == at ? f.want_slot : -1;
    f.want_planet = kInvalidPlanet;
    f.want_slot = -1;
    if (conflict && conflicts_) conflicts_->on_space_conflict(at, mover);  // S10: Initiate_Space_Conflict
    uint32_t into = kNoFleet;
    if (want >= 0) {
        into = slot_occupant(at, mover, want, fleet);
    } else {
        for (uint32_t g : p.orbit)
            if (g != fleet && fleets_[g].alive && fleets_[g].owner == mover && !fleets_[g].moving) {
                into = g;
                break;
            }
    }
    if (into != kNoFleet) {
        merge_fleet(fleet, into);
        return;
    }
    if (want >= 0) place_in_slot(fleet, want);
    settle_slots(at);
}

// ---- frame --------------------------------------------------------------------------------------------------

void GalacticSim::update() {
    sync_starbases();  // refix t05: no planet keeps a starbase level without its starbase object
    reveal_prestep();  // GRC 1.3: first in the galactic frame, also while paused (step 2 tests the pause)
    if (paused()) {  // 0x4b2360: the pause counter or the story block; mode frame frozen, behaviours not serviced [H]
        // A1_READINGS §31.7 (a1j t05): 0x2be640 gates the hero spawn lists (0x3fc750) on the counter alone, so they run
        // under the story block only.
        if (pause_ == 0) service_respawns();
        // S10: the conflict tail still runs; the story block's skip of the state machines is the hook's (0x2be640 step 2).
        if (conflicts_) conflicts_->on_service_tail();
        return;
    }
    // 1. Object services, newest registered first; only objects registered before the loop [M].
    const std::span<const ObjectHandle> serviced = store_->serviced();
    std::vector<ObjectHandle> order(serviced.rbegin(), serviced.rend());
    // Owner derivation is pure per planet. When no fleet services after a planet (always the case once the
    // planets are created first), fleets run first and the derivation can fan out; else stay interleaved.
    bool split = true, seen_planet = false;
    for (ObjectHandle h : order) {
        if (planet_of(h) != kInvalidPlanet) seen_planet = true;
        else if (fleet_of(h) != kNoFleet && seen_planet) split = false;
    }
    if (split) {
        for (ObjectHandle h : order) {
            const uint32_t f = fleet_of(h);
            if (f != kNoFleet && store_->alive(h)) service_fleet(f);
        }
        const uint32_t n = static_cast<uint32_t>(planets_.size());
        pf_(n, 8, [this](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i)
                derived_[i].owner = planets_[i].is_planet ? derive_owner(i) : kNoPlayer;
        });
        // A planet's derivation reads only its own structures / surface / starbase / queues / orbit, which no other
        // planet's ownership or production step touches, so the precomputed owners stay valid while interleaved.
        for (ObjectHandle h : order) {
            const PlanetIndex p = planet_of(h);
            if (p == kInvalidPlanet || !store_->alive(h)) continue;
            expire_visibility(p);    // GRC 2.4 (0x285e40 in the planet service)
            complete_corruption(p);  // GRC 4.4 (0x3fc5b0) [port order: before the ownership step]
            apply_ownership(p, derived_[p].owner);
            planet_production(p);
            service_income_abilities(p);  // P4 sim
        }
    } else {
        for (ObjectHandle h : order) {
            if (!store_->alive(h)) continue;
            const uint32_t f = fleet_of(h);
            if (f != kNoFleet) {
                service_fleet(f);
                continue;
            }
            const PlanetIndex p = planet_of(h);
            if (p == kInvalidPlanet) continue;
            expire_visibility(p);    // GRC 2.4
            complete_corruption(p);  // GRC 4.4
            apply_ownership(p, derive_owner(p));
            planet_production(p);
            service_income_abilities(p);  // P4 sim
        }
    }
    // 2. P4 sim: the planets' hero spawn lists (0x3fc750, P4 6.2), planet row order [inferred].
    service_respawns();
    // 3. delayed creations + deferred-delete flush.
    store_->update(static_cast<uint32_t>(frame_), {});
    // S10: conflict processing at the end of the GOM service (BATTLE_TRANSITIONS §1.4).
    if (conflicts_) conflicts_->on_service_tail();
    // 4. fiscal cycle [H].
    if (next_cycle_frame_ <= frame_) fiscal_cycle();
    // 5. T12: the mode frame advances.
    ++frame_;
}

// ---- commands -----------------------------------------------------------------------------------------------

RejectReason GalacticSim::handle_command(uint64_t /*tick*/, const commands::Command& cmd) {
    return execute(cmd.header.player, cmd.payload);
}

RejectReason GalacticSim::execute(uint8_t player_id, const commands::EventData& ev) {
    using commands::EventType;
    const int32_t player = player_id;
    if (!valid_player(player)) return RejectReason::InvalidPlayer;
    auto planet_by_id = [this](uint32_t id) {
        return id == commands::kNoObject ? kInvalidPlanet : planet_of(store_->find(static_cast<objects::ObjectId>(id)));
    };
    auto fleet_by_id = [this](uint32_t id) {
        return id == commands::kNoObject ? kNoFleet : fleet_of(store_->find(static_cast<objects::ObjectId>(id)));
    };
    switch (ev.type()) {
    case EventType::MoveObjectToObject: {
        const uint32_t f = fleet_by_id(ev.u("object"));
        const PlanetIndex p = planet_by_id(ev.u("target"));
        if (f == kNoFleet) {
            // The HUD drags single units off their fleet (galactic_hud.hpp): a member of the issuer's idle fleet in orbit
            // is split off into a fleet of its own that then moves (a lone member moves its fleet).
            if (paused()) return RejectReason::NotAllowed;  // splitting is a fleet-management change (COMMANDS 6.2)
            if (p == kInvalidPlanet) return RejectReason::InvalidTarget;
            const ObjectHandle unit = ev.u("object") == commands::kNoObject ? ObjectHandle{} : store_->find(static_cast<objects::ObjectId>(ev.u("object")));
            const uint32_t from = static_cast<bool>(unit) ? fleet_containing(unit) : kNoFleet;
            if (from == kNoFleet) return RejectReason::InvalidUnit;
            if (fleets_[from].owner != player) return RejectReason::NotOwner;
            if (fleets_[from].moving || fleets_[from].planet == kInvalidPlanet) return RejectReason::NotAllowed;
            const PlanetIndex origin = fleets_[from].planet;
            if (p == origin) return RejectReason::None;  // dropped on its own planet
            if (fleets_[from].contents.size() == 1) return move_fleet(player, from, p);
            std::vector<ObjectHandle>& src = fleets_[from].contents;
            const size_t at = static_cast<size_t>(std::find(src.begin(), src.end(), unit) - src.begin());
            const uint8_t carried = fleets_[from].carried[at];
            src.erase(src.begin() + static_cast<std::ptrdiff_t>(at));
            fleets_[from].carried.erase(fleets_[from].carried.begin() + static_cast<std::ptrdiff_t>(at));
            const uint32_t nf = create_fleet(player, origin);
            fleets_[nf].contents.push_back(unit);
            fleets_[nf].carried.push_back(carried);
            const RejectReason r = move_fleet(player, nf, p);
            if (r != RejectReason::None) {  // put it back where it was
                fleets_[nf].contents.clear();
                fleets_[nf].carried.clear();
                kill_fleet(nf);
                fleets_[from].contents.insert(fleets_[from].contents.begin() + static_cast<std::ptrdiff_t>(at), unit);
                fleets_[from].carried.insert(fleets_[from].carried.begin() + static_cast<std::ptrdiff_t>(at), carried);
            }
            return r;
        }
        if (p == kInvalidPlanet) return RejectReason::InvalidTarget;
        return move_fleet(player, f, p);
    }
    case EventType::MoveThroughObjects: {
        if (paused()) return RejectReason::NotAllowed;  // A2 galactic: COMMANDS §6.2 battle-pending block
        const uint32_t f = fleet_by_id(ev.u("object"));
        if (f == kNoFleet) return RejectReason::InvalidUnit;
        std::vector<PlanetIndex> path;
        for (uint32_t id : ev.ids("waypoints")) {
            const PlanetIndex p = planet_by_id(id);
            if (p == kInvalidPlanet) return RejectReason::InvalidTarget;
            path.push_back(p);
        }
        return move_fleet_path(player, f, path);
    }
    case EventType::Production: {
        const PlanetIndex p = planet_by_id(ev.u("location"));
        if (p == kInvalidPlanet) return RejectReason::InvalidTarget;
        if (ev.u("cancel") != 0) return cancel(player, p, ev.u("queue"), ev.u("index"), true);
        return enqueue(player, p, ev.u("type_index"));
    }
    case EventType::Invade: {
        const PlanetIndex p = planet_by_id(ev.u("planet"));
        if (p == kInvalidPlanet) return RejectReason::InvalidTarget;
        // InvadeEventClass Execute 0x48ff50 (A1_READINGS §31.2). Prologue gates: the galactic pause / battle-pending
        // counter is raised, or a story-linked battle is pending and the local player is in a queued conflict (0x2b0350).
        // The AI's task-force invasions do not come here: GalacticTaskForce.Invade 0x6ce600 calls Land_Invade (invade())
        // directly. Then only the issuer's own units land. A list whose units are all dropped lands nothing (an empty
        // list given to invade() would mean "all of the player's units").
        if (paused()) return RejectReason::NotAllowed;
        const std::vector<uint32_t>& ids = ev.ids("units");
        std::vector<ObjectHandle> units;
        for (uint32_t id : ids) {
            const ObjectHandle h = store_->find(static_cast<objects::ObjectId>(id));
            if (store_->alive(h) && store_->owner(h) == player) units.push_back(h);
        }
        if (!ids.empty() && units.empty()) return RejectReason::NotOwner;
        return invade(player, p, units);
    }
    case EventType::FleetManagement:
        return fleet_management(player, ev);
    case EventType::SpecialAbility:  // P4 sim: kinds 1 / 4 / 5 / 7 (kind 6 and others: Unhandled -> host hooks)
        return special_command(player, ev);
    default:
        return RejectReason::Unhandled;
    }
}

// FleetManagement (FleetOp): GalacticSim::fleet_management in src/ai_hooks.cpp (A2 galactic; was the P host block).

// ---- story / ability hooks ------------------------------------------------------------------------------------

void GalacticSim::set_corruption(PlanetIndex planet, int8_t type, int32_t corrupting_player) {
    if (!valid_planet(planet)) return;
    const int8_t old = planets_[planet].corruption;
    const int32_t old_player = planets_[planet].corrupting_player;
    cancel_corruption_transition(planet);  // GRC 4.3 planet reset / instant paths
    planets_[planet].corruption = type;
    planets_[planet].corrupting_player = type == kCorruptionNone ? kNoPlayer : corrupting_player;
    rebuild_corruption_pairs(planet);
    if (old != type) corruption_unlocks(planet, old, old_player);
    if (old != type) {
        GalacticEvent ev;
        ev.kind = GalacticEventKind::CorruptionChanged;
        ev.planet = planet;
        ev.player = planets_[planet].corrupting_player;
        ev.value = type;
        ev.other_player = old;
        emit(ev);
    }
}
void GalacticSim::set_black_market_influence(PlanetIndex planet, bool on) {
    if (valid_planet(planet)) planets_[planet].black_market_influences = on;
}
void GalacticSim::set_destroyed(PlanetIndex planet, bool destroyed) {
    if (valid_planet(planet)) planets_[planet].destroyed = destroyed;
}
void GalacticSim::set_credit_halt(PlanetIndex planet, int32_t until_frame) {
    if (valid_planet(planet)) planets_[planet].halt_until = std::max(planets_[planet].halt_until, until_frame);
}
void GalacticSim::set_siphon(PlanetIndex planet, int32_t player, float fraction, int32_t until_frame) {
    if (!valid_planet(planet)) return;
    planets_[planet].siphon_player = player;
    planets_[planet].siphon_fraction = fraction;
    planets_[planet].siphon_until = until_frame;
    planets_[planet].siphon_amount = 0.0f;
}
void GalacticSim::set_income_limits(PlanetIndex planet, bool has_min, float min_value, bool has_max, float max_value) {
    if (!valid_planet(planet)) return;
    PlanetState& p = planets_[planet];
    p.min_income_set = has_min;
    p.min_income = min_value;
    p.max_income_set = has_max;
    p.max_income = max_value;
}
void GalacticSim::add_income_entry(PlanetIndex planet, float value, int32_t expiry) {
    if (!valid_planet(planet)) return;
    IncomeEntry e;
    e.kind = kIncomeAbility;
    e.value = value;
    e.expiry = expiry;
    planets_[planet].income.push_back(e);
}
void GalacticSim::set_hidden(PlanetIndex planet, bool hidden) {
    if (valid_planet(planet)) planets_[planet].hidden = hidden;
}
void GalacticSim::set_tech_level(int32_t player, int32_t level, bool sweep) {
    if (!valid_player(player)) return;
    PlayerState& ps = players_[static_cast<size_t>(player)];
    const int32_t old = ps.tech_level;
    ps.tech_level = std::min(level < 1 ? 0 : level, ps.max_tech_level);
    if (old != ps.tech_level) {
        GalacticEvent ev;
        ev.kind = GalacticEventKind::TechLevelChanged;
        ev.player = player;
        ev.value = ps.tech_level;
        ev.other_player = old;
        emit(ev);
    }
    if (!sweep) return;
    for (uint32_t type = 0; type < rules_->types.size(); ++type) {  // type-manager order
        const TypeInfo& t = rules_->types[type];
        if (t.build_initially_locked && t.sliceable && t.tech_level < ps.tech_level && !type_unlocked(player, type) &&
            type_affiliated(type, player))
            unlock_type(player, type, 1);
    }
}

bool GalacticSim::raise_tech_level(int32_t player) {
    if (!valid_player(player)) return false;
    const PlayerState& ps = players_[static_cast<size_t>(player)];
    if (ps.tech_level >= ps.max_tech_level) return false;
    set_tech_level(player, ps.tech_level + 1, true);
    return true;
}

// ---- S10 battle write-back hooks --------------------------------------------------------------------------------

void GalacticSim::set_in_battle(PlanetIndex planet, bool on) {
    if (valid_planet(planet)) planets_[planet].in_battle = on;
}

void GalacticSim::keep_built_objects(PlanetIndex planet, int32_t keep) {
    // AI_GALACTIC_TOKENS §9.4 owner filter 0x4b7d90: last to first, every record owned by another player is erased.
    if (planet >= planets_.size()) return;
    std::vector<PersistedBuiltObject>& v = planets_[planet].built_objects;
    for (size_t i = v.size(); i-- > 0;)
        if (v[i].owner != keep) v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
}

bool GalacticSim::replace_starbase(PlanetIndex planet, uint32_t type) {
    if (!valid_planet(planet)) return false;
    PlanetState& p = planets_[planet];
    if (!store_->alive(p.starbase)) return false;
    if (type == kNoIndex || type >= rules_->types.size()) return destroy_object(p.starbase);
    const int32_t owner = p.starbase_owner;
    store_->destroy(p.starbase);
    p.starbase = spawn(owner, planet, type, p.object);
    p.starbase_level = rules_->types[type].base_level;
    GalacticEvent ev;
    ev.kind = GalacticEventKind::StarbaseLevelChanged;
    ev.player = owner;
    ev.planet = planet;
    ev.type = type;
    ev.value = p.starbase_level;
    emit(ev);
    return true;
}

// refix t05 (A2GAL_READINGS 2.2, starbase_type()): the planet's starbase type is its starbase object's type.
uint32_t GalacticSim::starbase_type(PlanetIndex planet) const {
    if (!valid_planet(planet) || !store_->alive(planets_[planet].starbase)) return kNoIndex;
    const auto t = static_cast<uint32_t>(store_->type(planets_[planet].starbase));
    return t < rules_->types.size() ? t : kNoIndex;
}

// refix t05 [port]: a starbase object that left the store without destroy_object (no sim bookkeeping) is a destroyed
// starbase: level 0, owner none, StarbaseLevelChanged as destroy_object emits it. Planet row order; nothing happens
// while every planet's starbase state is consistent (the normal case), so no hashed state changes then.
void GalacticSim::sync_starbases() {
    for (PlanetIndex pi = 0; pi < planets_.size(); ++pi) {
        PlanetState& p = planets_[pi];
        if (store_->alive(p.starbase) || (!p.starbase && p.starbase_level == 0 && p.starbase_owner == kNoPlayer)) continue;
        const bool had_level = p.starbase_level != 0;
        p.starbase = {};
        p.starbase_level = 0;
        if (had_level) {
            GalacticEvent ev;
            ev.kind = GalacticEventKind::StarbaseLevelChanged;
            ev.player = p.starbase_owner;
            ev.planet = pi;
            ev.value = 0;
            emit(ev);
        }
        p.starbase_owner = kNoPlayer;
    }
}

void GalacticSim::clear_queue(PlanetIndex planet, uint32_t queue) {
    if (!valid_planet(planet) || queue > 1) return;
    auto& qv = planets_[planet].queue[queue];
    while (!qv.empty()) {
        emit_production(GalacticEventKind::ProductionCanceled, planet, queue, qv.back(), 0);
        qv.pop_back();
    }
}

// ---- S4 event stream ------------------------------------------------------------------------------------------

void GalacticSim::drain_events(std::vector<GalacticEvent>& out) {
    out.insert(out.end(), events_.begin(), events_.end());
    events_.clear();
}

std::string_view galactic_event_name(GalacticEventKind kind) {
    switch (kind) {
    case GalacticEventKind::PlanetOwnerChanged: return "PlanetOwnerChanged";
    case GalacticEventKind::FleetDeparted: return "FleetDeparted";
    case GalacticEventKind::FleetArrived: return "FleetArrived";
    case GalacticEventKind::FleetBounced: return "FleetBounced";
    case GalacticEventKind::ProductionBegin: return "ProductionBegin";
    case GalacticEventKind::ProductionCanceled: return "ProductionCanceled";
    case GalacticEventKind::ProductionCompleted: return "ProductionCompleted";
    case GalacticEventKind::StarbaseLevelChanged: return "StarbaseLevelChanged";
    case GalacticEventKind::ObjectDestroyed: return "ObjectDestroyed";
    case GalacticEventKind::UnitsLanded: return "UnitsLanded";
    case GalacticEventKind::CorruptionChanged: return "CorruptionChanged";
    case GalacticEventKind::TechLevelChanged: return "TechLevelChanged";
    case GalacticEventKind::GalacticDayPassed: return "GalacticDayPassed";
    case GalacticEventKind::BattleWon: return "BattleWon";
    case GalacticEventKind::BattleLost: return "BattleLost";
    case GalacticEventKind::BattleRetreat: return "BattleRetreat";
    // P4 sim
    case GalacticEventKind::SpecialDialog: return "SpecialDialog";
    case GalacticEventKind::SpecialError: return "SpecialError";
    case GalacticEventKind::SpecialApplied: return "SpecialApplied";
    case GalacticEventKind::BlackMarketPurchase: return "BlackMarketPurchase";
    case GalacticEventKind::GalacticSabotage: return "GalacticSabotage";
    case GalacticEventKind::SabotageVigilance: return "SabotageVigilance";
    case GalacticEventKind::PlanetDestroyed: return "PlanetDestroyed";
    case GalacticEventKind::SuperWeaponHacked: return "SuperWeaponHacked";
    case GalacticEventKind::HeroRespawnScheduled: return "HeroRespawnScheduled";
    case GalacticEventKind::HeroRespawned: return "HeroRespawned";
    case GalacticEventKind::TypeUnlocked: return "TypeUnlocked";
    case GalacticEventKind::Slice: return "Slice";
    case GalacticEventKind::Count: break;
    }
    return "?";
}

// ---- CRC ----------------------------------------------------------------------------------------------------

void GalacticSim::hash_state(StateHasher& h) const {
    auto id_of = [this](ObjectHandle o) { return store_->exists(o) ? store_->id(o) : objects::kNoObjectId; };
    h.i32(frame_);
    h.i32(pause_);
    h.i32(last_cycle_frame_);
    h.i32(next_cycle_frame_);
    h.i32(cycle_counter_);
    h.u32(static_cast<uint32_t>(players_.size()));
    for (const PlayerState& p : players_) {
        h.u32(p.faction);
        h.boolean(p.human);
        h.boolean(p.ai);
        h.u32(p.difficulty);
        h.f32(p.account.credits);
        h.f32(p.account.cap);
        h.i32(p.tech_level);
        h.f32(p.tech_cost_mult);
        h.boolean(p.cheat);
    }
    h.u32(static_cast<uint32_t>(planets_.size()));
    for (const PlanetState& p : planets_) {
        h.boolean(p.is_planet);
        if (!p.is_planet) continue;
        h.i32(id_of(p.object));
        h.boolean(p.initialised);
        h.i32(p.owner);
        h.f32(p.control);
        h.f32(p.target_control);
        h.i32(p.leaning);
        h.i32(p.base_value);
        h.f32(p.black_market);
        h.boolean(p.black_market_influences);
        h.boolean(p.destroyed);
        h.value(p.corruption);
        h.i32(p.corrupting_player);
        h.boolean(p.corruption_protected);
        h.f32(p.piracy_pool);
        h.i32(p.siphon_player);
        h.f32(p.siphon_fraction);
        h.i32(p.siphon_until);
        h.f32(p.siphon_amount);
        h.i32(p.halt_until);
        h.boolean(p.min_income_set);
        h.boolean(p.max_income_set);
        h.f32(p.min_income);
        h.f32(p.max_income);
        h.f32(p.build_time_bonus);
        h.f32(p.build_cost_bonus);
        h.boolean(p.hidden);
        h.boolean(p.in_battle);
        h.u32(static_cast<uint32_t>(p.income.size()));
        for (const IncomeEntry& e : p.income) {
            h.u8(e.kind);
            h.f32(e.value);
            h.i32(e.expiry);
            if (e.ability != 0) {  // P4 sim: ability entries only (S3 / story entries hash as before)
                h.u32(e.ability);
                h.i32(e.source);
                h.u32(e.tag);
                h.i32(e.next_eval);
            }
        }
        for (const auto& q : p.queue) {
            h.u32(static_cast<uint32_t>(q.size()));
            for (const BuildEntry& e : q) {
                h.u32(e.type);
                h.f32(e.build_time);
                h.i32(e.start);
                h.i32(e.duration);
                h.f32(e.remaining);
                h.i32(e.player);
                h.f32(e.cost);
            }
        }
        h.i32(p.starbase_level);
        h.i32(p.starbase_owner);
        h.i32(id_of(p.starbase));
        h.u32(static_cast<uint32_t>(p.structures.size()));
        for (ObjectHandle s : p.structures) h.i32(id_of(s));
        h.u32(static_cast<uint32_t>(p.surface.size()));
        for (ObjectHandle s : p.surface) h.i32(id_of(s));
        h.u32(static_cast<uint32_t>(p.orbit.size()));
        for (uint32_t f : p.orbit) h.i32(id_of(fleets_[f].object));
    }
    uint32_t live = 0;
    for (const FleetState& f : fleets_) live += f.alive ? 1u : 0u;
    h.u32(live);
    for (const FleetState& f : fleets_) {
        if (!f.alive) continue;
        h.i32(id_of(f.object));
        h.i32(f.owner);
        h.u32(static_cast<uint32_t>(f.contents.size()));
        for (size_t i = 0; i < f.contents.size(); ++i) {
            h.i32(id_of(f.contents[i]));
            h.u8(f.carried[i]);
        }
        h.u32(f.planet);
        h.u32(f.from);
        h.u32(f.to);
        h.u32(static_cast<uint32_t>(f.path.size()));
        for (PlanetIndex p : f.path) h.u32(p);
        h.u32(f.path_index);
        h.f32(f.base_speed);
        h.f32(f.hop_speed);
        h.f32(f.position.x);
        h.f32(f.position.y);
        h.f32(f.position.z);
        h.f32(f.facing.x);
        h.f32(f.facing.y);
        h.f32(f.facing.z);
        h.boolean(f.moving);
    }
    if (!special_state_default()) hash_special(h);  // P4 sim (order: special_systems.hpp "CRC")
    if (!diplomacy_state_default()) hash_diplomacy(h);  // relation table + unlocked lists, once touched
    if (!slot_state_default()) hash_slots(h);  // Playtest 3 t07: orbit slots once a command chose one
    if (!oi_state_default()) hash_oi(h);  // OI galactic: stealth bytes, victory claims, max tech, build counts
    if (!reveal_state_default()) hash_reveal(h);  // GRC: local player, auto-reveal, revealed bytes, transitions
    if (!visibility_state_default()) hash_visibility(h);  // GRC 2: stored visibility modifier entries
}

void GalacticSim::register_crc(CrcRegistry& registry, const std::string& prefix, bool include_store) const {
    registry.add(prefix + ".state", [this](StateHasher& h) { hash_state(h); });
    if (include_store) store_->register_crc(registry, prefix + ".objects");
}

}  // namespace alamo::galactic

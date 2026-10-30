// Galactic AI perception: interim EnemyForce model, the four consumer paths, Node_Weight inputs,
// PlayerView fill, CRC (see the header).
#include "alamo/ai/galactic_perception.hpp"

#include "alamo/ai/ai_path_finder.hpp"

#include <utility>

namespace alamo::ai {

namespace {

constexpr double kNoAiNodeWeight = 4294967295.0;  // Node_Weight 0xFFFFFFFF (no AI system, Q7)

struct Ids {
    TokenId enemy_force, space_total, ground_total, has_space, has_ground, target_player;
};
const Ids& ids() {
    static const Ids k{registry_token("EnemyForce"),    registry_token("SpaceTotalUnnormalized"),
                       registry_token("GroundTotalUnnormalized"), registry_token("HasSpaceForce"),
                       registry_token("HasGroundForce"), registry_token("TargetPlayerID")};
    return k;
}

enum class Tok : uint8_t { None, SpaceTotal, GroundTotal, HasSpace, HasGround, TargetPlayer };

Tok classify(Root root, std::span<const TokenId> path) {
    if (root != Root::Target) return Tok::None;
    const Ids& k = ids();
    if (path.size() == 1 && path[0] == k.target_player) return Tok::TargetPlayer;
    if (path.size() == 2 && path[0] == k.enemy_force) {
        if (path[1] == k.space_total) return Tok::SpaceTotal;
        if (path[1] == k.ground_total) return Tok::GroundTotal;
        if (path[1] == k.has_space) return Tok::HasSpace;
        if (path[1] == k.has_ground) return Tok::HasGround;
    }
    return Tok::None;
}

template <class T>
T at_or(const std::vector<T>& v, size_t i, T fallback) {
    return i < v.size() ? v[i] : fallback;
}

}  // namespace

// ---- GalacticSnapshot ----

bool GalacticSnapshot::friendly(PlayerId a, PlayerId b) const {
    if (a == b) return true;
    if (!is_player(a) || !is_player(b)) return false;
    const size_t P = players.size();
    if (relation.size() != P * P) return false;
    return relation[static_cast<size_t>(a) * P + static_cast<size_t>(b)] == 0;
}

bool GalacticSnapshot::held_by_ally(uint32_t planet, PlayerId p) const {
    const PlayerId o = at_or(owner, planet, kNoPlayer);
    if (o == kNoPlayer) return false;
    const float control = political_control.empty() ? 1.0f : at_or(political_control, planet, 0.0f);
    return friendly(p, o) && control > 0.0f;
}

bool GalacticSnapshot::unowned_or_neutral(uint32_t planet) const {
    const PlayerId o = at_or(owner, planet, kNoPlayer);
    return !is_player(o) || players[static_cast<size_t>(o)].neutral;
}

bool GalacticSnapshot::sees(PlayerId p, uint32_t planet) const {
    if (fog_disabled) return true;
    if (!is_player(p) || static_cast<size_t>(p) >= sees_orbit.size()) return true;
    const galactic::Bitset& b = sees_orbit[static_cast<size_t>(p)];
    return b.empty() || (planet < b.size() && b.test(planet));
}

bool GalacticSnapshot::sees(PlayerId p, uint32_t planet, uint32_t m) const {
    if (fog_disabled) return true;
    if (is_player(p) && static_cast<size_t>(p) < visibility.size() && !visibility[static_cast<size_t>(p)].empty()) {
        const std::vector<uint32_t>& v = visibility[static_cast<size_t>(p)];
        const uint32_t level = planet < v.size() ? v[planet] : 0u;
        return (level & m) == m;
    }
    return sees(p, planet);
}

std::span<const UnitPresence> GalacticSnapshot::units_at(uint32_t planet) const {
    if (unit_begin.size() != static_cast<size_t>(planet_count) + 1 || planet >= planet_count) return {};
    return std::span<const UnitPresence>(units).subspan(unit_begin[planet], unit_begin[planet + 1] - unit_begin[planet]);
}

std::span<const float> GalacticSnapshot::structures_at(uint32_t planet) const {
    if (structure_begin.size() != static_cast<size_t>(planet_count) + 1 || planet >= planet_count) return {};
    return std::span<const float>(structure_power)
        .subspan(structure_begin[planet], structure_begin[planet + 1] - structure_begin[planet]);
}

std::vector<std::string> GalacticSnapshot::validate() const {
    std::vector<std::string> e;
    const size_t n = planet_count, P = players.size();
    auto size_ok = [&](size_t got, const char* what, bool optional) {
        if (got != n && !(optional && got == 0)) e.push_back(std::string(what) + ": size " + std::to_string(got));
    };
    size_ok(owner.size(), "owner", false);
    size_ok(political_control.size(), "political_control", true);
    size_ok(starbase_level.size(), "starbase_level", true);
    size_ok(starbase_power.size(), "starbase_power", true);
    size_ok(corruption.size(), "corruption", true);
    if (!relation.empty() && relation.size() != P * P) e.push_back("relation: not players^2");
    auto csr_ok = [&](const std::vector<uint32_t>& b, size_t items, const char* what) {
        if (b.empty()) {
            if (items) e.push_back(std::string(what) + ": items without offsets");
            return;
        }
        if (b.size() != n + 1 || b.front() != 0 || b.back() != items) {
            e.push_back(std::string(what) + ": bad offsets");
            return;
        }
        for (size_t i = 0; i < n; ++i)
            if (b[i] > b[i + 1]) e.push_back(std::string(what) + ": offsets decrease");
    };
    csr_ok(unit_begin, units.size(), "units");
    csr_ok(structure_begin, structure_power.size(), "structures");
    for (size_t i = 0; i < owner.size(); ++i)
        if (owner[i] != kNoPlayer && !is_player(owner[i])) e.push_back("owner of planet " + std::to_string(i) + " is not a player");
    for (const UnitPresence& u : units)
        if (u.owner != kNoPlayer && !is_player(u.owner)) {
            e.push_back("unit owner is not a player");
            break;
        }
    if (sees_orbit.size() > P) e.push_back("sees_orbit: more entries than players");
    if (visibility.size() > P) e.push_back("visibility: more entries than players");
    for (const std::vector<uint32_t>& v : visibility)
        if (!v.empty() && v.size() != n) {
            e.push_back("visibility: row size is not planet_count");
            break;
        }
    return e;
}

void PerceptionMemory::reset(uint32_t planets) {
    seen.assign(planets, 0);
    last_seen.assign(planets, 0);
    owner.assign(planets, kNoPlayer);
    space_total.assign(planets, 0.0);
    ground_total.assign(planets, 0.0);
}

// ---- GalacticTokenResolver ----

bool GalacticTokenResolver::implements(Root root, std::span<const TokenId> path, const TokenTable&) const {
    return classify(root, path) != Tok::None;
}

bool GalacticTokenResolver::resolve(const EvalContext& ctx, const TokenRef& token, double& out) const {
    const Tok t = classify(token.root, token.path);
    if (t == Tok::None || !token.params.empty()) return false;
    if (ctx.self < 0 || static_cast<size_t>(ctx.self) >= mem_.size()) return false;
    if (ctx.target < 0 || static_cast<uint32_t>(ctx.target) >= s_.planet_count) return false;
    const PerceptionMemory& m = mem_[static_cast<size_t>(ctx.self)];
    const auto p = static_cast<size_t>(ctx.target);
    if (p >= m.seen.size()) return false;
    switch (t) {
        case Tok::SpaceTotal: out = m.space_total[p]; return true;
        case Tok::GroundTotal: out = m.ground_total[p]; return true;
        case Tok::HasSpace: out = m.space_total[p] > 0.0 ? 1.0 : 0.0; return true;
        case Tok::HasGround: out = m.ground_total[p] > 0.0 ? 1.0 : 0.0; return true;
        case Tok::TargetPlayer:
            if (!m.seen[p]) return false;  // never seen: ownership unknown to Self
            out = static_cast<double>(m.owner[p]);
            return true;
        case Tok::None: break;
    }
    return false;
}

// ---- GalacticPerception ----

GalacticPerception::GalacticPerception() {
    static constexpr std::string_view kPaths[4] = {
        "Variable_Target.EnemyForce.SpaceTotalUnnormalized",
        "Variable_Target.EnemyForce.GroundTotalUnnormalized",
        "Variable_Target.EnemyForce.HasSpaceForce",
        "Variable_Target.TargetPlayerID",
    };
    static constexpr std::string_view kNames[4] = {"Node_Weight_Space", "Node_Weight_Ground", "Planet_Danger",
                                                   "Planet_Perceived_Owner"};
    for (uint32_t i = 0; i < 4; ++i) path_index_[i] = paths_.add(kNames[i], kPaths[i]);
    paths_.link();
}

void GalacticPerception::reset(const GalacticSnapshot& s) {
    planets_ = s.planet_count;
    frame_ = 0;
    const size_t P = s.players.size();
    tracked_.assign(P, 0);
    memory_.assign(P, PerceptionMemory{});
    results_.assign(P, PlanetPerception{});
    for (size_t p = 0; p < P; ++p) {
        tracked_[p] = s.players[p].has_ai_system ? 1 : 0;
        if (tracked_[p]) memory_[p].reset(planets_);
    }
    revealed_ = galactic::Bitset(planets_, true);
}

void GalacticPerception::observe(const GalacticSnapshot& s, uint32_t frame) {
    if (s.planet_count != planets_ || s.players.size() != tracked_.size()) reset(s);
    frame_ = frame;
    for (size_t pi = 0; pi < tracked_.size(); ++pi) {
        if (!tracked_[pi]) continue;
        const auto self = static_cast<PlayerId>(pi);
        PerceptionMemory& m = memory_[pi];
        for (uint32_t i = 0; i < planets_; ++i) {
            // Each remembered value is its own fogged leaf with its own mask (AI_GALACTIC_TOKENS §0); without a
            // visibility row the three tests are the same sees_orbit bit (the former single-flag model).
            const bool see_owner = s.sees(self, i, kVisMaskOwner);
            const bool see_space = s.sees(self, i, kVisMaskSpaceUnits);
            const bool see_ground = s.sees(self, i, kVisMaskGroundUnits);
            if (!see_owner && !see_space && !see_ground) continue;
            double space = 0.0, ground = 0.0;
            for (const UnitPresence& u : s.units_at(i)) {
                if (u.owner != kNoPlayer && s.friendly(self, u.owner)) continue;
                if (u.layer == UnitLayer::Space) space += static_cast<double>(u.combat_power);
                else ground += static_cast<double>(u.combat_power);
            }
            m.last_seen[i] = frame;  // [port] last sighting of any of the three
            if (see_owner) {
                m.seen[i] = 1;  // TargetPlayerID fails until the owner leaf was visible once
                m.owner[i] = at_or(s.owner, i, kNoPlayer);
            }
            if (see_space) m.space_total[i] = space;
            if (see_ground) m.ground_total[i] = ground;
        }
    }
}

void GalacticPerception::evaluate(const GalacticSnapshot& s, const ParallelFor& pf) {
    if (s.planet_count != planets_ || s.players.size() != tracked_.size()) reset(s);
    // Is_Planet_Revealed (0x3fa420) from the LOCAL masks.
    for (uint32_t i = 0; i < planets_; ++i) {
        bool r = true;
        if (s.multiplayer) r = true;
        else if (!s.hidden.empty() && i < s.hidden.size() && s.hidden.test(i)) r = false;
        else if (s.planet_auto_reveal) r = true;
        else r = s.revealed.empty() || (i < s.revealed.size() && s.revealed.test(i));
        revealed_.set(i, r);
    }
    human_.assign(tracked_.size(), 0);
    for (size_t p = 0; p < human_.size() && p < s.players.size(); ++p) human_[p] = s.players[p].is_human ? 1 : 0;
    for (size_t p = 0; p < tracked_.size(); ++p) evaluate_player(s, static_cast<PlayerId>(p), pf);
}

void GalacticPerception::update(const GalacticSnapshot& s, uint32_t frame, const ParallelFor& pf) {
    observe(s, frame);
    evaluate(s, pf);
}

void GalacticPerception::evaluate_player(const GalacticSnapshot& s, PlayerId player, const ParallelFor& pf) {
    PlanetPerception& r = results_[static_cast<size_t>(player)];
    const uint32_t n = planets_;
    r.space_total.assign(n, 0.0);
    r.ground_total.assign(n, 0.0);
    r.space_ok.assign(n, 0);
    r.ground_ok.assign(n, 0);
    r.has_space_force.assign(n, 0.0);
    r.has_space_force_ok.assign(n, 0);
    r.target_player.assign(n, 0.0);
    r.target_player_ok.assign(n, 0);
    if (!tracked_[static_cast<size_t>(player)]) {
        r.node_value.assign(n, kNoAiNodeWeight);
        r.ground_value.assign(n, kNoAiNodeWeight);
        return;
    }
    r.node_value.assign(n, 0.0);
    r.ground_value.assign(n, 0.0);
    const GalacticTokenResolver resolver(s, memory_);
    auto body = [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            EvalContext ctx;
            ctx.self = player;
            ctx.target = static_cast<int32_t>(i);
            const EvalResult sp = ai::evaluate(paths_, path_index_[kSpaceTotal], ctx, resolver);
            const EvalResult gr = ai::evaluate(paths_, path_index_[kGroundTotal], ctx, resolver);
            const EvalResult hs = ai::evaluate(paths_, path_index_[kHasSpaceForce], ctx, resolver);
            const EvalResult tp = ai::evaluate(paths_, path_index_[kTargetPlayerId], ctx, resolver);
            r.space_total[i] = sp.value;
            r.space_ok[i] = sp.ok ? 1 : 0;
            r.ground_total[i] = gr.value;
            r.ground_ok[i] = gr.ok ? 1 : 0;
            r.has_space_force[i] = hs.value;
            r.has_space_force_ok[i] = hs.ok ? 1 : 0;
            r.target_player[i] = tp.value;
            r.target_player_ok[i] = tp.ok ? 1 : 0;
            // 0x5f2430: perception (double) + opposing strength when not held by Self or an ally.
            double v = sp.ok ? sp.value : 0.0;
            double g = gr.ok ? gr.value : 0.0;
            if (!s.held_by_ally(i, player)) {
                v += static_cast<double>(at_or(s.starbase_power, i, 0.0f));
                for (float f : s.structures_at(i)) g += static_cast<double>(f);
            }
            r.node_value[i] = v;
            r.ground_value[i] = g;
        }
    };
    if (n >= kPerceptionParallelMin) pf(n, 1024, body);
    else body(0, n);
}

void GalacticPerception::fill_view(const GalacticSnapshot& s, PlayerId p, galactic::PlayerView& view) const {
    const uint32_t n = planets_;
    const PerceptionPlayer me = s.is_player(p) ? s.players[static_cast<size_t>(p)] : PerceptionPlayer{};
    view.is_human = me.is_human;
    view.has_ai_system = me.has_ai_system;
    view.benefits_from_corruption = me.benefits_from_corruption;
    view.multiplayer = s.multiplayer;
    view.fog_disabled = s.planet_auto_reveal;  // PlayerView::fog_disabled = mode +0x340 auto-reveal
    view.revealed = s.revealed;
    view.hidden = s.hidden;
    view.in_battle = s.in_battle;
    view.ownership_visible = s.ownership_visible;
    view.corrupted = galactic::Bitset(n);
    view.unowned_or_neutral = galactic::Bitset(n);
    view.held_by_ally = galactic::Bitset(n);
    view.ai_eval_failed = galactic::Bitset(n);
    view.ai_perceived_non_neutral = galactic::Bitset(n);
    view.dangerous = galactic::Bitset(n);
    const bool ai = tracks(p);
    for (uint32_t i = 0; i < n; ++i) {
        const bool held = s.held_by_ally(i, p);
        view.corrupted.set(i, at_or(s.corruption, i, -1) != -1);
        view.unowned_or_neutral.set(i, s.unowned_or_neutral(i));
        view.held_by_ally.set(i, held);
        bool dangerous = false;
        if (at_or(s.starbase_level, i, 0) != 0) {
            dangerous = !held;
        } else if (ai) {
            const PlanetPerception& r = results(p);
            dangerous = !r.has_space_force_ok[i] || r.has_space_force[i] != 0.0;
        } else if (!s.local_orbit_visible.empty() && i < s.local_orbit_visible.size() && s.local_orbit_visible.test(i)) {
            for (const UnitPresence& u : s.units_at(i)) {
                if (u.layer != UnitLayer::Space) continue;
                if (u.owner == kNoPlayer || !s.friendly(p, u.owner)) {
                    dangerous = true;
                    break;
                }
            }
        }
        view.dangerous.set(i, dangerous);
        if (ai) {
            const PlanetPerception& r = results(p);
            view.ai_eval_failed.set(i, r.target_player_ok[i] == 0);
            if (r.target_player_ok[i]) {
                const auto who = static_cast<PlayerId>(r.target_player[i]);
                view.ai_perceived_non_neutral.set(i, s.is_player(who) && !s.players[static_cast<size_t>(who)].neutral);
            }
        }
    }
}

galactic::AllPairsInput GalacticPerception::all_pairs_input(PlayerId p) const {
    galactic::AllPairsInput in;
    if (p < 0 || static_cast<size_t>(p) >= results_.size()) return in;
    // Q8 step 2: the fog gate only for a player with an AI system that is NOT human (ai_path_fog_gate).
    const bool human = static_cast<size_t>(p) < human_.size() && human_[static_cast<size_t>(p)] != 0;
    return ai_path_input(results_[static_cast<size_t>(p)].node_value, &revealed_, tracks(p), human);
}

void GalacticPerception::hash(StateHasher& h) const {
    h.u32(planets_);
    h.u32(static_cast<uint32_t>(tracked_.size()));
    for (size_t p = 0; p < tracked_.size(); ++p) {
        h.boolean(tracked_[p] != 0);
        if (!tracked_[p]) continue;
        const PerceptionMemory& m = memory_[p];
        h.u32(static_cast<uint32_t>(m.seen.size()));
        h.span(m.seen);
        h.span(m.last_seen);
        h.span(m.owner);
        h.span(m.space_total);
        h.span(m.ground_total);
        const PlanetPerception& r = results_[p];
        h.u32(static_cast<uint32_t>(r.node_value.size()));
        h.span(r.node_value);
        h.span(r.ground_value);
        h.span(r.has_space_force);
        h.span(r.has_space_force_ok);
        h.span(r.target_player);
        h.span(r.target_player_ok);
    }
}

CrcRegistry::SystemId register_crc(CrcRegistry& reg, const GalacticPerception& p, std::string name) {
    return reg.add(std::move(name), [&p](StateHasher& h) { p.hash(h); });
}

}  // namespace alamo::ai

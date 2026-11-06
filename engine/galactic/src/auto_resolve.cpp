#include "alamo/galactic/auto_resolve.hpp"

#include <algorithm>

#include "alamo/objects/type_fields.hpp"

namespace alamo::galactic {

namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// Items of a Lua table constructor "{a, b, c}" on one line: quoted strings or numbers, as written.
std::vector<std::string> table_items(std::string_view line) {
    std::vector<std::string> out;
    const size_t open = line.find('{'), close = line.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return out;
    std::string_view body = line.substr(open + 1, close - open - 1);
    size_t i = 0;
    while (i < body.size()) {
        const size_t comma = body.find(',', i);
        const size_t end = comma == std::string_view::npos ? body.size() : comma;
        std::string_view item = trim(body.substr(i, end - i));
        if (item.size() >= 2 && (item.front() == '"' || item.front() == '\'')) item = item.substr(1, item.size() - 2);
        if (!item.empty()) out.emplace_back(item);
        i = end + 1;
    }
    return out;
}

// Value of `X = "name"` on one line.
std::string quoted_value(std::string_view line) {
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) return {};
    const size_t q0 = line.find('"', eq);
    if (q0 == std::string_view::npos) return {};
    const size_t q1 = line.find('"', q0 + 1);
    if (q1 == std::string_view::npos) return {};
    return std::string(line.substr(q0 + 1, q1 - q0 - 1));
}

int32_t round_half_away(double x) { return static_cast<int32_t>(x < 0.0 ? x - 0.5 : x + 0.5); }

}  // namespace

// ---- contrast table ---------------------------------------------------------------------------------------------

float ContrastTable::contrast(size_t row, std::string_view type_name, uint64_t categories) const {
    if (row >= rows.size()) return 0.0f;
    for (const ContrastRow::Entry& e : rows[row].friendly) {
        if ((e.mask & categories) != 0) return e.weight;
        if (!type_name.empty() && iequal(e.name, type_name)) return e.weight;
    }
    return 0.0f;
}

float ContrastTable::ai_weight(size_t row, std::string_view type_name, uint64_t categories) const {
    if (row >= rows.size()) return 0.0f;
    float sum = 0.0f;
    int32_t count = 0;
    bool matched = false;
    for (const ContrastRow::Entry& e : rows[row].friendly) {
        if (e.mask == 0) {
            if (!type_name.empty() && iequal(e.name, type_name)) return e.weight;
            continue;
        }
        if ((e.mask & categories) == 0) continue;
        matched = true;
        if (e.weight != 1.0f) {
            sum += e.weight;
            ++count;
        }
    }
    if (count > 0) return sum / static_cast<float>(count);
    return matched ? 1.0f : 0.0f;
}

bool ContrastTable::parse_lua(std::string_view src, const std::function<uint64_t(std::string_view)>& category_mask,
                              std::vector<std::string>* diag) {
    rows.clear();
    const size_t fn = src.find("function Set_Contrast_Values");
    if (fn == std::string_view::npos) {
        if (diag) diag->push_back("contrast: Set_Contrast_Values not found");
        return false;
    }
    std::vector<std::string> names;
    std::vector<std::string> weights;
    size_t i = src.find('\n', fn);
    while (i != std::string_view::npos && i < src.size()) {
        const size_t e = std::min(src.find('\n', i + 1), src.size());
        std::string_view line = src.substr(i + 1, e - i - 1);
        const size_t comment = line.find("--");
        if (comment != std::string_view::npos) line = line.substr(0, comment);
        const std::string_view t = trim(line);
        i = e;
        if (t == "end") break;  // end of the function (column-0 "end" in the shipped script)
        if (t.rfind("EnemyContrastTypes[", 0) == 0) {
            ContrastRow r;
            r.enemy = quoted_value(t);
            r.enemy_mask = category_mask ? category_mask(r.enemy) : 0;
            if (r.enemy_mask == 0 && diag) diag->push_back("contrast: unknown enemy category " + r.enemy);
            rows.push_back(std::move(r));
            names.clear();
            weights.clear();
        } else if (t.rfind("FriendlyContrastTypeNames", 0) == 0) {
            names = table_items(t);
        } else if (t.rfind("FriendlyContrastWeights", 0) == 0) {
            weights = table_items(t);
        } else if (t.find(".Parse(") != std::string_view::npos && !rows.empty()) {
            ContrastRow& r = rows.back();
            r.friendly.clear();
            const size_t n = std::min(names.size(), weights.size());
            if (names.size() != weights.size() && diag)
                diag->push_back("contrast: row " + r.enemy + " names/weights size mismatch");
            for (size_t k = 0; k < n; ++k) {
                ContrastRow::Entry en;
                en.name = names[k];
                en.mask = category_mask ? category_mask(en.name) : 0;
                en.weight = static_cast<float>(objects::c_atof(weights[k]));
                r.friendly.push_back(std::move(en));
            }
        }
    }
    return !rows.empty();
}

void ContrastTable::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(rows.size()));
    for (const ContrastRow& r : rows) {
        h.u64(r.enemy_mask);
        h.u32(static_cast<uint32_t>(r.friendly.size()));
        for (const ContrastRow::Entry& e : r.friendly) {
            h.u64(e.mask);
            h.f32(e.weight);
        }
    }
}

// ---- result hash ------------------------------------------------------------------------------------------------

void AutoResolveResult::hash(StateHasher& h) const {
    h.boolean(valid);
    h.boolean(degenerate);
    h.i32(winner);
    h.i32(loser);
    h.i32(retreating);
    h.u32(static_cast<uint32_t>(survives.size()));
    for (size_t i = 0; i < survives.size(); ++i) {
        h.u8(survives[i]);
        h.u32(starbase_type[i]);
    }
    h.u32(static_cast<uint32_t>(cargo_survives.size()));
    for (uint8_t c : cargo_survives) h.u8(c);
    for (int s = 0; s < 2; ++s) {
        h.i32(side_player[s]);
        h.f64(strength[s]);
        h.f64(budget[s]);
        h.u32(losses[s]);
        for (double v : rem[s]) h.f64(v);
    }
    h.u32(draws);
}

// ---- auto-resolve -------------------------------------------------------------------------------------------------

AutoResolveResult auto_resolve(const AutoResolveInput& in, const ContrastTable& ct, const AutoResolveConstants& k,
                               SimRng& rng) {
    AutoResolveResult r;
    const size_t n = in.combatants.size();
    r.survives.assign(n, 1);
    r.starbase_type.assign(n, 0xFFFFFFFFu);
    for (size_t i = 0; i < n; ++i)
        if (!in.combatants[i].starbase.empty()) r.starbase_type[i] = in.combatants[i].starbase.front().type;
    r.cargo_survives.assign(in.cargo.size(), 1);
    const uint64_t draws0 = rng.draws();

    // ---- sides (§4.2) ----
    std::vector<int8_t> side_of(n, -1);
    for (size_t i = 0; i < n; ++i) {
        const int32_t o = in.combatants[i].owner;
        if (r.side_player[0] == -1 || r.side_player[0] == o) {
            r.side_player[0] = o;
            side_of[i] = 0;
        } else if (r.side_player[1] == -1 || r.side_player[1] == o) {
            r.side_player[1] = o;
            side_of[i] = 1;
        }
    }
    auto side_index = [&r](int32_t player) { return player == r.side_player[0] ? 0 : player == r.side_player[1] ? 1 : -1; };
    auto info = [&in](int32_t player) {
        for (const ArSideInfo& s : in.players)
            if (s.player == player) return s;
        ArSideInfo d;
        d.player = player;
        d.ai = true;
        return d;
    };
    if (r.side_player[0] == -1) return r;  // nothing to resolve
    if (r.side_player[1] == -1) {          // one side only: it wins, nothing dies
        r.valid = true;
        r.winner = r.side_player[0];
        return r;
    }
    r.valid = true;

    // ---- strength vectors (§4.3) ----
    const size_t K = ct.size();
    const size_t tot = in.space ? 1 : 0;
    bool super[2] = {false, false}, killer[2] = {false, false};
    int32_t weakest[2] = {-1, -1};
    float weakest_p[2] = {0.0f, 0.0f};
    uint32_t count[2] = {0, 0};
    for (int s = 0; s < 2; ++s) r.vec[s].assign(2 + K, 0.0);
    auto category_entry = [&ct, K](uint64_t cats) -> int32_t {
        for (size_t j = 0; j < K; ++j)
            if ((ct.rows[j].enemy_mask & cats) != 0) return static_cast<int32_t>(2 + j);
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        if (side_of[i] < 0) continue;
        const int s = side_of[i];
        const ArCombatant& c = in.combatants[i];
        ++count[s];
        for (const ArPiece& p : c.pieces) {
            const int32_t e = category_entry(p.categories);
            if (e >= 0) r.vec[s][static_cast<size_t>(e)] += static_cast<double>(p.power);
            r.vec[s][tot] += static_cast<double>(p.power);
        }
        if (weakest[s] < 0 || c.own_power < weakest_p[s]) {
            weakest[s] = static_cast<int32_t>(i);
            weakest_p[s] = c.own_power;
        }
        super[s] = super[s] || c.super;
        killer[s] = killer[s] || c.super_killer;
    }
    r.rem[0] = r.vec[0];
    r.rem[1] = r.vec[1];
    int32_t retreating = side_index(in.retreating) >= 0 ? in.retreating : -1;
    const int att_side = side_index(in.attacker);

    // ---- degenerate space case (vf4) ----
    if (in.space && !super[0] && !super[1] && r.vec[0][1] <= 0.0 && r.vec[1][1] <= 0.0) {
        r.degenerate = true;
        int w;
        if (count[0] != count[1]) w = count[0] > count[1] ? 0 : 1;
        else w = att_side == 0 ? 1 : 0;  // tie: the side that is not the Begin player
        r.winner = r.side_player[w];
        r.loser = r.side_player[1 - w];
        r.retreating = r.loser;
        r.draws = static_cast<uint32_t>(rng.draws() - draws0);
        return r;
    }

    // ---- exchange of fire (0x60d2a0), once per side ----
    auto fire = [&](const ArPiece& p, std::vector<double>& R) {
        const float pw = p.power;
        int32_t best = -1;
        double best_score = 0.0;
        float best_c = 0.0f;
        for (size_t j = 0; j < K; ++j) {
            const double Rj = R[2 + j];
            if (!(Rj > 0.0)) continue;
            const float c = ct.contrast(j, p.name, p.categories);
            if (!(c > 0.0f)) continue;
            const double pc = static_cast<double>(pw) * static_cast<double>(c);
            const double m = std::max(Rj, pc);
            const double x = (Rj - pc) / m;
            const double score = (1.0 - x * x) * static_cast<double>(c);
            if (best < 0 || score > best_score) {
                best = static_cast<int32_t>(j);
                best_score = score;
                best_c = c;
            }
        }
        if (best < 0) {
            R[tot] -= static_cast<double>(pw);
            return;
        }
        const float bonus = 1.0f;  // planet modifier table (open item 7)
        const float d = pw * best_c * bonus;
        double& Rj = R[2 + static_cast<size_t>(best)];
        const double a = std::min(static_cast<double>(d), Rj);
        Rj -= a;
        const double left = d > 0.0f ? (1.0 - a / static_cast<double>(d)) * static_cast<double>(pw) : static_cast<double>(pw);
        R[tot] -= a + left;
    };
    for (int s = 0; s < 2; ++s)
        for (size_t i = 0; i < n; ++i)
            if (side_of[i] == s)
                for (const ArPiece& p : in.combatants[i].pieces) fire(p, r.rem[1 - s]);

    // ---- winner (0x60c010) ----
    if (super[0] && !killer[1] && retreating < 0) retreating = r.side_player[1];
    if (super[1] && !killer[0] && retreating < 0) retreating = r.side_player[0];
    for (int s = 0; s < 2; ++s) {
        double sum = 0.0;
        for (double v : r.rem[s])
            if (v > 0.0) sum += v;
        r.strength[s] = sum;
    }
    int w;
    if (retreating >= 0) {
        w = side_index(retreating) == 0 ? 1 : 0;
    } else {
        const ArSideInfo i0 = info(r.side_player[0]), i1 = info(r.side_player[1]);
        if (r.strength[0] > 0.0 && r.strength[1] > 0.0 && i0.ai && i1.ai && i0.playable != i1.playable) {
            w = i0.playable ? 0 : 1;
        } else if (r.strength[0] == r.strength[1]) {
            w = att_side >= 0 ? att_side : 0;
        } else {
            w = r.strength[0] <= r.strength[1] ? 1 : 0;
        }
    }
    const int l = 1 - w;
    r.winner = r.side_player[w];
    r.loser = r.side_player[l];
    const ArSideInfo loser_info = info(r.loser);

    // ---- transports (0x60aac0), space, loser only ----
    if (in.space) {
        uint32_t kk = 0;
        for (const auto& c : in.cargo)
            if (c.owner == r.loser) ++kk;
        int32_t m = round_half_away((1.0 - static_cast<double>(k.transport_losses)) * static_cast<double>(kk));
        if (kk == 1 || !loser_info.playable) m = 0;
        bool any = false;
        for (size_t i = 0; i < in.cargo.size(); ++i) {
            if (in.cargo[i].owner != r.loser) continue;
            if (in.cargo[i].hero) {
                any = true;
                continue;
            }
            if (m > 0) {
                --m;
                any = true;
            } else {
                r.cargo_survives[i] = 0;
            }
        }
        if (any && retreating < 0) retreating = r.loser;
    }

    // ---- budgets (§4.4.5) ----
    const bool retreat_flag = retreating >= 0;
    for (int s = 0; s < 2; ++s) {
        const double orig = r.vec[s][tot], rem = r.rem[s][tot];
        const float x = s == w ? (retreat_flag ? k.retreat_winner_attrition : k.winner_attrition)
                               : (retreat_flag ? k.retreat_loser_attrition : k.loser_attrition);
        double b = rem + (orig - rem) * (1.0 - static_cast<double>(x));
        if (s == l && !loser_info.playable) b = 0.0;
        if (in.from_tactical) b *= static_cast<double>(k.tactical_multiplier);
        r.budget[s] = b;
    }

    // ---- survival roll (0x60a140): winner first, then loser ----
    const double allow = static_cast<double>(k.attrition_allowance);
    for (int s : {w, l}) {
        const bool loser = s == l;
        double b = r.budget[s];
        std::vector<uint32_t> list;
        for (size_t i = 0; i < n; ++i)
            if (side_of[i] == s) list.push_back(static_cast<uint32_t>(i));
        bool any = false;
        while (!list.empty()) {
            size_t pick = list.size();
            for (size_t q = 0; q < list.size(); ++q) {
                const ArCombatant& c = in.combatants[list[q]];
                if (c.hero || c.always_dies) {
                    pick = q;
                    break;
                }
            }
            if (pick == list.size()) pick = static_cast<size_t>(rng.range_int(0, static_cast<int32_t>(list.size()) - 1));
            const uint32_t ci = list[pick];
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(pick));
            const ArCombatant& c = in.combatants[ci];
            bool survive = false;
            if (loser && c.super) {
                survive = !killer[w];
            } else if (!c.starbase.empty()) {
                r.starbase_type[ci] = 0xFFFFFFFFu;
                for (const ArStarbaseLevel& lv : c.starbase)
                    if (static_cast<double>(lv.power) * allow <= b) {
                        survive = true;
                        r.starbase_type[ci] = lv.type;
                        b = std::max(0.0, b - static_cast<double>(lv.power));
                        break;
                    }
            } else if (loser && c.always_dies) {
                survive = false;
            } else {
                double cost = 0.0;
                for (const ArPiece& p : c.pieces) cost += static_cast<double>(p.power);
                if (b - cost * allow > 0.0) {
                    survive = true;
                    b = std::max(0.0, b - cost);
                }
            }
            r.survives[ci] = survive ? 1 : 0;
            if (!survive) ++r.losses[s];
            any = any || survive;
        }
        if (!loser && !any && weakest[s] >= 0) {  // the winner keeps its weakest combatant
            const uint32_t wi = static_cast<uint32_t>(weakest[s]);
            r.survives[wi] = 1;
            --r.losses[s];
            if (!in.combatants[wi].starbase.empty()) r.starbase_type[wi] = in.combatants[wi].starbase.back().type;
        }
        if (loser && any && retreating < 0) retreating = r.loser;
    }
    r.retreating = retreating;
    r.draws = static_cast<uint32_t>(rng.draws() - draws0);
    return r;
}

}  // namespace alamo::galactic

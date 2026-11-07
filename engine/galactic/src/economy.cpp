#include "alamo/galactic/economy.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::galactic {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

}  // namespace

ForceSide parse_force_side(std::string_view text) {
    text = trim_ws(text);
    if (iequals(text, "Light")) return ForceSide::Light;
    if (iequals(text, "Dark")) return ForceSide::Dark;
    return ForceSide::Neutral;
}

PiecewiseCurve PiecewiseCurve::parse(std::string_view text) {
    PiecewiseCurve c;
    std::vector<float> v;
    for (std::string_view tok : objects::split_type_list(text)) v.push_back(static_cast<float>(objects::c_atof(tok)));
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
        c.x.push_back(v[i]);
        c.y.push_back(v[i + 1]);
    }
    return c;
}

float PiecewiseCurve::eval(float v) const {
    if (x.empty()) return 0.0f;
    if (v <= x.front()) return y.front();
    if (v >= x.back()) return y.back();
    for (size_t i = 0; i + 1 < x.size(); ++i) {
        if (v < x[i + 1]) {
            const float span = x[i + 1] - x[i];
            if (!(span > 0.0f)) return y[i + 1];
            const float t = (v - x[i]) / span;
            return y[i] + (y[i + 1] - y[i]) * t;
        }
    }
    return y.back();
}

namespace econ {

int32_t fiscal_period_frames(float seconds) {
    const float f = 30.0f * seconds;
    return static_cast<int32_t>(std::round(f));  // std::round: half away from zero
}

int32_t timer_frames(float seconds) {
    const float f = 30.0f * seconds;
    if (!(f > -2147483520.0f && f < 2147483520.0f)) return f > 0.0f ? std::numeric_limits<int32_t>::max() : 0;
    return static_cast<int32_t>(f);  // truncates toward zero
}

float timer_remaining_seconds(int32_t start, int32_t duration, int32_t now) {
    const int64_t r = static_cast<int64_t>(start) + duration - now;
    return r > 0 ? static_cast<float>(r) / 30.0f : 0.0f;
}

float add_credits(CreditAccount& a, float amount, bool ai_controlled, float credit_multiplier) {
    if (amount > 0.0f && ai_controlled) amount *= credit_multiplier;
    const float before = a.credits;
    a.credits += amount;
    if (a.credits < 0.0f) a.credits = 0.0f;
    if (a.cap >= 0.0f && a.credits > a.cap) a.credits = a.cap;
    return a.credits - before;
}

float credit_cap(int32_t planets_owned, float cap_per_planet) {
    return static_cast<float>(std::max(1, planets_owned)) * cap_per_planet;
}

float income_base(int32_t b, float black_market) { return static_cast<float>(b) * black_market; }

float income_force(int32_t b, ForceSide owner, ForceSide planet, float strength, float bonus, float penalty,
                   bool* keep) {
    const float fb = static_cast<float>(b);
    if (owner != ForceSide::Neutral) {
        if (keep) *keep = true;
        if (owner == planet) return fb * strength * bonus;
        return -fb * strength * penalty;
    }
    if (keep) *keep = false;
    return 0.0f;
}

float income_political(int32_t b, float control, const PiecewiseCurve& curve) {
    const float q = curve.eval(control);
    if (!(q > 0.0f)) return 0.0f;
    const float fb = static_cast<float>(b);
    return q * fb - fb;
}

float income_trade(int32_t b, float gain_sum) { return static_cast<float>(b) * gain_sum; }

float income_finish(float sum, bool has_min, float min_value, bool has_max, float max_value, bool siphon,
                    float siphon_fraction, float* siphoned) {
    if (has_min && sum < min_value) sum = min_value;
    if (has_max && sum > max_value) sum = max_value;
    if (siphoned) *siphoned = 0.0f;
    if (siphon && siphon_fraction >= 0.0f && siphon_fraction <= 1.0f) {
        const float s = sum * siphon_fraction;
        if (siphoned) *siphoned = s;
        return sum - s;
    }
    return sum;
}

float corruption_income_pct(const EconomyConstants& c, int32_t type) {
    if (type < 0 || type >= static_cast<int32_t>(c.corruption_income_pct.size())) return 0.0f;
    return c.corruption_income_pct[static_cast<size_t>(type)];
}

float build_time_t0(int32_t build_time_seconds, float production_speed_factor) {
    const float p = static_cast<float>(build_time_seconds) * production_speed_factor;
    if (!(p > 0.0f)) return 0.0f;
    if (p >= 4294967040.0f) return 4294967040.0f;
    return static_cast<float>(static_cast<uint32_t>(p));  // truncation (0x371230) [H]
}

float tech_time_factor(int32_t starbase_level, int32_t tech_level, const std::array<float, 5>& table) {
    const int32_t d = starbase_level - tech_level;
    if (d <= 0) return 1.0f;
    if (d >= 4) return table[4];
    return table[static_cast<size_t>(d)];
}

float build_time(const BuildTimeInputs& in) {
    if (in.cheat) return 5.0f;
    const float t0 = build_time_t0(in.build_time_seconds, in.production_speed_factor);
    float m_tech = 1.0f;
    if (in.tech_factor_applies && in.starbase_level >= 1 && in.starbase_level <= 5 && in.base_vs_tech)
        m_tech = tech_time_factor(in.starbase_level, in.tech_level, *in.base_vs_tech);
    const float m_planet = 1.0f - in.planet_bonus;
    float t = m_planet * t0;
    t = t * m_tech;
    t = t * in.ai_multiplier;
    t = t * in.corruption_multiplier;
    return t / static_cast<float>(std::max(1, in.factories));
}

int32_t build_cost(int32_t build_cost_credits, float planet_cost_mult, float corruption_price_mult,
                   float tech_upgrade_mult) {
    float raw = planet_cost_mult * static_cast<float>(build_cost_credits);
    raw = raw * corruption_price_mult;
    raw = raw * tech_upgrade_mult;
    if (raw > 0.0f) raw = std::max(1.0f, raw);
    if (!(raw > -2147483520.0f && raw < 2147483520.0f)) return raw > 0.0f ? std::numeric_limits<int32_t>::max() : 0;
    return static_cast<int32_t>(std::round(raw));
}

float payg_installment(float prev_remaining, float remaining, float build_time_s, float cost) {
    if (!(build_time_s > 0.0f)) return cost;
    return (prev_remaining - remaining) / build_time_s * cost;
}

float refund_amount(bool pay_as_you_go, float remaining, float build_time_s, float cost) {
    if (!pay_as_you_go) return cost;
    if (!(build_time_s > 0.0f)) return cost;
    const float paid = std::clamp(1.0f - remaining / build_time_s, 0.0f, 1.0f);
    return paid * cost;
}

float hop_factor(bool mover_benefits, bool a_corrupted, bool b_corrupted, bool has_route, float route_hs,
                 float corruption_bonus) {
    float r = 1.0f;
    if (mover_benefits && a_corrupted && b_corrupted) r = corruption_bonus;
    if (has_route && route_hs >= r) r = route_hs;
    return r;
}

float base_speed(float slowest_hyperspace_speed, float faction_factor, float m) {
    return slowest_hyperspace_speed * faction_factor * m;
}

bool fleet_step(Vec3& pos, const Vec3& target, float v, Vec3* facing) {
    const float dx = target.x - pos.x, dy = target.y - pos.y, dz = target.z - pos.z;
    const float len2 = dx * dx + dy * dy + dz * dz;
    if (!(len2 > 0.0f)) {
        pos = target;
        return true;
    }
    const float len = dmath::sqrt(len2);
    if (facing) *facing = {dx / len, dy / len, dz / len};
    if (!(v > 0.0f)) return false;
    const float k = v / len;
    const Vec3 vel{dx * k, dy * k, dz * k};
    const Vec3 np{pos.x + vel.x, pos.y + vel.y, pos.z + vel.z};
    const float rx = target.x - np.x, ry = target.y - np.y, rz = target.z - np.z;
    const float rem = dmath::sqrt(rx * rx + ry * ry + rz * rz);
    const float speed = dmath::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    if (rem >= 2.0f * speed) {
        pos = np;
        return false;
    }
    pos = target;
    return true;
}

float political_delta(int32_t elapsed_frames, float m, float change_seconds) {
    const float denom = 30.0f * m * change_seconds;
    if (!(denom > 0.0f)) return 1.0f;
    return static_cast<float>(elapsed_frames) / denom;
}

float influence_alignment_factor(ForceSide owner, ForceSide planet, float strength, float bonus, float penalty) {
    if (owner == ForceSide::Neutral || planet == ForceSide::Neutral) return 1.0f;
    if (owner == planet) return 1.0f - strength * bonus;
    return 1.0f + strength * penalty;
}

float political_ramp(float control, float target, float delta) {
    if (control < target) return std::min(target, control + delta);
    if (control > target) return std::max(target, control - delta);
    return control;
}

}  // namespace econ
}  // namespace alamo::galactic

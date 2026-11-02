#include "alamo/ai/ai_budget.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"

namespace alamo::ai {

void AiBudget::reset(size_t categories) {
    cat_.assign(categories, Category{});
    rate68_ = period_ = refund_ = 0.0f;
    reset_ = init_ = false;
}

const AiBudget::GoalEntry* AiBudget::find(int32_t c, uint32_t goal) const {
    if (!valid(c)) return nullptr;
    for (const GoalEntry& e : cat_[static_cast<size_t>(c)].goals)
        if (e.goal == goal) return &e;
    return nullptr;
}

AiBudget::GoalEntry* AiBudget::find(int32_t c, uint32_t goal) {
    if (!valid(c)) return nullptr;
    for (GoalEntry& e : cat_[static_cast<size_t>(c)].goals)
        if (e.goal == goal) return &e;
    return nullptr;
}

void AiBudget::normalise(std::span<const float> raw) {
    float sum = 0.0f;
    for (size_t i = 0; i < cat_.size(); ++i) {
        const float w = i < raw.size() && raw[i] > 0.0f ? raw[i] : 0.0f;  // negatives become 0
        cat_[i].weight = w;
        sum += w;
    }
    for (Category& k : cat_) k.weight = sum != 0.0f ? k.weight / sum : 1.0f / static_cast<float>(cat_.size());
}

void AiBudget::init(std::span<const float> raw, float credits) {
    normalise(raw);
    for (Category& k : cat_) {
        k.applied = k.weight;
        k.A = k.P = credits * k.weight;
        k.R = k.I = 0.0f;
    }
    rate68_ = period_ = refund_ = 0.0f;
    reset_ = false;
    init_ = true;
}

void AiBudget::reweight(std::span<const float> raw) {
    normalise(raw);
    for (Category& k : cat_) {
        k.A = k.P;
        k.R = k.I;
        for (GoalEntry& e : k.goals) {
            k.A -= e.a;
            e.c = 0.0f;
            e.readd = true;
        }
    }
}

void AiBudget::tick(float income, float period, float credits) {
    const float T = period;
    if (!(T > 0.0f)) return;
    const float rate = income / T;
    rate68_ = rate;
    float sigma = refund_;
    for (Category& k : cat_) {
        k.P = k.A;
        const float iprev = k.I;
        k.I = k.applied * rate;
        for (GoalEntry& e : k.goals) {
            const float share = iprev > 0.0f ? e.q / iprev * k.I : 0.0f;
            const float r = std::max(e.req, 0.0f);
            const float x = e.h > 0.0f ? r / e.h - share : share - r / T;
            const float xp = x > 0.0f ? std::max(x, 0.0001f / T) : 0.0f;
            e.a += xp * T;
            e.alloc = std::max(e.alloc, e.s);
            k.P += (share - xp) * T + e.c + e.a;
            e.h = std::max(e.h - T, 0.0f);
        }
        k.applied = k.weight;
        sigma += k.P;
    }
    for (Category& k : cat_) {
        if (reset_) k.P = sigma * k.weight;
        k.P += (credits - sigma) * k.weight;  // drift redistribution
    }
    period_ = T;
    reset_ = false;
}

void AiBudget::readd(Category& k, GoalEntry& e) const {
    const float T = period_;
    float need = std::max(e.req, 0.0f) + e.alloc - e.a;
    if (need < 0.0f) {
        k.A -= need;  // give back
        e.a += need;
        need = 0.0f;
    }
    k.A -= need;
    e.c = need;
    float shortfall = 0.0f;
    if (k.A < 0.0f) {
        const float back = std::min(need, -k.A);
        k.A += back;
        e.c -= back;
        shortfall = back;
    }
    const float span = e.h > 0.0f ? e.h : T;
    e.q = span > 0.0f ? shortfall / span : 0.0f;  // [port] guard: no tick yet (T = 0) -> no rate
    k.R -= e.q;
    if (k.R < 0.0f && k.A <= 0.0f) {
        e.q = std::max(k.R + e.q, 0.0f);
        k.A += k.R * span;
        e.c -= k.R * span;
        k.R = 0.0f;
    }
    e.readd = false;
}

bool AiBudget::add_goal(int32_t c, uint32_t goal, float cost, float eta, float time_limit) {
    if (!valid(c)) return false;
    Category& k = cat_[static_cast<size_t>(c)];
    GoalEntry* e = find(c, goal);
    if (!e) {
        GoalEntry n;
        n.goal = goal;
        n.req = cost;
        n.h = time_limit >= 0.0f ? std::min(eta, time_limit) : eta;
        ++k.mean_n;
        k.mean_h += (n.h - k.mean_h) / static_cast<float>(k.mean_n);
        k.goals.push_back(n);
        readd(k, k.goals.back());
        return true;
    }
    if (e->readd) readd(k, *e);
    return true;
}

bool AiBudget::can_fund(int32_t c, float cost, float eta, float time_limit) const {
    if (!valid(c)) return cost <= 0.0f;
    const Category& k = cat_[static_cast<size_t>(c)];
    const float horizon = time_limit >= 0.0f ? std::min(eta, time_limit) : eta;
    return std::max(std::max(horizon, period_) * k.R + k.A, 0.0f) >= cost;
}

bool AiBudget::can_fund_goal(int32_t c, uint32_t goal) const {
    const GoalEntry* e = find(c, goal);
    if (!e) return false;
    const Category& k = cat_[static_cast<size_t>(c)];
    return std::max(std::max(e->h, period_) * k.R + k.A, 0.0f) >= std::max(e->req, 0.0f) + e->alloc - e->a;
}

float AiBudget::time_to_afford(int32_t c, uint32_t goal, float n) const {
    const GoalEntry* e = find(c, goal);
    if (!e) return 0.0f;
    const float have = std::min(e->alloc, e->c + e->a);
    const float d = n + e->s - have;
    if (static_cast<int32_t>(d) <= 0) return 0.0f;
    float rate = std::max(e->alloc - e->c - e->a, d);
    if (e->h > 0.0f) rate /= e->h;
    return d / rate;
}

void AiBudget::spend(int32_t c, uint32_t goal, float n) {
    if (GoalEntry* e = find(c, goal)) {
        e->req -= n;
        e->alloc += n;
    }
}

void AiBudget::paid(int32_t c, uint32_t goal, float n) {
    if (GoalEntry* e = find(c, goal)) e->s += n;
}

float AiBudget::installment(int32_t c, uint32_t goal, float n) {
    GoalEntry* e = find(c, goal);
    if (!e) {
        refund_ = std::max(refund_ - n, 0.0f);
        return n;
    }
    const float have = std::min(e->alloc, e->c + e->a);
    if (static_cast<int32_t>(have - n) < 0) {
        if (e->alloc < n) e->s += n - e->alloc;
        return -1.0f;
    }
    const float p = std::min(n, have);
    cat_[static_cast<size_t>(c)].P -= p;
    e->s = std::max(e->s - p, 0.0f);
    e->alloc = std::max(e->alloc - p, 0.0f);
    e->a -= p;
    if (e->a < 0.0f) {
        e->c = std::max(e->c + e->a, 0.0f);
        e->a = 0.0f;
    }
    return p;
}

void AiBudget::set_request(int32_t c, uint32_t goal, float req) {
    if (GoalEntry* e = find(c, goal)) e->req = req;
}

void AiBudget::set_alloc(int32_t c, uint32_t goal, float alloc) {
    if (GoalEntry* e = find(c, goal)) e->alloc = alloc;
}

void AiBudget::release(int32_t c, uint32_t goal, float reclaimed) {
    if (!valid(c)) return;
    Category& k = cat_[static_cast<size_t>(c)];
    for (size_t i = 0; i < k.goals.size(); ++i) {
        if (k.goals[i].goal != goal) continue;
        const GoalEntry& e = k.goals[i];
        const float back = e.c + e.a;
        k.A += back;
        if (back > 0.0f && reclaimed != 0.0f) {
            k.A -= reclaimed;
            k.P -= reclaimed;
            refund_ += reclaimed;
        }
        k.goals.erase(k.goals.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void AiBudget::flush(int32_t c) {
    if (valid(c)) cat_[static_cast<size_t>(c)].A = 0.0f;
}

float AiBudget::pools_total() const {
    float s = 0.0f;
    for (const Category& k : cat_) s += k.P;
    return s;
}

void AiBudget::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(cat_.size()));
    for (const Category& k : cat_) {
        h.f32(k.applied);
        h.f32(k.weight);
        h.f32(k.A);
        h.f32(k.R);
        h.f32(k.P);
        h.f32(k.I);
        h.f32(k.mean_h);
        h.u32(static_cast<uint32_t>(k.goals.size()));
        for (const GoalEntry& e : k.goals) {
            h.u32(e.goal);
            h.f32(e.req);
            h.f32(e.alloc);
            h.f32(e.h);
            h.f32(e.q);
            h.f32(e.a);
            h.f32(e.c);
            h.f32(e.s);
            h.boolean(e.readd);
        }
    }
    h.f32(rate68_);
    h.f32(period_);
    h.f32(refund_);
    h.boolean(reset_);
}

}  // namespace alamo::ai

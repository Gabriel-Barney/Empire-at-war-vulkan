#include "alamo/core/profile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace alamo::profile {

namespace detail {
std::atomic<bool> g_enabled{false};
}

namespace {

constexpr size_t kNameChars = 48;

struct ZoneTable {
    std::mutex mu;
    char names[kMaxZones][kNameChars] = {};
    std::atomic<uint32_t> count{0};
};
ZoneTable& zones() {
    static ZoneTable t;
    return t;
}

struct alignas(64) ThreadAcc {
    std::atomic<int64_t> ns[kMaxZones];
    ThreadAcc() {
        for (auto& v : ns) v.store(0, std::memory_order_relaxed);
    }
};

struct State {
    ThreadAcc acc[kMaxThreads];
    std::atomic<int64_t> busy[kMaxBusySlots];
    std::atomic<uint32_t> next_thread{0};
    std::vector<TickRecord> ring;
    uint64_t closed = 0;
    int64_t tick_start = 0;
    int64_t lap_at = 0;
    uint64_t frame = 0;
    bool in_tick = false;
    State() {
        for (auto& b : busy) b.store(0, std::memory_order_relaxed);
    }
};
State& st() {
    static State s;
    return s;
}

uint32_t thread_slot() {
    thread_local uint32_t slot = st().next_thread.fetch_add(1, std::memory_order_relaxed) % kMaxThreads;
    return slot;
}

uint32_t clamp_u32(int64_t v) {
    if (v <= 0) return 0;
    return v > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
}

}  // namespace

ZoneId register_zone(const char* name) {
    ZoneTable& t = zones();
    std::lock_guard lk(t.mu);
    const uint32_t n = t.count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i)
        if (std::strncmp(t.names[i], name, kNameChars - 1) == 0) return static_cast<ZoneId>(i);
    if (n >= kMaxZones) return kNoZone;
    std::strncpy(t.names[n], name, kNameChars - 1);
    t.count.store(n + 1, std::memory_order_release);
    return static_cast<ZoneId>(n);
}

const char* zone_name(ZoneId id) {
    ZoneTable& t = zones();
    return id < t.count.load(std::memory_order_acquire) ? t.names[id] : "?";
}

uint32_t zone_count() { return zones().count.load(std::memory_order_acquire); }

int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void add_zone_ns(ZoneId id, int64_t ns) noexcept {
    if (id >= kMaxZones) return;
    st().acc[thread_slot()].ns[id].fetch_add(ns, std::memory_order_relaxed);
}

void add_busy_ns(uint32_t slot, int64_t ns) noexcept {
    st().busy[slot < kMaxBusySlots ? slot : kMaxBusySlots - 1].fetch_add(ns, std::memory_order_relaxed);
}

void start(uint32_t ring_ticks) {
    State& s = st();
    detail::g_enabled.store(false, std::memory_order_relaxed);
    s.ring.assign(std::max(ring_ticks, 1u), TickRecord{});
    s.closed = 0;
    s.in_tick = false;
    for (auto& a : s.acc)
        for (auto& v : a.ns) v.store(0, std::memory_order_relaxed);
    for (auto& b : s.busy) b.store(0, std::memory_order_relaxed);
    detail::g_enabled.store(true, std::memory_order_relaxed);
}

void stop() {
    detail::g_enabled.store(false, std::memory_order_relaxed);
    st().in_tick = false;
}

void begin_tick(uint64_t frame) {
    if (!enabled()) return;
    State& s = st();
    s.frame = frame;
    s.tick_start = s.lap_at = now_ns();
    s.in_tick = true;
    // Work done between ticks (background jobs, the host's own frame) is not part of this tick.
    for (auto& a : s.acc)
        for (auto& v : a.ns) v.store(0, std::memory_order_relaxed);
    for (auto& b : s.busy) b.store(0, std::memory_order_relaxed);
}

void lap_impl(const char* zone) {
    if (!enabled()) return;
    State& s = st();
    if (!s.in_tick) return;
    const int64_t t = now_ns();
    const ZoneId id = register_zone(zone);
    if (id != kNoZone) s.acc[thread_slot()].ns[id].fetch_add(t - s.lap_at, std::memory_order_relaxed);
    s.lap_at = t;
}

void end_tick() {
    if (!enabled()) return;
    State& s = st();
    if (!s.in_tick || s.ring.empty()) return;
    const int64_t end = now_ns();
    TickRecord& r = s.ring[s.closed % s.ring.size()];
    r = TickRecord{};
    r.frame = s.frame;
    r.total_ns = clamp_u32(end - s.tick_start);
    const uint32_t zc = zone_count();
    for (uint32_t z = 0; z < zc; ++z) {
        int64_t sum = 0;
        for (auto& a : s.acc) sum += a.ns[z].exchange(0, std::memory_order_relaxed);
        r.zone_ns[z] = clamp_u32(sum);
    }
    for (uint32_t b = 0; b < kMaxBusySlots; ++b) r.busy_ns[b] = clamp_u32(s.busy[b].exchange(0, std::memory_order_relaxed));
    ++s.closed;
    s.in_tick = false;
}

uint64_t ticks_recorded() { return st().closed; }

std::vector<TickRecord> snapshot() {
    State& s = st();
    std::vector<TickRecord> out;
    const uint64_t n = std::min<uint64_t>(s.closed, s.ring.size());
    out.reserve(static_cast<size_t>(n));
    const uint64_t first = s.closed - n;
    for (uint64_t i = 0; i < n; ++i) out.push_back(s.ring[(first + i) % s.ring.size()]);
    return out;
}

// ------------------------------------------------------------------------------------------------ report

namespace {
double ms(uint64_t ns) { return static_cast<double>(ns) / 1e6; }
// Nearest-rank percentile of an ascending vector.
double pct(const std::vector<uint32_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t i = static_cast<size_t>(p * static_cast<double>(sorted.size()));
    if (i >= sorted.size()) i = sorted.size() - 1;
    return ms(sorted[i]);
}
}  // namespace

Report build_report(const std::vector<TickRecord>& ticks, double budget_ms, uint32_t worst_count) {
    Report rep;
    rep.ticks = ticks.size();
    rep.budget_ms = budget_ms;
    if (ticks.empty()) return rep;

    std::vector<uint32_t> totals;
    totals.reserve(ticks.size());
    uint64_t sum_total = 0;
    for (const TickRecord& t : ticks) {
        totals.push_back(t.total_ns);
        sum_total += t.total_ns;
        if (ms(t.total_ns) > budget_ms) ++rep.over_budget;
    }
    std::sort(totals.begin(), totals.end());
    rep.mean_ms = ms(sum_total) / static_cast<double>(ticks.size());
    rep.p50_ms = pct(totals, 0.50);
    rep.p99_ms = pct(totals, 0.99);
    rep.max_ms = ms(totals.back());

    const uint32_t zc = zone_count();
    std::vector<uint32_t> col(ticks.size());
    for (uint32_t z = 0; z < zc; ++z) {
        ZoneStat zs;
        zs.name = zone_name(static_cast<ZoneId>(z));
        uint64_t sum = 0;
        for (size_t i = 0; i < ticks.size(); ++i) {
            col[i] = ticks[i].zone_ns[z];
            sum += col[i];
            if (col[i]) ++zs.active_ticks;
        }
        if (sum == 0) continue;
        std::sort(col.begin(), col.end());
        zs.mean_ms = ms(sum) / static_cast<double>(ticks.size());
        zs.p50_ms = pct(col, 0.50);
        zs.p99_ms = pct(col, 0.99);
        zs.max_ms = ms(col.back());
        zs.share = sum_total ? static_cast<double>(sum) / static_cast<double>(sum_total) : 0;
        rep.zones.push_back(std::move(zs));
    }
    std::stable_sort(rep.zones.begin(), rep.zones.end(),
                     [](const ZoneStat& a, const ZoneStat& b) { return a.share > b.share; });

    std::vector<size_t> order(ticks.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    const size_t k = std::min<size_t>(worst_count, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(k), order.end(), [&](size_t a, size_t b) {
        return ticks[a].total_ns != ticks[b].total_ns ? ticks[a].total_ns > ticks[b].total_ns
                                                       : ticks[a].frame < ticks[b].frame;
    });
    for (size_t n = 0; n < k; ++n) {
        const TickRecord& t = ticks[order[n]];
        WorstTick w;
        w.frame = t.frame;
        w.total_ms = ms(t.total_ns);
        std::vector<std::pair<uint32_t, uint32_t>> zv;  // (ns, zone)
        for (uint32_t z = 0; z < zc; ++z)
            if (t.zone_ns[z]) zv.emplace_back(t.zone_ns[z], z);
        std::sort(zv.begin(), zv.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        for (size_t j = 0; j < zv.size() && j < 4; ++j)
            w.top.emplace_back(zone_name(static_cast<ZoneId>(zv[j].second)), ms(zv[j].first));
        rep.worst.push_back(std::move(w));
    }

    uint32_t last_busy = 0;
    std::vector<uint64_t> busy(kMaxBusySlots, 0);
    for (const TickRecord& t : ticks)
        for (uint32_t b = 0; b < kMaxBusySlots; ++b) busy[b] += t.busy_ns[b];
    for (uint32_t b = 0; b < kMaxBusySlots; ++b)
        if (busy[b]) last_busy = b + 1;
    for (uint32_t b = 0; b < last_busy; ++b)
        rep.busy_share.push_back(sum_total ? static_cast<double>(busy[b]) / static_cast<double>(sum_total) : 0);
    return rep;
}

std::string format_report(const Report& r) {
    std::string out;
    char buf[256];
    auto add = [&](const char* fmt, auto... a) {
        std::snprintf(buf, sizeof buf, fmt, a...);
        out += buf;
    };
    add("=== tick profile: %llu ticks\n", static_cast<unsigned long long>(r.ticks));
    if (r.ticks == 0) return out;
    add("tick ms: mean %.3f  p50 %.3f  p99 %.3f  max %.3f  | budget %.2f ms: %llu tick(s) over (%.2f%%)\n", r.mean_ms,
        r.p50_ms, r.p99_ms, r.max_ms, r.budget_ms, static_cast<unsigned long long>(r.over_budget),
        100.0 * static_cast<double>(r.over_budget) / static_cast<double>(r.ticks));
    add("%-28s %8s %9s %9s %9s %9s %7s\n", "zone (thread time, ms)", "active", "mean", "p50", "p99", "max", "share");
    for (const ZoneStat& z : r.zones)
        add("%-28s %8llu %9.3f %9.3f %9.3f %9.3f %6.1f%%\n", z.name.c_str(), static_cast<unsigned long long>(z.active_ticks),
            z.mean_ms, z.p50_ms, z.p99_ms, z.max_ms, 100.0 * z.share);
    if (!r.busy_share.empty()) {
        out += "job slot busy (share of tick wall time; slot 0 = sim thread helping):";
        for (size_t i = 0; i < r.busy_share.size(); ++i) add(" %zu:%.0f%%", i, 100.0 * r.busy_share[i]);
        out += "\n";
    }
    add("worst %zu ticks:\n", r.worst.size());
    for (const WorstTick& w : r.worst) {
        add("  frame %-8llu %8.3f ms ", static_cast<unsigned long long>(w.frame), w.total_ms);
        for (const auto& [name, v] : w.top) add(" %s=%.3f", name.c_str(), v);
        out += "\n";
    }
    return out;
}

}  // namespace alamo::profile

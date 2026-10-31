#include "alamo/sim/scenario.hpp"

#include <chrono>
#include <thread>
#include <vector>

#include "alamo/commands/command.hpp"

namespace alamo::sim {

namespace cmd = alamo::commands;

namespace {

std::vector<ObjectHandle> alive_owned(const objects::ObjectStore& s, int32_t player, bool movable_only,
                                      const TacticalMode& tm) {
    std::vector<ObjectHandle> out;
    for (ObjectHandle h : s.owned_by(player))
        if (s.active(h) && (!movable_only || tm.movable(h))) out.push_back(h);
    return out;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

uint32_t RandomOrders::issue(SimHost& host) {
    TacticalMode* tm = host.tactical();
    if (!tm) return 0;
    const objects::ObjectStore& store = tm->primary();
    const auto all = store.all_objects();
    if (all.empty()) return 0;
    auto pick = [this](size_t n) { return static_cast<size_t>(rng_.range_int(0, static_cast<int32_t>(n) - 1)); };
    const int32_t count = rng_.range_int(0, static_cast<int32_t>(max_));
    uint32_t accepted = 0;
    for (int32_t k = 0; k < count; ++k) {
        const auto player = static_cast<uint8_t>(rng_.range_int(0, static_cast<int32_t>(host.players()) - 1));
        const auto own = alive_owned(store, player, false, *tm);
        const int32_t kind = rng_.range_int(0, 99);
        cmd::EventData ev;
        if (kind < 30) {
            std::vector<uint32_t> ids;
            if (own.empty()) {
                ids.push_back(host.wire_id(all[pick(all.size())]));  // foreign: rejected
            } else {
                const auto movers = alive_owned(store, player, true, *tm);
                const auto& from = movers.empty() ? own : movers;
                const int32_t n = rng_.range_int(1, from.size() < 6 ? static_cast<int32_t>(from.size()) : 6);
                for (int32_t i = 0; i < n; ++i) ids.push_back(host.wire_id(from[pick(from.size())]));
            }
            ev = cmd::ev::select(std::move(ids));
        } else if (kind < 55) {
            const tactical::Aabb& b = tm->bounds();
            const float x0 = clampf(b.min.x, -20000.0f, 20000.0f), x1 = clampf(b.max.x, -20000.0f, 20000.0f);
            const float y0 = clampf(b.min.y, -20000.0f, 20000.0f), y1 = clampf(b.max.y, -20000.0f, 20000.0f);
            float x = rng_.range_float(x0, x1), y = rng_.range_float(y0, y1);
            if (rng_.percent(5)) x = x1 + 1000.0f;  // out of bounds: rejected
            ev = cmd::ev::move_to_position(x, y, 0.0f);
        } else if (kind < 75) {
            const ObjectHandle t = all[pick(all.size())];  // sometimes own: rejected
            ev = cmd::ev::attack(host.wire_id(t), {});
        } else if (kind < 85) {
            ev = cmd::ev::move_to_object(host.wire_id(all[pick(all.size())]));
        } else if (kind < 95) {
            ev = cmd::ev::stop();
        } else {
            ev = cmd::ev::select_all();
        }
        if (host.submit(player, std::move(ev)).ok()) ++accepted;
    }
    return accepted;
}

void AiStandIn::operator()(SimHost& host, uint8_t player, uint64_t frame) const {
    if (period == 0 || (frame + player) % period != 0) return;
    TacticalMode* tm = host.tactical();
    if (!tm) return;
    const objects::ObjectStore& store = tm->primary();
    const auto units = alive_owned(store, player, true, *tm);
    if (units.empty()) return;
    SimRng& rng = host.random().synced();
    const ObjectHandle u = units[static_cast<size_t>(rng.range_int(0, static_cast<int32_t>(units.size()) - 1))];
    const Vec3 p = store.position(u);
    tactical::QueryContext ctx;
    tactical::CircleXY c;
    c.center = {p.x, p.y, p.z};
    c.radius = radius;
    for (tactical::EntityHandle e : tm->trees().all().query_circle_xy(c, tactical::kSkipDead, ctx)) {
        const ObjectHandle t = store.find(static_cast<ObjectId>(e));
        if (!store.active(t)) continue;
        const int32_t owner = store.owner(t);
        if (owner == static_cast<int32_t>(player) || owner < 0 || owner >= static_cast<int32_t>(host.players())) continue;
        host.execute_direct(player, cmd::CommandSource::AI, cmd::ev::attack(host.wire_id(t), {host.wire_id(u)}));
        return;
    }
}

MapRunResult run_map(const objects::GameObjectTypes& types, const terrain::MapData& map, const TypeCrcIndex& index,
                     const MapRunSpec& spec) {
    MapRunResult r;
    jobs::JobSystemConfig jc;
    jc.workers = spec.workers;
    jobs::JobSystem js(jc);
    SimConfig cfg;
    cfg.seed = spec.seed;
    SimHost host(types, js, cfg);
    if (spec.replay) host.replay_from(spec.replay);
    host.start_game(map.is_space() ? ModeKind::Space : ModeKind::Land);
    r.spawn = spawn_map(*host.tactical(), map, index, host.parallel_for());
    if (spec.ai)
        for (uint32_t p = 0; p < host.players(); ++p) host.set_controller(static_cast<uint8_t>(p), AiStandIn{});
    if (spec.record && !spec.replay) host.record_to(spec.record);
    host.set_trace(spec.trace);
    RandomOrders gen(spec.seed);
    const bool live = spec.live_orders && !spec.replay;
    // UI input arrives between ticks: once before the first tick and once after every tick, in both
    // the fast and the paced loop, so both see the same order stream.
    auto between = [&] {
        if (live) r.orders_live += gen.issue(host);
    };
    const auto t0 = std::chrono::steady_clock::now();
    between();
    if (spec.realtime) {
        auto now_ms = [t0] {
            return static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        };
        auto sleep_ms = [](uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        r.ticks = host.run_realtime(spec.ticks, now_ms, sleep_ms, kDefaultFpsCap, between);
    } else {
        for (uint64_t i = 0; i < spec.ticks && host.tick_once(); ++i) {
            ++r.ticks;
            between();
        }
    }
    r.run_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.log = host.crc_log();
    r.applied = host.commands_applied();
    r.rejected = host.commands_rejected();
    r.final_frame = host.frame();
    r.live_objects = static_cast<uint32_t>(host.tactical()->primary().live_count());
    r.orders_pending = static_cast<uint32_t>(host.tactical()->orders().size());
    return r;
}

}  // namespace alamo::sim

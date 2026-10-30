#include "alamo/render/particles/particle_sim.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace alamo::render::particles {

// ------------------------------------------------------------------------------------------ ranges
void RangeAllocator::init(uint32_t capacity) {
    capacity_ = capacity;
    used_ = 0;
    free_.clear();
    if (capacity) free_.push_back({0, capacity});
}

uint32_t RangeAllocator::allocate(uint32_t count) {
    if (count == 0) return UINT32_MAX;
    for (size_t i = 0; i < free_.size(); ++i) {
        if (free_[i].count < count) continue;
        const uint32_t base = free_[i].base;
        free_[i].base += count;
        free_[i].count -= count;
        if (free_[i].count == 0) free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        used_ += count;
        return base;
    }
    return UINT32_MAX;
}

void RangeAllocator::free(uint32_t base, uint32_t count) {
    if (count == 0 || base == UINT32_MAX) return;
    used_ -= std::min(used_, count);
    auto it = std::lower_bound(free_.begin(), free_.end(), base, [](const Range& r, uint32_t b) { return r.base < b; });
    it = free_.insert(it, Range{base, count});
    // Coalesce with the next and the previous range.
    if (it + 1 != free_.end() && it->base + it->count == (it + 1)->base) {
        it->count += (it + 1)->count;
        free_.erase(it + 1);
    }
    if (it != free_.begin() && (it - 1)->base + (it - 1)->count == it->base) {
        (it - 1)->count += it->count;
        free_.erase(it);
    }
}

// ------------------------------------------------------------------------------------------ detail
uint32_t detail_active_count(uint32_t pool, float detail, float scale2) {
    const double f = static_cast<double>(std::clamp(scale2, 0.0f, 1.0f)) * static_cast<double>(std::clamp(detail, 0.0f, 1.0f));
    const double a = std::ceil(f * static_cast<double>(pool));
    return static_cast<uint32_t>(std::clamp(a, 0.0, static_cast<double>(pool)));
}

bool detail_slot_active(uint32_t slot, uint32_t pool, uint32_t active) {
    if (pool == 0) return false;
    return ((0xEDF1ull + static_cast<uint64_t>(slot) * 0xECF3ull) % pool) < active;
}

// ------------------------------------------------------------------------------------------- world
ParticleWorld::ParticleWorld(uint32_t gpu_capacity, uint32_t seed) : rng_(seed) { ranges_.init(gpu_capacity); }

float ParticleWorld::tick_delta(uint32_t ticks) {
    return std::min(kTickSeconds * static_cast<float>(ticks), kMaxUpdateSeconds);
}

ParticleWorld::Handle ParticleWorld::create(SystemPtr def, const Mat3x4& transform) {
    if (!def) return 0;
    uint32_t index;
    if (!free_systems_.empty()) {
        index = free_systems_.back();
        free_systems_.pop_back();
    } else {
        index = static_cast<uint32_t>(systems_.size());
        systems_.emplace_back();
    }
    SystemState& s = systems_[index];
    const uint32_t gen = s.generation + 1;
    s = SystemState{};
    s.generation = gen;
    s.def = std::move(def);
    s.cur = s.prev = transform;
    s.has_transform = true;
    s.alive = true;
    init_emitters(s);
    return (static_cast<uint64_t>(gen) << 32) | index;
}

bool ParticleWorld::valid(Handle h) const { return system(h) != nullptr; }

const SystemState* ParticleWorld::system(Handle h) const {
    const auto index = static_cast<uint32_t>(h & 0xFFFFFFFFu);
    const auto gen = static_cast<uint32_t>(h >> 32);
    if (h == 0 || index >= systems_.size()) return nullptr;
    const SystemState& s = systems_[index];
    return s.alive && s.generation == gen ? &s : nullptr;
}

void ParticleWorld::set_transform(Handle h, const Mat3x4& t, bool teleport) {
    auto* s = const_cast<SystemState*>(system(h));
    if (!s) return;
    s->cur = t;
    if (teleport) s->prev = t;
}
void ParticleWorld::set_visible(Handle h, bool v) {
    if (auto* s = const_cast<SystemState*>(system(h))) s->visible = v;
}
void ParticleWorld::set_fade(Handle h, float f) {
    if (auto* s = const_cast<SystemState*>(system(h))) s->fade = f;
}
void ParticleWorld::set_water(Handle h, bool w) {
    if (auto* s = const_cast<SystemState*>(system(h))) s->water = w;
}
void ParticleWorld::set_scales(Handle h, float emission, float detail) {
    set_scalar(h, SystemScalar::Emission, std::span<const float>(&emission, 1), 0.0f);
    set_scalar(h, SystemScalar::Detail, std::span<const float>(&detail, 1), 0.0f);
}

namespace {
// One critically damped spring step toward `target` over dt (exact solution, omega = spring rate).
void spring_step(float& x, SystemState::Spring& s, float dt) {
    const float w = s.rate, d0 = x - s.target;
    const float k = s.velocity + w * d0;
    const float e = std::exp(-w * dt);
    x = s.target + (d0 + k * dt) * e;
    s.velocity = (s.velocity - w * k * dt) * e;
    if (!std::isfinite(x) || (std::fabs(x - s.target) < 1e-5f && std::fabs(s.velocity) < 1e-4f)) {  // settled: snap
        x = s.target;
        s.velocity = 0.0f;
    }
}
}  // namespace

void ParticleWorld::set_scalar(Handle h, SystemScalar index, std::span<const float> value, float smoothing_seconds) {
    auto* s = const_cast<SystemState*>(system(h));
    if (!s || value.empty()) return;
    const bool snap = !(smoothing_seconds > 0.0f);
    const float rate = 1.0f / (snap ? 0.01f : smoothing_seconds);
    auto aim = [&](SystemState::Spring& sp, float& cur, float v) {
        sp.target = v;
        sp.rate = rate;
        if (snap) {
            cur = v;
            sp.velocity = 0.0f;
        }
    };
    switch (index) {
    case SystemScalar::Detail: {
        aim(s->detail_spring, s->detail_scale, value[0]);
        if (snap)
            for (EmitterState& e : s->emitters)
                if (detail_active_count(e.pool, detail_, s->detail_scale) != e.active) reset_detail(e, s->detail_scale);
        break;
    }
    case SystemScalar::Emission: aim(s->emission_spring, s->emission_scale, value[0]); break;
    case SystemScalar::Immediate:
        s->scalar2[0] = value[0];
        if (value.size() > 1) s->scalar2[1] = value[1];
        break;
    case SystemScalar::Color:
        for (size_t c = 0; c < 4 && c < value.size(); ++c) aim(s->color_spring[c], s->color[c], value[c]);
        break;
    }
}

// B.4 first step of an update: the springs move toward their targets; a detail change re-evaluates the active slots
// (only when the active count of an emitter changes).
void ParticleWorld::smooth_scalars(SystemState& s, float d) {
    if (s.emission_scale != s.emission_spring.target) spring_step(s.emission_scale, s.emission_spring, d);
    for (int c = 0; c < 4; ++c)
        if (s.color[c] != s.color_spring[c].target) spring_step(s.color[c], s.color_spring[c], d);
    if (s.detail_scale != s.detail_spring.target) {
        spring_step(s.detail_scale, s.detail_spring, d);
        for (EmitterState& e : s.emitters)
            if (detail_active_count(e.pool, detail_, s.detail_scale) != e.active) reset_detail(e, s.detail_scale);
    }
}

void ParticleWorld::release(Handle h) {
    auto* s = const_cast<SystemState*>(system(h));
    if (!s) return;
    if (!s->def->leave_particles) {
        free_system(*s);
        return;
    }
    s->released = true;
    for (EmitterState& e : s->emitters)
        for (EmitterState::Schedule& sc : e.schedules) sc.stopped = true;
}

void ParticleWorld::set_detail(float detail) {
    detail = std::clamp(detail, 0.0f, 1.0f);
    if (detail == detail_) return;
    detail_ = detail;
    for (SystemState& s : systems_)
        if (s.alive)
            for (EmitterState& e : s.emitters) reset_detail(e, s.detail_scale);
}

void ParticleWorld::reset_detail(EmitterState& e, float scale2) {
    const EmitterDef& d = *e.def;
    // Spawn-source-3 emitters are exempt unless they are weather (B.4).
    e.active = d.spawn_source == 3 && !d.weather ? e.pool : detail_active_count(e.pool, detail_, scale2);
    e.free.clear();
    std::vector<uint8_t> pending(e.pool, 0);
    for (uint32_t s : e.pending_free) pending[s] = 1;
    for (uint32_t i = e.pool; i-- > 0;)  // back() = lowest slot
        if (e.life[i] < 0.0f && !pending[i] && detail_slot_active(i, e.pool, e.active)) e.free.push_back(i);
}

void ParticleWorld::init_emitters(SystemState& s) {
    const SystemDef& def = *s.def;
    s.emitters.resize(def.emitters.size());
    for (size_t i = 0; i < def.emitters.size(); ++i) {
        const EmitterDef& d = def.emitters[i];
        EmitterState& e = s.emitters[i];
        e.def = &d;
        e.pool = d.pool;
        e.age.assign(e.pool, 0.0f);
        e.life.assign(e.pool, -1.0f);
        if (e.pool) {
            e.base = ranges_.allocate(e.pool);
            if (e.base == UINT32_MAX) {
                ++totals_.pool_exhausted;
            } else if (record_) {
                pending_clears_.push_back({e.base, e.pool});
            }
        }
        reset_detail(e, s.detail_scale);
        if (d.parent < 0) {
            e.schedules.resize(1);
            e.schedules[0].running = true;
        } else {
            const uint32_t parent_pool = def.emitters[static_cast<size_t>(d.parent)].pool;
            e.schedules.resize(parent_pool);
        }
    }
}

void ParticleWorld::free_system(SystemState& s) {
    for (EmitterState& e : s.emitters) {
        if (e.base != UINT32_MAX) ranges_.free(e.base, e.pool);
        e.base = UINT32_MAX;
        totals_.spawned += e.spawned;
        totals_.dropped += e.dropped;
        totals_.died += e.died;
    }
    s.emitters.clear();
    s.alive = false;
    s.def.reset();
    free_systems_.push_back(static_cast<uint32_t>(&s - systems_.data()));
}

bool ParticleWorld::take_slot(EmitterState& e, uint32_t& slot) {
    if (e.base == UINT32_MAX || e.free.empty()) return false;
    slot = e.free.back();
    e.free.pop_back();
    return true;
}

void ParticleWorld::update(float d) {
    if (!(d > 0.0f)) return;
    d = std::min(d, kMaxUpdateSeconds);
    ++totals_.updates;
    UpdatePacket* pkt = nullptr;
    if (record_) {
        packets_.emplace_back();
        pkt = &packets_.back();
        pkt->d = d;
        pkt->clears = std::move(pending_clears_);
        pending_clears_.clear();
    }
    for (SystemState& s : systems_) {
        if (!s.alive) continue;
        // Invisible-system throttle (B.4, STRATEGIC_GAPS §6.2): a system updated before and not drawn since gathers d and
        // steps only once >= 0.1 s gathered, with the gathered d. [port] d left over when the system is drawn again is
        // added to that step (the system clock stays the scene clock).
        float dd = d;
        if (throttle_ && s.updated && !s.drawn) {
            s.throttle_acc += d;
            if (s.throttle_acc < kThrottleSeconds) {
                ++totals_.throttled;
                continue;
            }
            dd = s.throttle_acc;
            s.throttle_acc = 0.0f;
        } else if (s.throttle_acc > 0.0f) {
            dd = d + s.throttle_acc;
            s.throttle_acc = 0.0f;
        }
        dd = std::min(dd, kMaxUpdateSeconds);
        s.drawn = false;
        s.updated = true;
        ++totals_.system_steps;
        step_system(s, dd, pkt);
    }
}

std::vector<UpdatePacket> ParticleWorld::take_packets() {
    std::vector<UpdatePacket> out = std::move(packets_);
    packets_.clear();
    if (!pending_clears_.empty()) {  // ranges allocated since the last update: cleared before anything uses them
        UpdatePacket p;
        p.d = 0.0f;
        p.clears = std::move(pending_clears_);
        pending_clears_.clear();
        out.insert(out.begin(), std::move(p));
    }
    return out;
}

void ParticleWorld::step_system(SystemState& s, float d, UpdatePacket* pkt) {
    smooth_scalars(s, d);
    const Float3 p0 = s.prev.translation(), p1 = s.cur.translation();
    s.velocity = {(p1.x - p0.x) / d, (p1.y - p0.y) / d, (p1.z - p0.z) / d};
    s.time += d;
    // Packet entries: one per emitter with a GPU range.
    std::vector<int32_t> pidx(s.emitters.size(), -1);
    if (pkt) {
        for (size_t i = 0; i < s.emitters.size(); ++i) {
            const EmitterState& e = s.emitters[i];
            if (e.base == UINT32_MAX) continue;
            PacketEmitter pe;
            pe.def = e.def;
            pe.base = e.base;
            pe.pool = e.pool;
            pe.prev = s.prev;
            pe.cur = s.cur;
            pe.velocity = s.velocity;
            pe.d = d;
            if (e.def->parent >= 0) pe.parent_base = static_cast<int32_t>(s.emitters[static_cast<size_t>(e.def->parent)].base);
            pidx[i] = static_cast<int32_t>(pkt->emitters.size());
            pkt->emitters.push_back(pe);
        }
    }
    // A. Ages and deaths (the GPU integration does the same float additions).
    for (size_t i = 0; i < s.emitters.size(); ++i) {
        EmitterState& e = s.emitters[i];
        const EmitterDef& ed = *e.def;
        const int32_t death_child = ed.spawn_on_death, life_child = ed.spawn_during_life;
        for (uint32_t k = 0; k < e.pool; ++k) {
            if (e.life[k] < 0.0f) continue;
            e.age[k] += d;
            if (ed.weather) {  // weather particles wrap their age (B.4)
                if (e.age[k] >= e.life[k]) e.age[k] = std::fmod(e.age[k], e.life[k]);
                continue;
            }
            if (e.age[k] < e.life[k]) continue;
            e.life[k] = -1.0f;
            --e.live;
            ++e.died;
            bool defer = false;
            if (death_child >= 0 && static_cast<size_t>(death_child) < s.emitters.size()) {
                EmitterState& c = s.emitters[static_cast<size_t>(death_child)];
                if (c.def->parent == static_cast<int32_t>(i) && k < c.schedules.size() && !s.released) {
                    c.schedules[k] = EmitterState::Schedule{};
                    c.schedules[k].running = true;
                    c.schedules[k].from_death = true;
                    defer = true;  // the death position stays readable until the child spawned this update
                }
            }
            if (life_child >= 0 && static_cast<size_t>(life_child) < s.emitters.size()) {
                EmitterState& c = s.emitters[static_cast<size_t>(life_child)];
                if (c.def->parent == static_cast<int32_t>(i) && k < c.schedules.size()) c.schedules[k].stopped = true;
            }
            if (detail_slot_active(k, e.pool, e.active)) (defer ? e.pending_free : e.free).push_back(k);
        }
    }
    // B. Spawning, emitters in file order.
    std::vector<uint32_t> started;  // during-life child schedules started this update (begin next update)
    for (size_t i = 0; i < s.emitters.size(); ++i) {
        EmitterState& e = s.emitters[i];
        const EmitterDef& ed = *e.def;
        const int32_t pe = pidx[i];
        auto step = [&](EmitterState::Schedule& sc, int32_t parent_slot) {
            if (sc.stopped || !sc.running) return;
            float dd = d;
            if (sc.age == 0.0f && ed.skip_time > 0.0f) {  // pre-warm: 0.1 s steps until age > skip time
                float pre = 0.0f;
                while (pre <= ed.skip_time) pre += kPrewarmStep;
                dd += pre;
            }
            const float a0 = sc.age, a1 = a0 + dd;
            const bool stop_now = ed.stop_time > 0.0f && a1 > ed.stop_time;
            const double limit = stop_now ? static_cast<double>(ed.stop_time) : static_cast<double>(a1);
            const uint64_t max_events = ed.use_bursts && ed.burst_count > 0 ? ed.burst_count : UINT64_MAX;
            if (ed.weather) {
                // Weather: the pool filled once with random ages (B.4, SPEC 0x3D), wrapping forever.
                const float l = std::max(ed.lifetime, 0.001f);
                uint32_t slot;
                while (take_slot(e, slot)) {
                    e.age[slot] = rng_.uniform() * l;
                    e.life[slot] = l;
                    ++e.live;
                    ++e.spawned;
                    if (pkt && pe >= 0)
                        pkt->spawns.push_back({slot, static_cast<uint32_t>(pe), e.age[slot], l, 1.0f, rng_.next(), -1, 0});
                }
                sc.stopped = true;
                sc.age = a1;
                return;
            }
            const double interval = static_cast<double>(ed.interval);
            bool emitted_zero_interval = false;
            while (sc.events < max_events) {
                double t;
                if (interval > 0.0) {
                    t = static_cast<double>(ed.initial_delay) + interval * static_cast<double>(sc.events);
                } else {  // no interval: the first event at the initial delay, then one per update
                    if (emitted_zero_interval) break;
                    t = sc.events == 0 ? static_cast<double>(ed.initial_delay) : static_cast<double>(a1);
                }
                if (t > limit) break;
                ++sc.events;
                emitted_zero_interval = true;
                const float pre_age = static_cast<float>(static_cast<double>(a1) - t);
                for (uint32_t n = 0; n < ed.per_event; ++n) {
                    if (!(rng_.uniform() < s.emission_scale)) continue;
                    const float life = ed.lifetime * (1.0f - ed.random_lifetime * rng_.uniform());
                    if (pre_age >= life) continue;  // older than its lifetime: skipped
                    uint32_t slot;
                    if (!take_slot(e, slot)) {
                        ++e.dropped;
                        continue;
                    }
                    e.age[slot] = pre_age;
                    e.life[slot] = life;
                    ++e.live;
                    ++e.spawned;
                    const float lerp = std::clamp(1.0f - pre_age / d, 0.0f, 1.0f);
                    if (pkt && pe >= 0)
                        pkt->spawns.push_back({slot, static_cast<uint32_t>(pe), pre_age, life, lerp, rng_.next(), parent_slot,
                                               sc.from_death ? kSpawnFromDeath : 0u});
                    // A during-life child gets one schedule per parent particle, running from the next update.
                    if (ed.spawn_during_life >= 0 && static_cast<size_t>(ed.spawn_during_life) < s.emitters.size()) {
                        EmitterState& c = s.emitters[static_cast<size_t>(ed.spawn_during_life)];
                        if (c.def->parent == static_cast<int32_t>(i) && slot < c.schedules.size()) {
                            c.schedules[slot] = EmitterState::Schedule{};
                            c.schedules[slot].stopped = true;  // armed below, after this update's spawns
                            started.push_back(static_cast<uint32_t>(ed.spawn_during_life));
                            started.push_back(slot);
                        }
                    }
                }
            }
            sc.age = a1;
            if (stop_now || sc.events >= max_events) sc.stopped = true;
            if (s.released) sc.stopped = true;
        };
        if (ed.parent < 0) {
            if (!e.schedules.empty()) step(e.schedules[0], -1);
        } else {
            for (uint32_t k = 0; k < e.schedules.size(); ++k)
                if (e.schedules[k].running) step(e.schedules[k], static_cast<int32_t>(k));
        }
    }
    for (size_t j = 0; j + 1 < started.size(); j += 2) {
        EmitterState::Schedule& sc = s.emitters[started[j]].schedules[started[j + 1]];
        sc.stopped = s.released;
        sc.running = true;
    }
    // C. Deferred frees, done flags, culling trail.
    bool all_done = true;
    for (EmitterState& e : s.emitters) {
        for (uint32_t k : e.pending_free) e.free.push_back(k);
        e.pending_free.clear();
        // A finished on-death child schedule (stopped) with no particles left stops running.
        bool scheduling = false;
        for (EmitterState::Schedule& sc : e.schedules)
            if (sc.running && !sc.stopped) scheduling = true;
        // Linked children are done when their parent is done (it can still start schedules).
        e.done = !scheduling && e.live == 0;
        const Float3 pos = s.cur.translation();
        if (e.trail.empty() || s.time - e.trail.back().time >= 0.1f) e.trail.push_back({s.time, pos});
        else e.trail.back().pos = pos;
        const float keep = e.def->lifetime + d + 0.1f;
        while (e.trail.size() > 1 && e.trail.front().time < s.time - keep) e.trail.pop_front();
    }
    for (size_t i = 0; i < s.emitters.size(); ++i) {
        const EmitterState& e = s.emitters[i];
        bool done = e.done;
        if (e.def->parent >= 0) done = done && s.emitters[static_cast<size_t>(e.def->parent)].done;
        all_done = all_done && done;
    }
    s.prev = s.cur;
    if (all_done) free_system(s);  // every emitter finished: the system kills itself (no linger)
}

Aabb ParticleWorld::bounds(const SystemState& s) const {
    const Float3 c = s.cur.translation();
    Aabb box{{c.x - 0.5f, c.y - 0.5f, c.z - 0.5f}, {c.x + 0.5f, c.y + 0.5f, c.z + 0.5f}};  // empty: unit box
    for (const EmitterState& e : s.emitters) {
        if (e.live == 0) continue;
        const float r = e.def->reach;
        auto add = [&](const Float3& p) {
            box.min = {std::min(box.min.x, p.x - r), std::min(box.min.y, p.y - r), std::min(box.min.z, p.z - r)};
            box.max = {std::max(box.max.x, p.x + r), std::max(box.max.y, p.y + r), std::max(box.max.z, p.z + r)};
        };
        add(c);
        if (!e.def->object_space)
            for (const EmitterState::Sample& sm : e.trail) add(sm.pos);
    }
    return box;
}

WorldStats ParticleWorld::stats() const {
    WorldStats st = totals_;
    for (const SystemState& s : systems_) {
        if (!s.alive) continue;
        ++st.systems;
        for (const EmitterState& e : s.emitters) {
            ++st.emitters;
            st.live_particles += e.live;
            st.spawned += e.spawned;
            st.dropped += e.dropped;
            st.died += e.died;
        }
    }
    return st;
}

}  // namespace alamo::render::particles

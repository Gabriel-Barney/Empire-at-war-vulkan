#include "alamo/render/particles/particle_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>

#include "alamo/assets/model.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/text.hpp"
#include "alamo/formats/alo.hpp"
#include "alamo/formats/particle.hpp"

namespace alamo::render::particles {

namespace {

Shape to_shape(const particle::SpawnShape& s) {
    Shape o;
    o.kind = static_cast<uint32_t>(s.kind);
    o.box_min[0] = s.box_min.x;
    o.box_min[1] = s.box_min.y;
    o.box_min[2] = s.box_min.z;
    o.box_max[0] = s.box_max.x;
    o.box_max[1] = s.box_max.y;
    o.box_max[2] = s.box_max.z;
    o.cube_half = s.cube_half_size;
    o.sphere_radius = s.sphere_radius;
    o.sphere_surface = s.sphere_surface;
    o.cylinder_radius = s.cylinder_radius;
    o.cylinder_surface = s.cylinder_surface;
    o.cylinder_height = s.cylinder_height;
    o.point[0] = s.point_value.x;
    o.point[1] = s.point_value.y;
    o.point[2] = s.point_value.z;
    return o;
}

// Track -> 32 samples at life fractions i / 31: the header start value at 0 %, the keys (time in percent) in
// between, the end value at 100 %. Mode 2 holds the value of the last point at or before the sample (step); modes
// 0 and 1 interpolate linearly (the meaning of 1 is not traced, B.2 [L]; linear is kept for it).
TrackTable resample(const particle::Track& t, bool bytes) {
    struct P {
        float time, value;
    };
    std::vector<P> pts;
    pts.reserve(t.keys.size() + 2);
    pts.push_back({0.0f, t.start});
    // Key records are (value, time as a 0..1 life fraction); byte tracks store the value as a u32 0..255, which
    // alamo_formats returns as a float (SPEC 3.3, corrected after the survey of all 1,490 emitters).
    for (const particle::TrackKey& k : t.keys) {
        const float value = bytes ? std::min(k.value, 255.0f) : k.value;
        pts.push_back({std::clamp(k.time, 0.0f, 1.0f) * 100.0f, value});
    }
    pts.push_back({100.0f, t.end});
    std::stable_sort(pts.begin(), pts.end(), [](const P& a, const P& b) { return a.time < b.time; });
    TrackTable out;
    out.interpolation = t.interpolation;
    const float scale = bytes ? 1.0f / 255.0f : 1.0f;
    for (uint32_t i = 0; i < kTrackSamples; ++i) {
        const float time = 100.0f * static_cast<float>(i) / static_cast<float>(kTrackSamples - 1);
        size_t j = 0;
        while (j + 1 < pts.size() && pts[j + 1].time <= time) ++j;
        float v = pts[j].value;
        if (t.interpolation != 2 && j + 1 < pts.size()) {
            const float span = pts[j + 1].time - pts[j].time;
            const float f = span > 0.0f ? (time - pts[j].time) / span : 0.0f;
            v = pts[j].value + (pts[j + 1].value - pts[j].value) * f;
        }
        out.v[i] = v * scale;
    }
    return out;
}

float length3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

void derive(EmitterDef& e) {
    if (e.use_bursts) {
        e.interval = e.burst_delay;
        e.per_event = e.particles_per_burst;
    } else {
        e.interval = e.particles_per_second > 0 ? 1.0f / static_cast<float>(e.particles_per_second) : 0.0f;
        e.per_event = 1;
    }
    e.base_pool = emitter_pool(e);
    // Reach: furthest a particle can get from its spawn point (shape + speed x life + accel x life^2 / 2), padded by
    // the largest particle size and 1.5 x tail (B.5 "Culling").
    const float l = std::max(e.lifetime, 0.0f);
    const float speed = e.velocity.extent() + std::fabs(e.outward_speed);
    float accel = length3(e.acceleration) + std::fabs(e.gravity) + std::fabs(e.outward_acceleration);
    if (e.weather) accel = 0.0f;
    float size = 0.0f;
    for (float v : e.tracks[kScale].v) size = std::max(size, std::fabs(v));
    e.reach = e.position.extent() + speed * l + 0.5f * accel * l * l + size + 1.5f * std::fabs(e.tail_size);
}

}  // namespace

float Shape::extent() const {
    switch (kind) {
        case 1: {
            float m = 0;
            for (int i = 0; i < 3; ++i) m += std::max(box_min[i] * box_min[i], box_max[i] * box_max[i]);
            return std::sqrt(m);
        }
        case 2: return std::fabs(cube_half) * 1.7320508f;
        case 3: return std::fabs(sphere_radius);
        case 4: return std::sqrt(cylinder_radius * cylinder_radius + cylinder_height * cylinder_height);
        default: return std::sqrt(point[0] * point[0] + point[1] * point[1] + point[2] * point[2]);
    }
}

bool EmitterDef::sorted() const {
    return depth_sort && (blend_mode == 0 || blend_mode == 2 || blend_mode == 5 || blend_mode == 11);
}

bool SystemDef::has_heat() const {
    return std::any_of(emitters.begin(), emitters.end(), [](const EmitterDef& e) { return e.heat; });
}
bool SystemDef::has_non_heat() const {
    return std::any_of(emitters.begin(), emitters.end(), [](const EmitterDef& e) { return !e.heat; });
}

std::string texture_base_name(std::string_view name) {
    const size_t p = name.find_last_of("\\/");
    return ascii_upper(p == std::string_view::npos ? name : name.substr(p + 1));
}

float sample_track(const TrackTable& t, float f) {
    const float x = std::clamp(f, 0.0f, 1.0f) * static_cast<float>(kTrackSamples - 1);
    const auto i = static_cast<uint32_t>(x);
    if (i >= kTrackSamples - 1) return t.v[kTrackSamples - 1];
    if (t.interpolation == 2) return t.v[i];
    const float w = x - static_cast<float>(i);
    return t.v[i] + (t.v[i + 1] - t.v[i]) * w;
}

uint32_t emitter_pool(const EmitterDef& e) {
    const float life = e.weather ? 1.0f : std::max(e.lifetime, 0.0f);
    double rate = 0.0;
    if (e.use_bursts) {
        rate = e.burst_delay > 0.0f ? static_cast<double>(e.particles_per_burst) / static_cast<double>(e.burst_delay)
                                    : static_cast<double>(e.particles_per_burst);
    } else {
        rate = static_cast<double>(e.particles_per_second);
    }
    const double interval = e.use_bursts ? static_cast<double>(e.burst_delay) : (rate > 0.0 ? 1.0 / rate : 1e30);
    double pool = 0.0;
    if (interval <= static_cast<double>(life)) {
        pool = std::ceil(rate * static_cast<double>(life));
        // A burst always fits one whole event (burst delay 0 = one burst per update).
        if (e.use_bursts) pool = std::max(pool, static_cast<double>(e.particles_per_burst));
    } else {
        pool = e.use_bursts ? static_cast<double>(e.particles_per_burst) : std::ceil(rate);
    }
    pool = std::clamp(pool, 0.0, static_cast<double>(kPoolCap));
    return static_cast<uint32_t>(pool);
}

void finalize_system(SystemDef& s) {
    for (size_t i = 0; i < s.emitters.size(); ++i) {
        EmitterDef& e = s.emitters[i];
        e.index = static_cast<uint32_t>(i);
        e.parent = -1;
        e.parent_on_death = false;
        e.life.kind = 1;
        e.life.box_min[0] = e.lifetime * (1.0f - e.random_lifetime);
        e.life.box_max[0] = e.lifetime;
        derive(e);
    }
    // Links: a child's pool is multiplied by its parent's pool (B.2, 0x163da0); a child has one parent (the first link
    // in file order, during-life before on-death). Links outside the system or to the emitter itself are ignored.
    const auto n_em = static_cast<int32_t>(s.emitters.size());
    for (int32_t i = 0; i < n_em; ++i) {
        const EmitterDef& p = s.emitters[static_cast<size_t>(i)];
        for (int pass = 0; pass < 2; ++pass) {
            const int32_t c = pass == 0 ? p.spawn_during_life : p.spawn_on_death;
            if (c < 0 || c >= n_em || c == i) continue;
            EmitterDef& ch = s.emitters[static_cast<size_t>(c)];
            if (ch.parent >= 0) continue;
            ch.parent = i;
            ch.parent_on_death = pass == 1;
        }
    }
    for (EmitterDef& e : s.emitters) e.pool = e.base_pool;
    // Chains resolve in at most n rounds.
    for (int32_t round = 0; round < n_em; ++round)
        for (EmitterDef& e : s.emitters) {
            if (e.parent < 0) continue;
            const uint64_t pp = s.emitters[static_cast<size_t>(e.parent)].pool;
            e.pool = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(e.base_pool) * std::max<uint64_t>(pp, 1), kPoolCap));
        }
}

bool is_particle_file(std::span<const uint8_t> b) {
    return b.size() >= 8 && b[0] == 0x00 && b[1] == 0x09 && b[2] == 0x00 && b[3] == 0x00;
}

bool load_system(std::span<const uint8_t> bytes, std::string_view name, SystemDef& out, std::string* error) {
    if (!is_particle_file(bytes)) {
        if (error) *error = "not a particle system (first chunk is not 0x900)";
        return false;
    }
    try {
        const alo::AloFile f = alo::parse(bytes);
        if (f.kind != alo::AloFile::Kind::ParticleSystem) {
            if (error) *error = "not a particle system";
            return false;
        }
        const particle::ParticleSystem& ps = f.particles;
        SystemDef s;
        std::string n = ascii_upper(name);
        const size_t slash = n.find_last_of("\\/");
        if (slash != std::string::npos) n = n.substr(slash + 1);
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ALO") == 0) n.resize(n.size() - 4);
        s.name = n;
        s.leave_particles = ps.leave_particles != 0;
        s.emitters.reserve(ps.emitters.size());
        for (size_t i = 0; i < ps.emitters.size(); ++i) {
            const particle::Emitter& pe = ps.emitters[i];
            const particle::EmitterDesc& d = pe.desc;
            EmitterDef e;
            e.name = pe.name;
            e.color_texture = pe.color_texture.empty() ? std::string() : texture_base_name(pe.color_texture);
            e.bump_texture = pe.bump_texture.empty() ? std::string() : texture_base_name(pe.bump_texture);
            e.index = static_cast<uint32_t>(i);
            e.blend_mode = d.blend_mode;
            e.geometry = d.geometry;
            e.texture_cells = std::max<uint32_t>(d.texture_cells, 1u);
            e.depth_sort = d.depth_sort;
            e.heat = d.is_heat_particle;
            e.has_tail = d.has_tail;
            e.tail_size = d.tail_size;
            e.enabled = true;  // the loader forces 0x15 to 1 (SPEC 3.1)
            e.use_bursts = d.use_bursts;
            e.particles_per_second = d.particles_per_second;
            e.burst_delay = d.burst_delay;
            e.particles_per_burst = d.particles_per_burst;
            e.burst_count = d.burst_count;
            e.initial_delay = d.initial_delay;
            e.stop_time = d.stop_time;
            e.skip_time = d.skip_time;
            e.spawn_source = d.spawn_source;
            e.lifetime = d.lifetime;
            e.random_lifetime = d.random_lifetime;
            e.object_space = d.object_space;
            e.acceleration[0] = d.acceleration.x;
            e.acceleration[1] = d.acceleration.y;
            e.acceleration[2] = d.acceleration.z;
            e.object_space_acceleration = d.object_space_acceleration;
            e.gravity = d.gravity;
            e.outward_speed = d.outward_speed;
            e.outward_acceleration = d.outward_acceleration;
            e.inherit_velocity = d.inherit_velocity;
            e.inherit_velocity_factor = d.inherit_velocity_factor;
            e.affected_by_wind = d.affected_by_wind;
            e.ground_behavior = d.ground_behavior;
            e.random_color[0] = d.random_color.x;
            e.random_color[1] = d.random_color.y;
            e.random_color[2] = d.random_color.z;
            e.random_color[3] = d.random_color.w;
            e.random_color_uniform = d.random_color_uniform;
            e.random_initial_rotation = d.random_initial_rotation;
            e.random_rotation_speed = std::clamp(d.random_rotation_speed, 0.0f, 1.0f);
            e.random_rotation_direction = d.random_rotation_direction;
            e.rotation_fixed_at_spawn = d.rotation_fixed_at_spawn;
            e.weather = d.is_weather_particle;
            e.weather_cube_size = d.weather_cube_size;
            e.weather_fade_start = d.weather_fade_start;
            e.weather_fade_length = d.weather_fade_length;
            e.weather_camera_distance = d.weather_camera_distance;
            if (pe.velocity_shape()) e.velocity = to_shape(*pe.velocity_shape());
            if (pe.position_shape()) e.position = to_shape(*pe.position_shape());
            // Lifetime shape recomputed from 0x0F / 0x13 (SPEC 3.2): a box [L(1-r), L] on x.
            e.life.kind = 1;
            e.life.box_min[0] = e.lifetime * (1.0f - e.random_lifetime);
            e.life.box_max[0] = e.lifetime;
            // Weather cube: the position shape is a cube of half the edge (SPEC 3.1 0x3E).
            if (e.weather) {
                e.position.kind = 2;
                e.position.cube_half = 0.5f * e.weather_cube_size;
            }
            for (uint32_t t = 0; t < kTrackCount; ++t) {
                if (t < pe.tracks.size()) e.tracks[t] = resample(pe.tracks[t], t <= kAlpha);
                else e.tracks[t].v.fill(t <= kAlpha || t == kScale ? 1.0f : 0.0f);
            }
            e.spawn_on_death = pe.spawn_on_death == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(pe.spawn_on_death);
            e.spawn_during_life = pe.spawn_during_life == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(pe.spawn_during_life);
            s.emitters.push_back(std::move(e));
        }
        finalize_system(s);
        out = std::move(s);
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

ProxyName parse_proxy_name(std::string_view name) {
    ProxyName p;
    const std::string u = ascii_upper(name);
    auto index_after = [&](const char* tag, size_t& at) {
        at = u.find(tag);
        if (at == std::string::npos) return -1;
        size_t i = at + 4;
        int v = 0;
        bool any = false;
        while (i < u.size() && std::isdigit(static_cast<unsigned char>(u[i])) && v < 100000) {
            v = v * 10 + (u[i] - '0');
            ++i;
            any = true;
        }
        return any ? v : -1;
    };
    size_t at_alt = std::string::npos, at_lod = std::string::npos;
    p.alt = index_after("_ALT", at_alt);
    p.lod = index_after("_LOD", at_lod);
    p.base = u.substr(0, std::min(at_alt, at_lod));
    return p;
}

SystemPtr ParticleLibrary::get(std::string_view name) {
    std::string key = ascii_upper(name);
    if (key.size() > 4 && key.compare(key.size() - 4, 4, ".ALO") == 0) key.resize(key.size() - 4);
    {
        std::lock_guard<std::mutex> l(mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
    }
    SystemPtr result;
    int outcome = 0;  // 0 missing, 1 loaded, 2 not particle, 3 malformed
    std::vector<uint8_t> bytes;
    if (files_ && !key.empty() && files_->read(assets::model_path(key), bytes)) {
        if (!is_particle_file(bytes)) {
            outcome = 2;
        } else {
            auto s = std::make_shared<SystemDef>();
            if (load_system(bytes, key, *s)) {
                result = std::move(s);
                outcome = 1;
            } else {
                outcome = 3;
            }
        }
    }
    std::lock_guard<std::mutex> l(mutex_);
    auto [it, inserted] = cache_.try_emplace(key, result);
    if (inserted) {
        if (outcome == 0) ++stats_.missing;
        else if (outcome == 1) ++stats_.loaded;
        else if (outcome == 2) ++stats_.not_particle;
        else ++stats_.malformed;
    }
    return it->second;
}

void ParticleLibrary::add(SystemPtr system) {
    if (!system) return;
    std::lock_guard<std::mutex> l(mutex_);
    cache_[ascii_upper(system->name)] = std::move(system);
}

ParticleLibrary::Stats ParticleLibrary::stats() const {
    std::lock_guard<std::mutex> l(mutex_);
    return stats_;
}

}  // namespace alamo::render::particles

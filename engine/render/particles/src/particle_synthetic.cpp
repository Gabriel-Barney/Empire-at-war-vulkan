#include "alamo/render/particles/particle_synthetic.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace alamo::render::particles {

namespace {

void fill_track(TrackTable& t, float a, float b) {
    t.interpolation = 0;
    for (uint32_t i = 0; i < kTrackSamples; ++i)
        t.v[i] = a + (b - a) * static_cast<float>(i) / static_cast<float>(kTrackSamples - 1);
}

}  // namespace

EmitterDef basic_emitter(uint32_t pps, float lifetime, float size) {
    EmitterDef e;
    e.name = "EMITTER";
    e.blend_mode = 1;
    e.particles_per_second = pps;
    e.lifetime = lifetime;
    e.life.kind = 1;
    e.life.box_min[0] = lifetime;
    e.life.box_max[0] = lifetime;
    fill_track(e.tracks[kRed], 1, 1);
    fill_track(e.tracks[kGreen], 1, 1);
    fill_track(e.tracks[kBlue], 1, 1);
    fill_track(e.tracks[kAlpha], 1, 0);
    fill_track(e.tracks[kScale], size, size);
    fill_track(e.tracks[kTexture], 0, 0);
    fill_track(e.tracks[kRotation], 0, 0);
    e.tracks[kTexture].interpolation = 2;
    return e;
}

std::vector<SystemPtr> synthetic_systems() {
    std::vector<SystemPtr> out;
    {
        auto s = std::make_shared<SystemDef>();
        s->name = "SYN_FIRE";
        EmitterDef e = basic_emitter(120, 1.2f, 9.0f);
        e.name = "FLAMES";
        e.position.kind = 3;
        e.position.sphere_radius = 4.0f;
        e.velocity.kind = 3;
        e.velocity.sphere_radius = 6.0f;
        e.gravity = -18.0f;
        fill_track(e.tracks[kGreen], 0.7f, 0.15f);
        fill_track(e.tracks[kBlue], 0.25f, 0.0f);
        fill_track(e.tracks[kScale], 10.0f, 3.0f);
        e.random_initial_rotation = 3.14159f;
        s->emitters.push_back(e);
        finalize_system(*s);
        out.push_back(s);
    }
    {
        auto s = std::make_shared<SystemDef>();
        s->name = "SYN_SMOKE";
        EmitterDef e = basic_emitter(40, 3.0f, 10.0f);
        e.name = "SMOKE";
        e.blend_mode = 2;
        e.depth_sort = true;
        e.position.kind = 4;
        e.position.cylinder_radius = 5.0f;
        e.velocity.kind = 3;
        e.velocity.sphere_radius = 3.0f;
        e.gravity = -6.0f;
        fill_track(e.tracks[kRed], 0.55f, 0.35f);
        fill_track(e.tracks[kGreen], 0.55f, 0.35f);
        fill_track(e.tracks[kBlue], 0.6f, 0.4f);
        fill_track(e.tracks[kAlpha], 0.8f, 0.0f);
        fill_track(e.tracks[kScale], 6.0f, 22.0f);
        fill_track(e.tracks[kRotation], 0.0f, 0.5f);
        s->emitters.push_back(e);
        finalize_system(*s);
        out.push_back(s);
    }
    {
        auto s = std::make_shared<SystemDef>();
        s->name = "SYN_SPARKS";
        EmitterDef e = basic_emitter(0, 1.5f, 1.5f);
        e.name = "SPARKS";
        e.use_bursts = true;
        e.particles_per_burst = 60;
        e.burst_delay = 0.75f;
        e.random_lifetime = 0.5f;
        e.velocity.kind = 3;
        e.velocity.sphere_radius = 1.0f;
        e.velocity.sphere_surface = true;
        e.outward_speed = 0.0f;
        e.velocity.sphere_radius = 45.0f;
        e.gravity = 40.0f;
        e.has_tail = true;
        e.tail_size = 3.0f;
        fill_track(e.tracks[kBlue], 0.4f, 0.1f);
        fill_track(e.tracks[kGreen], 0.9f, 0.5f);
        s->emitters.push_back(e);
        finalize_system(*s);
        out.push_back(s);
    }
    {
        auto s = std::make_shared<SystemDef>();
        s->name = "SYN_HEAT";
        EmitterDef e = basic_emitter(30, 1.0f, 12.0f);
        e.name = "HEAT";
        e.blend_mode = 2;
        e.heat = true;
        e.velocity.kind = 0;
        e.velocity.point[2] = 10.0f;
        s->emitters.push_back(e);
        finalize_system(*s);
        out.push_back(s);
    }
    {
        auto s = std::make_shared<SystemDef>();
        s->name = "SYN_CHAIN";
        s->leave_particles = true;
        EmitterDef parent = basic_emitter(0, 0.6f, 4.0f);
        parent.name = "SHELLS";
        parent.use_bursts = true;
        parent.particles_per_burst = 8;
        parent.burst_delay = 1.0f;
        parent.velocity.kind = 3;
        parent.velocity.sphere_radius = 40.0f;
        parent.velocity.sphere_surface = true;
        parent.spawn_on_death = 1;
        parent.spawn_during_life = 2;
        EmitterDef pop = basic_emitter(0, 0.5f, 3.0f);
        pop.name = "POP";
        pop.use_bursts = true;
        pop.particles_per_burst = 12;
        pop.burst_delay = 1.0f;
        pop.burst_count = 1;
        pop.velocity.kind = 3;
        pop.velocity.sphere_radius = 15.0f;
        fill_track(pop.tracks[kBlue], 0.2f, 0.2f);
        EmitterDef trail = basic_emitter(40, 0.4f, 2.0f);
        trail.name = "TRAIL";
        fill_track(trail.tracks[kRed], 0.4f, 0.4f);
        s->emitters = {parent, pop, trail};
        finalize_system(*s);
        out.push_back(s);
    }
    return out;
}

}  // namespace alamo::render::particles

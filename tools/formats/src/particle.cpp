#include "alamo/formats/particle.hpp"

#include <cstring>

namespace alamo::particle {

namespace {

using C = Confidence;
using T = FieldType;

constexpr EmitterFieldInfo kFields[] = {
    {0x04, "blend_mode", T::U32, C::High, "index of the engine Prim effect (see BlendMode)"},
    {0x05, "geometry", T::U32, C::Medium, "0 = triangle (3 vertices), 1 = quad (4 vertices)"},
    {0x06, "tool_field_06", T::U32, C::Low, "ignored by the game loader (0 or 40)"},
    {0x07, "use_bursts", T::Bool, C::High, "emit in bursts instead of a continuous rate"},
    {0x08, "object_space", T::Bool, C::Medium, "particles are simulated in emitter/object space"},
    {0x09, "outward_speed", T::F32, C::Medium, "initial radial speed away from the emitter centre"},
    {0x0A, "acceleration", T::Vec3, C::High, "constant acceleration"},
    {0x0B, "outward_acceleration", T::F32, C::Medium, "radial acceleration (negative = inward)"},
    {0x0C, "gravity", T::F32, C::High, "gravity, subtracted from acceleration.z"},
    {0x0F, "lifetime", T::F32, C::High, "particle lifetime in seconds"},
    {0x10, "texture_cells", T::U32, C::High, "texture atlas cell count (square: columns = sqrt)"},
    {0x11, "random_color_legacy", T::F32, C::Medium, "legacy: uniform RGB colour variance (replaced by 0x2C)"},
    {0x12, "random_initial_rotation", T::F32, C::Medium, "initial rotation randomised in [-r, r]"},
    {0x13, "random_lifetime", T::F32, C::High, "lifetime variance fraction: life in [L(1-r), L]"},
    {0x14, "index", T::U32, C::High, "emitter index within the system (verified = file order)"},
    {0x15, "enabled", T::Bool, C::Medium, "emitter enabled (always 1; the loader forces 1)"},
    {0x17, "random_rotation_speed", T::F32, C::High, "rotation speed variance fraction (clamped 0..1)"},
    {0x23, "random_rotation_direction", T::Bool, C::High, "50% of particles rotate in the opposite direction"},
    {0x24, "initial_delay", T::F32, C::High, "seconds before the emitter starts"},
    {0x25, "burst_delay", T::F32, C::High, "seconds between bursts"},
    {0x26, "particles_per_burst", T::U32, C::High, "particles per burst"},
    {0x27, "burst_count", T::U32, C::High, "number of bursts (0 = unlimited)"},
    {0x28, "inherit_velocity_factor", T::F32, C::High, "fraction of emitter velocity inherited"},
    {0x2A, "particles_per_second", T::U32, C::High, "continuous emission rate"},
    {0x2B, "depth_sort", T::Bool, C::Medium, "depth-sort particles (alpha-type blend modes only)"},
    {0x2C, "random_color", T::Vec4, C::High, "RGBA colour variance fractions"},
    {0x2D, "random_color_uniform", T::Bool, C::High, "use one random value for R, G and B"},
    {0x2E, "alternate_vertex_layout", T::Bool, C::Low, "selects an alternate vertex/UV table"},
    {0x2F, "ground_behavior", T::U32, C::Medium, "0 none; 1/2 particles collide with terrain"},
    {0x30, "bounciness", T::F32, C::Medium, "ground collision response factor"},
    {0x31, "affected_by_wind", T::Bool, C::High, "adds global wind to particle velocity"},
    {0x32, "stop_time", T::F32, C::High, "stop emitting after N seconds (0 = never)"},
    {0x33, "skip_time", T::F32, C::High, "pre-simulate N seconds when started"},
    {0x34, "spawn_source", T::U32, C::Medium, "0 spawn shapes; 1/2 parent mesh surface; 3 mesh vertices"},
    {0x35, "object_space_acceleration", T::Bool, C::High, "acceleration rotated by emitter orientation"},
    {0x3B, "is_heat_particle", T::Bool, C::Medium, "rendered in the heat-distortion pass"},
    {0x3C, "mesh_normal_offset", T::F32, C::Medium, "offset along the mesh normal for spawn_source 1/2"},
    {0x3D, "is_weather_particle", T::Bool, C::High, "particles fill a camera-relative cube (rain/snow)"},
    {0x3E, "weather_cube_size", T::F32, C::High, "edge length of the weather cube"},
    {0x3F, "weather_fade_start", T::F32, C::Medium, "distance where weather particles start fading"},
    {0x40, "weather_fade_length", T::F32, C::Medium, "fade distance range"},
    {0x41, "has_tail", T::Bool, C::High, "stretch particles along their velocity"},
    {0x42, "tail_size", T::F32, C::High, "tail stretch factor"},
    {0x43, "inherit_velocity", T::Bool, C::High, "inherit emitter velocity (see 0x28)"},
    {0x44, "affected_by_wind_disturbance", T::Bool, C::Medium, "pushed by local wind-disturbance spheres"},
    {0x46, "render_flag_46", T::Bool, C::Low, "flag forwarded to the particle draw call"},
    {0x47, "weather_camera_distance", T::F32, C::Medium, "weather cube offset along the camera direction"},
    {0x48, "rotation_fixed_at_spawn", T::Bool, C::Medium, "rotation evaluated once at spawn"},
    {0x49, "tool_field_49", T::F32, C::Low, "ignored by the game loader (often garbage)"},
};

std::size_t type_size(FieldType t) {
    switch (t) {
        case T::Bool: return 1;
        case T::U32: return 4;
        case T::F32: return 4;
        case T::Vec3: return 12;
        case T::Vec4: return 16;
    }
    return 0;
}

SpawnShape decode_shape(Bytes rec, std::size_t off) {
    if (rec.size() != 64) throw FormatError("spawn shape record is not 64 bytes", off);
    SpawnShape s;
    std::memcpy(s.raw.data(), rec.data(), 64);
    ByteReader r(rec, off);
    s.kind = static_cast<ShapeKind>(r.u32());
    s.box_min = r.vec3();
    s.box_max = r.vec3();
    s.cube_half_size = r.f32();
    s.sphere_radius = r.f32();
    s.sphere_surface = r.u8() != 0;
    r.skip(3);
    s.cylinder_radius = r.f32();
    s.cylinder_surface = r.u8() != 0;
    r.skip(3);
    s.cylinder_height = r.f32();
    s.point_value = r.vec3();
    return s;
}

std::vector<Track> parse_tracks(const Chunk& c) {
    std::vector<Track> tracks;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        if (ch->id == 0x0) {
            Track t;
            auto mc = ch->minis();
            while (auto m = mc.next()) {
                if (m->id == 2 || m->id == 3) {
                    float v;
                    if (m->data.size() == 1) {
                        t.byte_values = true;
                        v = float(m->data[0]);
                    } else {
                        v = m->as_float();
                    }
                    (m->id == 2 ? t.start : t.end) = v;
                } else if (m->id == 4) {
                    t.interpolation = m->as_u32();
                }
            }
            tracks.push_back(std::move(t));
        } else if (ch->id == 0x1) {
            if (tracks.empty()) throw FormatError("track keys before track header", ch->offset);
            auto mc = ch->minis();
            while (auto m = mc.next()) {
                if (m->id != 5 || m->data.size() != 8) throw FormatError("unexpected track key mini-chunk", m->offset);
                auto r = m->reader();
                TrackKey k;  // SPEC 3.3: (value, time 0..1); byte tracks: u32 value
                if (tracks.back().byte_values) k.value = static_cast<float>(r.u32());
                else k.value = r.f32();
                k.time = r.f32();
                tracks.back().keys.push_back(k);
            }
        } else {
            throw FormatError("unexpected chunk in particle track list", ch->offset);
        }
    }
    return tracks;
}

Emitter parse_emitter(const Chunk& c) {
    Emitter e;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        switch (ch->id) {
            case 0x00: {
                // Legacy layout: spawn shapes as minis 0x19/0x1A/0x1B (not observed in shipped data).
                if (ch->has_children) break;
                auto mc = ch->minis();
                std::array<std::optional<SpawnShape>, 3> legacy;
                while (auto m = mc.next())
                    if (m->id >= 0x19 && m->id <= 0x1B) legacy[m->id - 0x19] = decode_shape(m->data, m->offset + 2);
                if (legacy[0] && legacy[1] && legacy[2]) e.shapes = {*legacy[0], *legacy[1], *legacy[2]};
                break;
            }
            case 0x02:
                e.properties.assign(ch->data.begin(), ch->data.end());
                e.desc = parse_emitter_desc(ch->data, ch->data_offset());
                break;
            case 0x03: e.color_texture = ch->as_string(); break;
            case 0x16: e.name = ch->as_string(); break;
            case 0x45: e.bump_texture = ch->as_string(); break;
            case 0x01: e.tracks = parse_tracks(*ch); break;
            case 0x29: {
                auto gc = ch->children();
                while (auto g = gc.next()) {
                    auto dc = g->children();
                    while (auto d = dc.next()) {
                        if (d->id != 0x1101) throw FormatError("unexpected spawn shape chunk", d->offset);
                        e.shapes.push_back(decode_shape(d->data, d->data_offset()));
                    }
                }
                break;
            }
            case 0x36: {
                auto mc = ch->minis();
                while (auto m = mc.next()) {
                    if (m->id == 0x37) e.spawn_on_death = m->as_u32();
                    else if (m->id == 0x39) e.spawn_during_life = m->as_u32();
                }
                break;
            }
            default: break;  // unknown emitter chunk: reported by the validator's generic walk
        }
    }
    return e;
}

}  // namespace

const char* to_string(Confidence c) {
    switch (c) {
        case C::High: return "high";
        case C::Medium: return "medium";
        case C::Low: return "low";
    }
    return "?";
}

const char* blend_mode_effect(std::uint32_t m) {
    static const char* n[] = {"Engine\\PrimOpaque", "Engine\\PrimAdditive", "Engine\\PrimAlpha",
                              "Engine\\PrimModulate", "Engine\\PrimDepthSpriteAdditive",
                              "Engine\\PrimDepthSpriteAlpha", "Engine\\PrimDepthSpriteModulate",
                              "Engine\\PrimDiffuseAlpha", "Engine\\StencilDarken", "Engine\\StencilDarkenFinalBlur",
                              "Engine\\PrimHeat", "Engine\\PrimParticleBumpAlpha", "Engine\\PrimDecalBumpAlpha",
                              "Engine\\PrimAlphaScanlines"};
    return m < 14 ? n[m] : "unknown";
}

const char* track_name(std::size_t i) {
    static const char* names[] = {"red", "green", "blue", "alpha", "scale", "texture_index", "rotation_speed"};
    return i < TrackCount ? names[i] : "?";
}

std::span<const EmitterFieldInfo> emitter_fields() { return kFields; }

const EmitterFieldInfo* emitter_field(std::uint8_t id) {
    for (const auto& f : kFields)
        if (f.id == id) return &f;
    return nullptr;
}

EmitterDesc parse_emitter_desc(Bytes block, std::size_t base) {
    EmitterDesc d;
    MiniChunkCursor mc(block, base);
    while (auto m = mc.next()) {
        const auto* info = emitter_field(m->id);
        if (!info) {
            d.unknown.push_back({m->id, std::vector<std::uint8_t>(m->data.begin(), m->data.end())});
            continue;
        }
        if (m->data.size() != type_size(info->type))
            throw FormatError(std::string("emitter property ") + info->name + " has size " +
                                  std::to_string(m->data.size()),
                              m->offset);
        d.present.set(m->id);
        auto b = [&] { return m->data[0] != 0; };
        auto u = [&] { return m->as_u32(); };
        auto f = [&] { return m->as_float(); };
        switch (m->id) {
            case 0x04: d.blend_mode = u(); break;
            case 0x05: d.geometry = u(); break;
            case 0x06: d.tool_field_06 = u(); break;
            case 0x07: d.use_bursts = b(); break;
            case 0x08: d.object_space = b(); break;
            case 0x09: d.outward_speed = f(); break;
            case 0x0A: d.acceleration = m->as_vec3(); break;
            case 0x0B: d.outward_acceleration = f(); break;
            case 0x0C: d.gravity = f(); break;
            case 0x0F: d.lifetime = f(); break;
            case 0x10: d.texture_cells = u(); break;
            case 0x11: { const float v = f(); d.random_color = {v, v, v, d.random_color.w}; break; }
            case 0x12: d.random_initial_rotation = f(); break;
            case 0x13: d.random_lifetime = f(); break;
            case 0x14: d.index = u(); break;
            case 0x15: d.enabled = b(); break;
            case 0x17: d.random_rotation_speed = f(); break;
            case 0x23: d.random_rotation_direction = b(); break;
            case 0x24: d.initial_delay = f(); break;
            case 0x25: d.burst_delay = f(); break;
            case 0x26: d.particles_per_burst = u(); break;
            case 0x27: d.burst_count = u(); break;
            case 0x28: d.inherit_velocity_factor = f(); break;
            case 0x2A: d.particles_per_second = u(); break;
            case 0x2B: d.depth_sort = b(); break;
            case 0x2C: d.random_color = m->as_vec4(); break;
            case 0x2D: d.random_color_uniform = b(); break;
            case 0x2E: d.alternate_vertex_layout = b(); break;
            case 0x2F: d.ground_behavior = u(); break;
            case 0x30: d.bounciness = f(); break;
            case 0x31: d.affected_by_wind = b(); break;
            case 0x32: d.stop_time = f(); break;
            case 0x33: d.skip_time = f(); break;
            case 0x34: d.spawn_source = u(); break;
            case 0x35: d.object_space_acceleration = b(); break;
            case 0x3B: d.is_heat_particle = b(); break;
            case 0x3C: d.mesh_normal_offset = f(); break;
            case 0x3D: d.is_weather_particle = b(); break;
            case 0x3E: d.weather_cube_size = f(); break;
            case 0x3F: d.weather_fade_start = f(); break;
            case 0x40: d.weather_fade_length = f(); break;
            case 0x41: d.has_tail = b(); break;
            case 0x42: d.tail_size = f(); break;
            case 0x43: d.inherit_velocity = b(); break;
            case 0x44: d.affected_by_wind_disturbance = b(); break;
            case 0x46: d.render_flag_46 = b(); break;
            case 0x47: d.weather_camera_distance = f(); break;
            case 0x48: d.rotation_fixed_at_spawn = b(); break;
            case 0x49: d.tool_field_49 = f(); break;
            default: break;
        }
    }
    return d;
}

ParticleSystem parse_system(const Chunk& c) {
    if (c.id != 0x900 || !c.has_children) throw FormatError("not a particle system chunk", c.offset);
    ParticleSystem ps;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        switch (ch->id) {
            case 0x0: ps.name = ch->as_string(); break;
            case 0x1: ps.editor_id = ch->reader().u32(); break;
            case 0x2: ps.leave_particles = ch->reader().u8(); break;
            case 0x800: {
                auto ec = ch->children();
                while (auto e = ec.next()) {
                    if (e->id != 0x700) throw FormatError("unexpected chunk in emitter list", e->offset);
                    ps.emitters.push_back(parse_emitter(*e));
                }
                break;
            }
            default: break;
        }
    }
    // Dangling emitter links (index >= emitter count) occur in modded data; the runtime ignores
    // them (lookup fails), so they are kept as-is and reported by the validator.
    return ps;
}

}  // namespace alamo::particle

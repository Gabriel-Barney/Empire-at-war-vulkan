// .ted reader (TERRAIN_WATER_POST.md 1, 5). See ted.hpp for the contract.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "alamo/core/ascii.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/terrain/ted.hpp"

namespace alamo::terrain {

namespace {

constexpr float kDegToRad = 0.017453292519943295f;

struct Ctx {
    std::string* err;
    bool fail(const std::string& why) {
        if (err) *err = why;
        return false;
    }
};

void set_bool(const MiniView& m, bool& b) {
    uint32_t v = 0;
    if (mini_u32(m, v)) b = (v & 0xFF) != 0;
}

}  // namespace

// ------------------------------------------------------------------ Environment
// Determinism note: the std::cos/sin helpers in this file (light direction, wind / cloud vectors,
// material texture matrices) are not correctly rounded and may differ between CRTs; they feed
// rendering (lighting, clouds, weather particles, UVs) only. Sim code must read the raw stored
// fields (yaw, elevation, wind_direction_deg, wind_speed), never these derived vectors.
std::array<float, 3> Environment::light_direction(int i) const {
    const int k = i < 0 ? 0 : (i > 2 ? 2 : i);
    const float a = yaw[k], e = elevation[k];
    return {std::cos(e) * std::sin(a), -std::cos(e) * std::cos(a), std::sin(e)};
}
std::array<float, 3> Environment::wind_vector() const {
    const float w = wind_direction_deg * kDegToRad;
    return {wind_speed * std::cos(w), wind_speed * std::sin(w), 0.0f};
}
std::array<float, 3> Environment::cloud_velocity() const {
    const float w = wind_direction_deg * kDegToRad;
    return {cloud_speed * std::cos(w), cloud_speed * std::sin(w), 0.0f};
}

bool decode_environment(std::span<const uint8_t> leaf, Environment& e) {
    std::vector<MiniView> minis;
    if (!read_minis(leaf, minis)) return false;
    e.raw = copy_minis(minis);
    float fog_a = e.fog_start, fog_b = e.fog_end;
    for (const MiniView& m : minis) {
        switch (m.id) {
        case 0x00: case 0x01: case 0x02: mini_f32x3(m, e.light_color[m.id]); break;
        case 0x03: mini_f32x3(m, e.sun_specular); break;
        case 0x04: mini_f32x3(m, e.ambient); break;
        case 0x05: case 0x06: case 0x07: mini_f32(m, e.intensity[m.id - 0x05]); break;
        case 0x08: case 0x09: case 0x0A: mini_f32(m, e.yaw[m.id - 0x08]); break;
        case 0x0B: case 0x0C: case 0x0D: mini_f32(m, e.elevation[m.id - 0x0B]); break;
        case 0x0F: mini_f32x3(m, e.fog_color); break;
        case 0x10: set_bool(m, e.fog_enabled); break;
        case 0x11: mini_f32(m, fog_a); break;
        case 0x12: mini_f32(m, fog_b); break;
        case 0x13: {
            uint32_t v = 0;
            if (mini_u32(m, v)) e.field_13 = static_cast<int32_t>(v) < 0 ? 0u : v;
            break;
        }
        case 0x14: e.name = mini_string(m); break;
        case 0x15: mini_f32x3(m, e.clear_color); break;
        case 0x17: mini_f32x3(m, e.shadow_color); break;
        case 0x19: e.skydome[0] = mini_string(m); break;
        case 0x1A: e.skydome[1] = mini_string(m); break;
        case 0x1B: mini_f32(m, e.skydome_scale[0]); break;
        case 0x1C: mini_f32(m, e.skydome_scale[1]); break;
        case 0x1D: mini_f32(m, e.skydome_angle_deg[0][0]); break;
        case 0x1F: mini_f32(m, e.skydome_angle_deg[0][1]); break;
        case 0x1E: mini_f32(m, e.skydome_angle_deg[1][0]); break;
        case 0x20: mini_f32(m, e.skydome_angle_deg[1][1]); break;
        case 0x21: mini_f32(m, e.lightning_intensity); break;
        case 0x23: mini_f32(m, e.bloom_strength); break;
        case 0x24: mini_f32(m, e.bloom_cutoff); break;
        case 0x28: mini_f32(m, e.bloom_size); break;
        case 0x29: e.weather_effect = mini_string(m); break;
        case 0x2B: mini_f32(m, e.wind_direction_deg); break;
        case 0x2C: mini_f32(m, e.wind_speed); break;
        case 0x2D: mini_f32(m, e.cloud_scale); break;
        case 0x2E: mini_f32(m, e.cloud_speed); break;
        case 0x2F: e.cloud_texture = mini_string(m); break;
        default: break;
        }
    }
    e.fog_start = std::min(fog_a, fog_b);  // the loader stores min / max of the pair
    e.fog_end = std::max(fog_a, fog_b);
    for (int i = 0; i < 2; ++i) {
        if (e.skydome[i].empty()) {  // 5.2: scale/angles reset to (1, 0, 0) when the name is empty
            e.skydome_scale[i] = 1.0f;
            e.skydome_angle_deg[i][0] = e.skydome_angle_deg[i][1] = 0.0f;
        }
    }
    return true;
}

// ------------------------------------------------------------------ TerrainMaterial
std::array<float, 4> TerrainMaterial::tex_u() const {
    const float inv = tile_size != 0.0f ? 1.0f / tile_size : 0.0f;
    // U0 = (inv, 0, 0): rotation about X leaves it unchanged; rotation about Z mixes x and y.
    const float cz = std::cos(rotation_z), sz = std::sin(rotation_z);
    return {inv * cz, inv * sz, 0.0f, 0.0f};
}
std::array<float, 4> TerrainMaterial::tex_v() const {
    const float inv = tile_size != 0.0f ? 1.0f / tile_size : 0.0f;
    const float cx = std::cos(rotation_x), sx = std::sin(rotation_x);
    const float cz = std::cos(rotation_z), sz = std::sin(rotation_z);
    // V0 = (0, inv, 0) -> about X: (0, inv cx, inv sx) -> about Z: (-y sz, y cz, z).
    const float y = inv * cx, z = inv * sx;
    return {-y * sz, y * cz, z, 0.0f};
}
std::string TerrainMaterial::effective_normal_texture() const {
    return normal_texture.empty() ? std::string("Generic_Flat_B.tga") : normal_texture;
}

bool decode_terrain_material(std::span<const uint8_t> leaf, TerrainMaterial& t) {
    std::vector<MiniView> minis;
    if (!read_minis(leaf, minis)) return false;
    t.present = true;
    t.raw = copy_minis(minis);
    for (const MiniView& m : minis) {
        switch (m.id) {
        case 0x06: mini_f32(m, t.specular_power); break;
        case 0x07: mini_f32(m, t.tile_size); break;
        case 0x08: mini_f32(m, t.rotation_z); break;
        case 0x09: mini_f32(m, t.rotation_x); break;
        case 0x0A: mini_f32x3(m, t.diffuse_color); break;
        case 0x0B: mini_f32x3(m, t.specular_color); break;
        case 0x0C: t.diffuse_texture = mini_string(m); break;
        case 0x0D: t.flag_0d = static_cast<uint8_t>(mini_u32_or(m, 0) & 0xFF); break;
        case 0x12: set_bool(m, t.full_coverage); break;
        case 0x13: t.normal_texture = mini_string(m); break;
        case 0x14: mini_u32(m, t.surface_type); break;
        case 0x23: set_bool(m, t.not_rendered); break;
        default: break;
        }
    }
    return true;
}

// ------------------------------------------------------------------ Terrain accessors
int16_t Terrain::raw_height(int64_t x, int64_t y) const {
    if (width == 0 || height == 0 || heights.size() < static_cast<size_t>(width) * height) return 0;
    x = std::clamp<int64_t>(x, 0, width - 1);
    y = std::clamp<int64_t>(y, 0, height - 1);
    return heights[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
}
uint8_t Terrain::material_at(int64_t x, int64_t y) const {
    if (width == 0 || height == 0 || material.size() < static_cast<size_t>(width) * height) return 0;
    x = std::clamp<int64_t>(x, 0, width - 1);
    y = std::clamp<int64_t>(y, 0, height - 1);
    return material[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
}
float Terrain::sample_height(float wx, float wy) const {
    if (width < 2 || height < 2 || !std::isfinite(wx) || !std::isfinite(wy)) return 0.0f;
    const float maxx = static_cast<float>(width - 1), maxy = static_cast<float>(height - 1);
    const float gx = std::clamp(wx / kVertexSpacing, 0.0f, maxx);
    const float gy = std::clamp(wy / kVertexSpacing, 0.0f, maxy);
    int64_t ix = static_cast<int64_t>(gx), iy = static_cast<int64_t>(gy);
    if (ix >= static_cast<int64_t>(width) - 1) ix = width - 2;
    if (iy >= static_cast<int64_t>(height) - 1) iy = height - 2;
    const float fx = gx - static_cast<float>(ix), fy = gy - static_cast<float>(iy);
    const float h00 = height_at(ix, iy), h10 = height_at(ix + 1, iy);
    const float h01 = height_at(ix, iy + 1), h11 = height_at(ix + 1, iy + 1);
    // Triangles (0,0)(1,0)(1,1) when fx >= fy, else (0,0)(1,1)(0,1).
    if (fx >= fy) return h00 + fx * (h10 - h00) + fy * (h11 - h10);
    return h00 + fy * (h01 - h00) + fx * (h11 - h01);
}

float CameraBlock::f32_or(uint8_t id, float fallback) const {
    float v = fallback;
    return raw_f32(raw, id, v) ? v : fallback;
}

uint32_t MapData::passability_at(uint32_t x, uint32_t y) const {
    const uint32_t w = terrain.width, h = terrain.height;
    if (x >= w || y >= h || passability.size() < static_cast<size_t>(w) * h) return 0xFFFFFFFFu;
    return passability[static_cast<size_t>(y) * w + x];
}

bool MapData::playable_box(float (&min_xy)[2], float (&max_xy)[2]) const {
    if (is_space()) {
        // space.present is false only for a hand-built MapData: use the loader defaults then.
        const SpaceMapDesc d = space.present ? space : SpaceMapDesc{};
        min_xy[0] = d.playable_min[0];
        min_xy[1] = d.playable_min[1];
        max_xy[0] = d.playable_max[0];
        max_xy[1] = d.playable_max[1];
        return true;
    }
    if (playable.has_box) {
        min_xy[0] = playable.box_min[0];
        min_xy[1] = playable.box_min[1];
        max_xy[0] = playable.box_max[0];
        max_xy[1] = playable.box_max[1];
        return true;
    }
    if (terrain.width < 2 || terrain.height < 2) return false;
    min_xy[0] = min_xy[1] = 0.0f;  // [L] fallback: the whole grid
    max_xy[0] = static_cast<float>(terrain.width - 1) * kVertexSpacing;
    max_xy[1] = static_cast<float>(terrain.height - 1) * kVertexSpacing;
    return true;
}

// ------------------------------------------------------------------ parsing
namespace {

uint32_t round_grid(uint32_t n) {
    if (n < 2) return 0;
    const uint32_t patches = (n - 1 + kPatchQuads - 1) / kPatchQuads;
    return patches * kPatchQuads + 1;
}

bool parse_map_info(std::span<const uint8_t> leaf, MapInfo& mi, Ctx& c) {
    std::vector<MiniView> minis;
    if (!read_minis(leaf, minis)) return c.fail("map info chunk 0: malformed mini list");
    mi.present = true;
    mi.raw = copy_minis(minis);
    for (const MiniView& m : minis) {
        switch (m.id) {
        case 0x00: mini_u32(m, mi.format); break;
        case 0x01: mini_u32(m, mi.map_type); break;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: mini_u32(m, mi.counts[m.id - 0x02]); break;
        case 0x08: mi.name_key = mini_utf16(m); break;
        case 0x09: mi.planet_name = mini_utf16(m); break;
        case 0x0A: mi.text_0a = mini_utf16(m); break;
        case 0x0B: mi.flag_0b = static_cast<uint8_t>(mini_u32_or(m, 0) & 0xFF); break;
        case 0x10: mini_f32(m, mi.world_width); break;
        case 0x11: mini_f32(m, mi.world_height); break;
        case 0x12: mi.flag_12 = static_cast<uint8_t>(mini_u32_or(m, 0) & 0xFF); break;
        default: break;
        }
    }
    return true;
}

bool parse_start_positions(std::span<const uint8_t> leaf, std::vector<Vec2f>& out, Ctx& c) {
    if (leaf.size() < 4) return c.fail("start positions chunk 3: too short");
    uint32_t n = 0;
    std::memcpy(&n, leaf.data(), 4);
    if (n > (leaf.size() - 4) / 8) return c.fail("start positions chunk 3: count exceeds payload");
    out.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        float xy[2];
        std::memcpy(xy, leaf.data() + 4 + 8 * static_cast<size_t>(i), 8);
        out[i] = {std::isfinite(xy[0]) ? xy[0] : 0.0f, std::isfinite(xy[1]) ? xy[1] : 0.0f};
    }
    return true;
}

bool parse_preview(std::span<const uint8_t> node, std::vector<PreviewObject>& out, Ctx& c) {
    std::vector<ChunkView> kids;
    if (!read_chunks(node, kids)) return c.fail("preview chunk 2: malformed");
    std::vector<MiniView> minis;
    for (const ChunkView& k : kids) {
        if (k.id != 0xB || k.has_children) continue;
        if (!read_minis(k.payload, minis)) return c.fail("preview object: malformed mini list");
        PreviewObject p;
        for (const MiniView& m : minis) {
            if (m.id == 0x0C) mini_u32(m, p.object_id);
            else if (m.id == 0x0D) mini_u32(m, p.type_crc);
            else if (m.id == 0x0E) mini_f32(m, p.position.x);
            else if (m.id == 0x0F) mini_f32(m, p.position.y);
        }
        out.push_back(p);
    }
    return true;
}

bool parse_environments(std::span<const uint8_t> node, EnvironmentSet& es, Ctx& c) {
    std::vector<ChunkView> kids, sub;
    std::vector<MiniView> minis;
    if (!read_chunks(node, kids)) return c.fail("environment chunk 0x100: malformed");
    for (const ChunkView& k : kids) {
        if (k.id == 4 && k.has_children) {
            if (!read_chunks(k.payload, sub)) return c.fail("environment list 0x100/4: malformed");
            for (const ChunkView& r : sub) {
                if (r.id == 5) break;  // end marker
                if (r.id != 6 || r.has_children) continue;
                Environment e;
                if (!decode_environment(r.payload, e)) return c.fail("malformed TEE record");
                if (e.name.empty()) e.name = "Environment " + std::to_string(es.environments.size() + 1);
                es.environments.push_back(std::move(e));
            }
        } else if (k.id == 8 && !k.has_children) {
            if (!read_minis(k.payload, minis)) return c.fail("environment index 0x100/8: malformed");
            for (const MiniView& m : minis) {
                if (m.id == 0x25) mini_u32(m, es.start_index);
                if (m.id == 0x2A) mini_u32(m, es.field_2a);
            }
        } else if (k.id == 2 && k.has_children) {
            if (!read_chunks(k.payload, sub)) return c.fail("scene fog 0x100/2: malformed");
            for (const ChunkView& f : sub) {
                if (f.id != 0 || f.has_children) continue;
                if (!read_minis(f.payload, minis)) return c.fail("scene fog 0x100/2/0: malformed");
                es.scene_fog_present = true;
                for (const MiniView& m : minis) {
                    if (m.id == 0x00) mini_f32x3(m, es.scene_fog_color);
                    if (m.id == 0x01) set_bool(m, es.scene_fog_enabled);
                    if (m.id == 0x02) mini_f32(m, es.scene_fog_start);
                    if (m.id == 0x03) mini_f32(m, es.scene_fog_end);
                }
            }
        } else if (k.id == 0 || k.id == 1) {
            es.legacy_format = true;
        }
    }
    return true;
}

bool parse_terrain_header(std::span<const uint8_t> leaf, TerrainHeader& h, Ctx& c) {
    std::vector<MiniView> minis;
    if (!read_minis(leaf, minis)) return c.fail("terrain header 0x101/0: malformed");
    h.raw = copy_minis(minis);
    for (const MiniView& m : minis) {
        switch (m.id) {
        case 0x00: mini_u32(m, h.width); break;
        case 0x01: mini_u32(m, h.height); break;
        case 0x04: mini_u32(m, h.vertex_count); break;
        case 0x05: mini_u32(m, h.material_slots); break;
        case 0x0E: h.cloud_texture = mini_string(m); break;
        case 0x0F: mini_f32(m, h.cloud_scale); break;
        case 0x10: mini_f32x3(m, h.cloud_velocity); break;
        case 0x11: set_bool(m, h.use_vertex_brightness); break;
        case 0x15: mini_f32(m, h.water_height); break;
        case 0x16:
            if (mini_u32(m, h.water_type_raw))
                h.water_type = h.water_type_raw <= 3 ? static_cast<WaterType>(h.water_type_raw) : WaterType::None;
            break;
        case 0x17: mini_f32(m, h.reflection_fraction); break;
        case 0x18: mini_f32(m, h.reflection_distortion); break;
        case 0x19: mini_f32(m, h.refraction_distortion); break;
        case 0x1A: mini_f32x3(m, h.water_color); break;
        case 0x1B: mini_f32x3(m, h.unused_1b); break;
        case 0x1D: h.water_texture[0] = mini_string(m); break;
        case 0x1E: h.water_texture[1] = mini_string(m); break;
        case 0x1F: h.water_texture[2] = mini_string(m); break;
        case 0x20: mini_f32(m, h.water_alpha); break;
        case 0x21: mini_f32(m, h.shore_fade_depth); break;
        case 0x22: mini_f32(m, h.shore_min_alpha); break;
        default: break;
        }
    }
    return true;
}

bool parse_scroll(std::span<const uint8_t> node, WaterScroll& s, Ctx& c) {
    std::vector<ChunkView> kids;
    std::vector<MiniView> minis;
    if (!read_chunks(node, kids)) return c.fail("water scroll record: malformed");
    for (const ChunkView& k : kids) {
        if (k.id != 0x0AA45BCDu || k.has_children) continue;
        if (!read_minis(k.payload, minis)) return c.fail("water scroll record minis: malformed");
        s.present = true;
        for (const MiniView& m : minis) {
            if (m.id == 0x00) mini_f32(m, s.tile_size);
            else if (m.id == 0x01) mini_f32(m, s.angle);
            else if (m.id == 0x02) mini_f32(m, s.speed);
            else if (m.id == 0x03) mini_f32(m, s.direction);
        }
    }
    return true;
}

// ---- chunk 6: decals (DECORATIONS.md 4.1). Each child 0x10 is one mini list.
bool parse_decals(std::span<const uint8_t> node, std::vector<TerrainDecal>& out, Ctx& c) {
    std::vector<ChunkView> kids;
    std::vector<MiniView> minis;
    if (!read_chunks(node, kids)) return c.fail("terrain decals 0x101/6: malformed");
    out.reserve(kids.size());  // bounded by the payload (>= 8 bytes per child)
    for (const ChunkView& k : kids) {
        if (k.id != 0x10 || k.has_children) continue;
        if (!read_minis(k.payload, minis)) return c.fail("terrain decal 0x101/6/10: malformed mini list");
        TerrainDecal d;
        d.raw = copy_minis(minis);
        for (const MiniView& m : minis) {
            switch (m.id) {
            case 0x00: mini_u32(m, d.field_00); break;
            case 0x01: set_bool(m, d.permanent); break;
            case 0x02: mini_u32(m, d.render_mode); break;
            case 0x03: mini_f32x3(m, d.position); break;
            case 0x04: mini_f32(m, d.rotation); break;
            case 0x05:
                if (m.payload.size() >= 8) {
                    float wh[2];
                    std::memcpy(wh, m.payload.data(), 8);
                    if (std::isfinite(wh[0]) && std::isfinite(wh[1])) {
                        d.width = wh[0];
                        d.height = wh[1];
                    }
                }
                break;
            case 0x06: mini_f32(m, d.intensity); break;
            case 0x07: mini_u32(m, d.slot_column); break;
            case 0x08: mini_u32(m, d.slot_row); break;
            case 0x09:
                if (m.payload.size() >= 8) {  // legacy double form
                    double v = 0;
                    std::memcpy(&v, m.payload.data(), 8);
                    if (std::isfinite(v) && std::fabs(v) < 1e30) d.fade_time = static_cast<float>(v);
                }
                break;
            case 0x0A: mini_f32(m, d.fade_time); break;
            case 0x0B: mini_u32(m, d.slot_scale); break;
            default: break;
            }
        }
        out.push_back(std::move(d));
    }
    return true;
}

// ---- chunk 9: tracks (DECORATIONS.md 5.1). Each child 0x100 holds 0x101 minis, 0x102 points,
// 0x103 texture name, 0x104 widths, 0x105 alpha profile.
bool parse_tracks(std::span<const uint8_t> node, std::vector<TerrainTrack>& out, Ctx& c) {
    std::vector<ChunkView> kids, sub;
    std::vector<MiniView> minis;
    if (!read_chunks(node, kids)) return c.fail("terrain tracks 0x101/9: malformed");
    out.reserve(kids.size());
    for (const ChunkView& k : kids) {
        if (k.id != 0x100 || !k.has_children) continue;
        if (!read_chunks(k.payload, sub)) return c.fail("terrain track 0x101/9/100: malformed");
        TerrainTrack t;
        for (const ChunkView& s : sub) {
            if (s.has_children) continue;
            if (s.id == 0x103) {  // zero-terminated name, not a mini list
                size_t n = 0;
                while (n < s.payload.size() && s.payload[n] != 0) ++n;
                t.texture.assign(reinterpret_cast<const char*>(s.payload.data()), n);
                continue;
            }
            if (s.id < 0x101 || s.id > 0x105) continue;
            if (!read_minis(s.payload, minis)) return c.fail("terrain track child: malformed mini list");
            switch (s.id) {
            case 0x101:
                t.raw = copy_minis(minis);
                for (const MiniView& m : minis) {
                    switch (m.id) {
                    case 0x00: mini_f32(m, t.field_00); break;
                    case 0x01: mini_u32(m, t.flags); break;
                    case 0x02: mini_u32(m, t.sample_spacing); break;
                    case 0x03: mini_f32(m, t.u_speed); break;
                    case 0x04: mini_f32(m, t.v_speed); break;
                    case 0x05: mini_f32(m, t.opacity); break;
                    case 0x06: mini_u32(m, t.stored_point_count); break;
                    case 0x0A: mini_f32(m, t.first_row_alpha); break;
                    case 0x0B: mini_f32(m, t.last_row_alpha); break;
                    case 0x0C: mini_f32(m, t.height_offset); break;
                    case 0x0D: mini_f32(m, t.repeat_length); break;
                    case 0x0E: set_bool(m, t.clamp_u); break;
                    case 0x0F: set_bool(m, t.clamp_v); break;
                    case 0x10: mini_u32(m, t.height_mode); break;
                    default: break;
                    }
                }
                break;
            case 0x102:
                t.points.reserve(minis.size());  // bounded by the leaf size
                for (const MiniView& m : minis) {
                    float p[3] = {0, 0, 0};
                    if (m.id == 0x07 && mini_f32x3(m, p)) t.points.push_back({p[0], p[1], p[2]});
                }
                break;
            case 0x104:
                for (const MiniView& m : minis) {
                    float w = 0;
                    if (m.id == 0x08 && mini_f32(m, w)) t.widths.push_back(w);
                }
                break;
            case 0x105:
                t.profile.clear();
                t.profile_present = true;
                for (const MiniView& m : minis) {
                    float a = 0;
                    if (m.id == 0x09 && mini_f32(m, a)) t.profile.push_back(a);
                }
                break;
            default: break;
            }
        }
        out.push_back(std::move(t));
    }
    return true;
}

void ensure_grid(Terrain& t) {
    if (!t.heights.empty() || t.width == 0) return;
    const size_t n = static_cast<size_t>(t.width) * t.height;
    t.heights.assign(n, 0);
    t.material.assign(n, 0);
    t.brightness.assign(n, 0xFF);
}

void load_vertices(Terrain& t, std::span<const uint8_t> data, bool legacy) {
    ensure_grid(t);
    if (t.width == 0) return;
    // File layout is row-major over the header width (1.3). Vertices beyond the data or outside
    // the (rounded) grid keep their defaults.
    const uint32_t fw = t.header.width, fh = t.header.height;
    const size_t count = data.size() / 4;
    for (size_t i = 0; i < count; ++i) {
        const size_t fx = fw ? i % fw : 0, fy = fw ? i / fw : 0;
        if (fx >= t.width || fy >= t.height || fy >= fh) continue;
        const size_t dst = fy * t.width + fx;
        int16_t h = 0;
        std::memcpy(&h, data.data() + 4 * i, 2);
        t.heights[dst] = h;
        t.material[dst] = data[4 * i + 2];
        t.brightness[dst] = legacy ? uint8_t{0xFF} : data[4 * i + 3];
    }
    t.legacy_vertices = legacy;
}

bool parse_terrain(std::span<const uint8_t> node, Terrain& t, Ctx& c) {
    std::vector<ChunkView> kids, sub;
    if (!read_chunks(node, kids)) return c.fail("terrain chunk 0x101: malformed");
    t.present = true;
    // Header first (it sizes the grid), wherever it is.
    for (const ChunkView& k : kids)
        if (k.id == 0 && !k.has_children && !parse_terrain_header(k.payload, t.header, c)) return false;
    const TerrainHeader& h = t.header;
    if (h.width < 2 || h.height < 2) return c.fail("terrain header: grid smaller than 2x2");
    if (h.width > kMaxGridVerts || h.height > kMaxGridVerts) return c.fail("terrain header: grid larger than 1985 vertices");
    // Allocation is driven by the payload, never by the header alone: the vertex data must hold
    // every header vertex before the grid is allocated (a 48-byte file must not allocate 30 MB).
    const uint64_t need = static_cast<uint64_t>(h.width) * h.height * 4;
    const ChunkView* vdata = nullptr;
    for (const ChunkView& k : kids)
        if (k.id == 5 && !vdata) vdata = &k;
    if (!vdata)
        for (const ChunkView& k : kids)
            if (k.id == 1 && !k.has_children && !vdata) vdata = &k;
    if (!vdata) return c.fail("terrain has no vertex data (0x101/5)");
    if (vdata->payload.size() < need) return c.fail("terrain vertex data shorter than header width x height x 4");
    t.width = round_grid(h.width);
    t.height = round_grid(h.height);
    for (const ChunkView& k : kids) {
        switch (k.id) {
        case 5:
            if (k.has_children) return c.fail("terrain vertex data 0x101/5 is not a leaf");
            if (k.payload.size() % 4 != 0) return c.fail("terrain vertex data size is not a multiple of 4");
            load_vertices(t, k.payload, false);
            break;
        case 1:
            if (!k.has_children && k.payload.size() % 4 == 0) load_vertices(t, k.payload, true);
            break;
        case 2: {
            if (!k.has_children) break;
            if (!read_chunks(k.payload, sub)) return c.fail("terrain material table 0x101/2: malformed");
            uint32_t slot = 0;
            for (const ChunkView& r : sub) {
                if (r.id != 3 || r.has_children) continue;
                if (slot >= kMaterialSlots) break;
                TerrainMaterial m;
                if (!decode_terrain_material(r.payload, m)) return c.fail("malformed TEM material record");
                t.materials[slot++] = std::move(m);
            }
            break;
        }
        case 6:  // an empty leaf in most maps
            if (k.has_children && !parse_decals(k.payload, t.decals, c)) return false;
            break;
        case 9:
            if (k.has_children && !parse_tracks(k.payload, t.tracks, c)) return false;
            break;
        case 7:
            if (k.has_children && !parse_scroll(k.payload, t.water_scroll[0], c)) return false;
            break;
        case 8:
            if (k.has_children && !parse_scroll(k.payload, t.water_scroll[1], c)) return false;
            break;
        default: break;
        }
    }
    ensure_grid(t);  // no-op: load_vertices allocated the (payload-verified) grid
    return true;
}

bool parse_object(std::span<const uint8_t> node, ObjectPlacement& o, Ctx& c) {
    std::vector<ChunkView> kids, sub, sub2;
    std::vector<MiniView> minis;
    if (!read_chunks(node, kids)) return c.fail("object 0x44C: malformed");
    for (const ChunkView& k : kids) {
        if (k.id == 0x454 && !k.has_children) {
            if (!read_minis(k.payload, minis)) return c.fail("object 0x454: malformed");
            for (const MiniView& m : minis)
                if (m.id <= 1) mini_u32(m, o.list_id[m.id]);
        } else if (k.id == 0x459 && k.has_children) {
            if (!read_chunks(k.payload, sub)) return c.fail("object 0x459: malformed");
            for (const ChunkView& s : sub) {
                if (s.id == 0x4B0 && !s.has_children) {
                    if (!read_minis(s.payload, minis)) return c.fail("object placement 0x4B0: malformed");
                    o.placement_raw = copy_minis(minis);
                    for (const MiniView& m : minis) {
                        switch (m.id) {
                        case 0x00: mini_u32(m, o.object_id); break;
                        case 0x01: mini_u32(m, o.type_crc); break;
                        case 0x02: mini_u32(m, o.owner); break;
                        case 0x03: mini_f32(m, o.health_fraction); break;
                        case 0x04: mini_f32x3(m, o.position); break;
                        case 0x05: mini_f32x3(m, o.rotation_deg); break;
                        case 0x12: mini_f32x3(m, o.rotation_copy); break;
                        case 0x15: o.has_secondary = mini_f32x3(m, o.secondary); break;
                        default: break;
                        }
                    }
                } else if (s.id == 0x4BA && s.has_children) {
                    if (!read_chunks(s.payload, sub2)) return c.fail("object attachments 0x4BA: malformed");
                    for (const ChunkView& a : sub2) {
                        if (a.has_children) continue;
                        ++o.attachment_slots;
                        if (!read_minis(a.payload, minis)) return c.fail("object attachment: malformed");
                        for (const MiniView& m : minis) {
                            if (m.id != 0x01) continue;
                            std::string n = mini_string(m);
                            if (!n.empty() && n != "NONE") o.attachments.push_back(std::move(n));
                        }
                    }
                }
            }
        } else if (k.id == 0x476 && k.has_children) {
            if (!read_chunks(k.payload, sub)) return c.fail("object owner 0x476: malformed");
            for (const ChunkView& s : sub) {
                if (s.id != 1 || s.has_children) continue;
                if (!read_minis(s.payload, minis)) return c.fail("object owner 0x476/1: malformed");
                o.owner_raw = copy_minis(minis);
            }
        } else if (k.id == 0x482 && k.has_children) {  // hint name (TERRAIN_WATER_POST 1.6)
            if (!read_chunks(k.payload, sub)) return c.fail("object hint 0x482: malformed");
            for (const ChunkView& s : sub) {
                if (s.id != 0 || s.has_children) continue;
                if (!read_minis(s.payload, minis)) return c.fail("object hint 0x482/0: malformed");
                for (const MiniView& m : minis)
                    if (m.id == 0x01) o.hint = mini_string(m);
            }
        }
    }
    return true;
}

bool parse_objects(std::span<const uint8_t> node, std::vector<ObjectPlacement>& out, Ctx& c) {
    std::vector<ChunkView> kids, sub;
    if (!read_chunks(node, kids)) return c.fail("object list 0x102: malformed");
    for (const ChunkView& k : kids) {
        if (k.id != 1 || !k.has_children) continue;
        if (!read_chunks(k.payload, sub)) return c.fail("object list 0x102/1: malformed");
        out.reserve(out.size() + sub.size());
        for (const ChunkView& o : sub) {
            if (o.id != 0x44C || !o.has_children) continue;
            ObjectPlacement p;
            if (!parse_object(o.payload, p, c)) return false;
            out.push_back(std::move(p));
        }
    }
    return true;
}

bool parse_zones(std::span<const uint8_t> node, std::vector<AiZone>& out, Ctx& c) {
    std::vector<ChunkView> kids, sub, sub2;
    std::vector<MiniView> minis;
    if (!read_chunks(node, kids)) return c.fail("AI zones 0x10B: malformed");
    for (const ChunkView& k : kids) {
        if (k.id != 0 || !k.has_children) continue;
        if (!read_chunks(k.payload, sub)) return c.fail("AI zone: malformed");
        AiZone z;
        for (const ChunkView& s : sub) {
            if (s.id == 0 && !s.has_children) {
                if (!read_minis(s.payload, minis)) return c.fail("AI zone header: malformed");
                z.raw = copy_minis(minis);
                for (const MiniView& m : minis) {
                    if (m.id == 0x01) z.name = mini_string(m);
                    else if (m.id == 0x02) mini_u32(m, z.id);
                    else if (m.id == 0x03) mini_u32(m, z.stored_cells);
                    else if (m.id == 0x05) mini_f32x3(m, z.center);
                }
            } else if ((s.id == 0xE || s.id == 0x7) && s.has_children) {
                if (!read_chunks(s.payload, sub2)) return c.fail("AI zone list: malformed");
                for (const ChunkView& e : sub2) {
                    if (e.id != 0 || e.has_children) continue;
                    if (!read_minis(e.payload, minis)) return c.fail("AI zone entry: malformed");
                    uint32_t a = 0, b = 0;
                    for (const MiniView& m : minis) {
                        if (m.id == 0x01) mini_u32(m, a);
                        else if (m.id == 0x02) mini_u32(m, b);
                    }
                    if (s.id == 0xE) z.links.push_back({a, b});
                    else z.cells.push_back({static_cast<uint16_t>(a & 0xFFFF), static_cast<uint16_t>(a >> 16), static_cast<uint8_t>(b & 0xFF)});
                }
            }
        }
        out.push_back(std::move(z));
    }
    return true;
}

bool parse_minis_block(std::span<const uint8_t> leaf, std::vector<RawMini>& raw, Ctx& c, const char* what) {
    std::vector<MiniView> minis;
    if (!read_minis(leaf, minis)) return c.fail(std::string(what) + ": malformed mini list");
    raw = copy_minis(minis);
    return true;
}

// SPACE_AND_OPEN.md A.1: 04/05 playable box (default +-5000), 07/08 camera box (default the
// playable box shrunk by 400 in X/Y), 00/01 pitch/yaw, 02 distance, 03 look-at, 06 FOV.
void decode_space(const PlayableArea& p, SpaceMapDesc& s) {
    s.present = true;
    float a[3], b[3];
    if (raw_f32x3(p.raw, 0x04, a) && raw_f32x3(p.raw, 0x05, b)) {
        std::copy(a, a + 3, s.playable_min);
        std::copy(b, b + 3, s.playable_max);
    }
    std::copy(s.playable_min, s.playable_min + 3, s.camera_min);
    std::copy(s.playable_max, s.playable_max + 3, s.camera_max);
    for (int i = 0; i < 2; ++i) {
        s.camera_min[i] += 400.0f;
        s.camera_max[i] -= 400.0f;
    }
    if (raw_f32x3(p.raw, 0x07, a) && raw_f32x3(p.raw, 0x08, b)) {
        std::copy(a, a + 3, s.camera_min);
        std::copy(b, b + 3, s.camera_max);
    }
    raw_f32(p.raw, 0x00, s.camera_pitch);
    raw_f32(p.raw, 0x01, s.camera_yaw);
    raw_f32(p.raw, 0x02, s.camera_distance);
    raw_f32x3(p.raw, 0x03, s.camera_look_at);
    raw_f32(p.raw, 0x06, s.camera_fov);
}

bool parse_world(std::span<const uint8_t> node, MapData& map, Ctx& c, const ParseOptions& opt) {
    std::vector<ChunkView> kids;
    if (!read_chunks(node, kids)) return c.fail("world chunk 1: malformed");
    if (opt.environments_only) {
        for (const ChunkView& k : kids) {
            if (k.id == 0x101) map.terrain.present = true;
            if (k.id == 0x100 && k.has_children && !parse_environments(k.payload, map.environments, c)) return false;
        }
        if (!map.terrain.present) map.space.present = true;  // flag only; 0x103 is not read
        return true;
    }
    // Terrain first: passability needs the grid size.
    for (const ChunkView& k : kids) {
        if (k.id != 0x101 || !k.has_children) continue;
        if (!parse_terrain(k.payload, map.terrain, c)) return false;
        break;  // first terrain chunk wins
    }
    for (const ChunkView& k : kids) {
        switch (k.id) {
        case 0x100:
            if (k.has_children && !parse_environments(k.payload, map.environments, c)) return false;
            break;
        case 0x101: break;
        case 0x102:
            if (opt.objects && k.has_children && !parse_objects(k.payload, map.objects, c)) return false;
            break;
        case 0x103: {
            if (k.has_children) break;
            PlayableArea& p = map.playable;
            if (!parse_minis_block(k.payload, p.raw, c, "playable area 0x103")) return false;
            p.present = true;
            if (map.terrain.present) {
                p.has_box = raw_f32x3(p.raw, 0x05, p.box_min) && raw_f32x3(p.raw, 0x06, p.box_max);
            } else {
                p.has_outer_box = raw_f32x3(p.raw, 0x04, p.outer_min) && raw_f32x3(p.raw, 0x05, p.outer_max);
                p.has_box = raw_f32x3(p.raw, 0x07, p.box_min) && raw_f32x3(p.raw, 0x08, p.box_max);
                decode_space(p, map.space);
            }
            break;
        }
        case 0x104:
            if (!k.has_children) map.legacy_passability.assign(k.payload.begin(), k.payload.end());
            break;
        case 0x105:
            if (!k.has_children) {
                if (!parse_minis_block(k.payload, map.space_camera.raw, c, "space camera 0x105")) return false;
                map.space_camera.present = true;
            }
            break;
        case 0x10A: {
            if (k.has_children) break;
            const Terrain& t = map.terrain;
            if (!t.present) break;
            const size_t fw = t.header.width, fh = t.header.height;
            if (k.payload.size() % 4 != 0) return c.fail("passability 0x10A: size is not a multiple of 4");
            if (k.payload.size() < fw * fh * 4) return c.fail("passability 0x10A: shorter than width x height x 4");
            map.passability.assign(static_cast<size_t>(t.width) * t.height, 0);
            map.passability_present = true;
            const size_t n = k.payload.size() / 4;
            for (size_t i = 0; i < n; ++i) {  // column-major: file index = x * H + y (1.5)
                const size_t x = fh ? i / fh : 0, y = fh ? i % fh : 0;
                if (x >= fw || x >= t.width || y >= t.height) continue;
                uint32_t v = 0;
                std::memcpy(&v, k.payload.data() + 4 * i, 4);
                map.passability[y * t.width + x] = v;
            }
            break;
        }
        case 0x10B:
            if (opt.zones && k.has_children && !parse_zones(k.payload, map.zones, c)) return false;
            break;
        case 0x10C:
            if (!k.has_children) {
                if (!parse_minis_block(k.payload, map.tactical_camera.raw, c, "tactical camera 0x10C")) return false;
                map.tactical_camera.present = true;
            }
            break;
        default:
            map.unknown_world_chunks.push_back(k.id);
            break;
        }
    }
    if (!map.terrain.present) map.space.present = true;  // defaults when 0x103 is absent
    return true;
}

}  // namespace

bool parse_ted(std::span<const uint8_t> bytes, MapData& out, std::string* error, const ParseOptions& options) {
    out = MapData{};
    Ctx c{error};
    std::vector<ChunkView> top;
    if (bytes.empty()) return c.fail("empty file");
    if (!read_chunks(bytes, top)) return c.fail("not a chunk file (top-level sequence does not end at EOF)");
    bool world = false;
    for (const ChunkView& k : top) {
        if (options.environments_only && k.id != 0 && k.id != 1) continue;
        switch (k.id) {
        case 0:
            if (!k.has_children && !parse_map_info(k.payload, out.info, c)) return false;
            break;
        case 3:
            if (!k.has_children && !parse_start_positions(k.payload, out.start_positions, c)) return false;
            break;
        case 2:
            if (k.has_children && !parse_preview(k.payload, out.preview_objects, c)) return false;
            break;
        case 1:
            if (!k.has_children) return c.fail("world chunk 1 is a leaf");
            if (world) break;  // first world chunk wins
            if (!parse_world(k.payload, out, c, options)) return false;
            world = true;
            break;
        default: break;
        }
    }
    if (!world) return c.fail("no world chunk 1");
    return true;
}

std::string map_path(std::string_view name) {
    std::string n(name);
    const bool has_ext = n.size() > 4 && ascii_iends_with(n, ".TED");
    if (!has_ext) n += ".TED";
    if (n.find_first_of("\\/") == std::string::npos) n = "DATA\\ART\\MAPS\\" + n;
    return alamo::normalize_path(n);
}

bool load_map(const FileProvider& files, std::string_view name, MapData& out, std::string* error,
              const ParseOptions& options) {
    std::vector<uint8_t> bytes;
    const std::string path = map_path(name);
    if (!files.read(path, bytes)) {
        out = MapData{};
        if (error) *error = "not found: " + path;
        return false;
    }
    return parse_ted(bytes, out, error, options);
}

CloudParams effective_clouds(const MapData& map, const Environment* env) {
    CloudParams p;
    const TerrainHeader& h = map.terrain.header;
    p.texture = h.cloud_texture;
    p.scale = h.cloud_scale;
    std::copy(h.cloud_velocity, h.cloud_velocity + 3, p.velocity);
    if (env) {
        p.texture = env->cloud_texture;
        p.scale = env->cloud_scale;
        const auto v = env->cloud_velocity();
        std::copy(v.begin(), v.end(), p.velocity);
    }
    return p;
}

}  // namespace alamo::terrain

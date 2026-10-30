// .ted writer: decoded fields first, then every raw mini whose id was not written from a field
// (so unknown minis survive a parse -> write round trip).
#include <algorithm>
#include <cstring>

#include "alamo/terrain/ted.hpp"

namespace alamo::terrain {

namespace {

std::vector<RawMini> merged(std::vector<RawMini> decoded, const std::vector<RawMini>& raw) {
    for (const RawMini& r : raw) {
        const bool written = std::any_of(decoded.begin(), decoded.end(), [&](const RawMini& d) { return d.id == r.id; });
        if (!written) decoded.push_back(r);
    }
    return decoded;
}

void write_environment(ChunkWriter& w, const Environment& e) {
    std::vector<RawMini> m;
    for (uint8_t i = 0; i < 3; ++i) m.push_back(mk_f32x3(i, e.light_color[i]));
    m.push_back(mk_f32x3(0x03, e.sun_specular));
    m.push_back(mk_f32x3(0x04, e.ambient));
    for (uint8_t i = 0; i < 3; ++i) m.push_back(mk_f32(static_cast<uint8_t>(0x05 + i), e.intensity[i]));
    for (uint8_t i = 0; i < 3; ++i) m.push_back(mk_f32(static_cast<uint8_t>(0x08 + i), e.yaw[i]));
    for (uint8_t i = 0; i < 3; ++i) m.push_back(mk_f32(static_cast<uint8_t>(0x0B + i), e.elevation[i]));
    m.push_back(mk_u32(0x13, e.field_13));
    m.push_back(mk_string(0x14, e.name));
    m.push_back(mk_u8(0x10, e.fog_enabled ? 1 : 0));
    m.push_back(mk_f32(0x11, e.fog_start));
    m.push_back(mk_f32(0x12, e.fog_end));
    m.push_back(mk_f32x3(0x0F, e.fog_color));
    m.push_back(mk_f32x3(0x15, e.clear_color));
    m.push_back(mk_string(0x29, e.weather_effect));
    m.push_back(mk_f32x3(0x17, e.shadow_color));
    m.push_back(mk_string(0x19, e.skydome[0]));
    m.push_back(mk_string(0x1A, e.skydome[1]));
    m.push_back(mk_f32(0x1B, e.skydome_scale[0]));
    m.push_back(mk_f32(0x1C, e.skydome_scale[1]));
    m.push_back(mk_f32(0x1D, e.skydome_angle_deg[0][0]));
    m.push_back(mk_f32(0x1E, e.skydome_angle_deg[1][0]));
    m.push_back(mk_f32(0x1F, e.skydome_angle_deg[0][1]));
    m.push_back(mk_f32(0x20, e.skydome_angle_deg[1][1]));
    m.push_back(mk_f32(0x21, e.lightning_intensity));
    m.push_back(mk_f32(0x24, e.bloom_cutoff));
    m.push_back(mk_f32(0x23, e.bloom_strength));
    m.push_back(mk_f32(0x28, e.bloom_size));
    m.push_back(mk_f32(0x2B, e.wind_direction_deg));
    m.push_back(mk_f32(0x2C, e.wind_speed));
    m.push_back(mk_f32(0x2D, e.cloud_scale));
    m.push_back(mk_f32(0x2E, e.cloud_speed));
    m.push_back(mk_string(0x2F, e.cloud_texture));
    const auto all = merged(std::move(m), e.raw);
    w.mini_leaf(6, all);
}

void write_material(ChunkWriter& w, const TerrainMaterial& t) {
    std::vector<RawMini> m;
    m.push_back(mk_f32(0x06, t.specular_power));
    m.push_back(mk_u8(0x0D, t.flag_0d));
    m.push_back(mk_f32(0x07, t.tile_size));
    m.push_back(mk_f32(0x08, t.rotation_z));
    m.push_back(mk_f32(0x09, t.rotation_x));
    m.push_back(mk_f32x3(0x0A, t.diffuse_color));
    m.push_back(mk_f32x3(0x0B, t.specular_color));
    m.push_back(mk_string(0x0C, t.diffuse_texture));
    m.push_back(mk_string(0x13, t.normal_texture));
    m.push_back(mk_u8(0x12, t.full_coverage ? 1 : 0));
    m.push_back(mk_u8(0x23, t.not_rendered ? 1 : 0));
    m.push_back(mk_u32(0x14, t.surface_type));
    const auto all = merged(std::move(m), t.raw);
    w.mini_leaf(3, all);
}

void write_scroll(ChunkWriter& w, uint32_t id, const WaterScroll& s) {
    if (!s.present) return;
    w.begin(id);
    const RawMini m[] = {mk_f32(0, s.tile_size), mk_f32(1, s.angle), mk_f32(2, s.speed), mk_f32(3, s.direction)};
    w.mini_leaf(0x0AA45BCDu, m);
    w.end();
}

RawMini mk_f32x2(uint8_t id, float a, float b) {
    RawMini r;
    r.id = id;
    r.data.resize(8);
    std::memcpy(r.data.data(), &a, 4);
    std::memcpy(r.data.data() + 4, &b, 4);
    return r;
}

void write_decal(ChunkWriter& w, const TerrainDecal& d) {
    std::vector<RawMini> m;
    m.push_back(mk_u32(0x00, d.field_00));
    m.push_back(mk_u8(0x01, d.permanent ? 1 : 0));
    m.push_back(mk_u32(0x02, d.render_mode));
    m.push_back(mk_f32x3(0x03, d.position));
    m.push_back(mk_f32(0x04, d.rotation));
    m.push_back(mk_f32x2(0x05, d.width, d.height));
    m.push_back(mk_f32(0x06, d.intensity));
    m.push_back(mk_u32(0x07, d.slot_column));
    m.push_back(mk_u32(0x08, d.slot_row));
    m.push_back(mk_f32(0x0A, d.fade_time));
    m.push_back(mk_u32(0x0B, d.slot_scale));
    auto all = merged(std::move(m), d.raw);
    // The legacy f64 fade time (09) is superseded by 0A.
    all.erase(std::remove_if(all.begin(), all.end(), [](const RawMini& r) { return r.id == 0x09; }), all.end());
    w.mini_leaf(0x10, all);
}

void write_track(ChunkWriter& w, const TerrainTrack& t) {
    w.begin(0x100);
    {
        std::vector<RawMini> m;
        m.push_back(mk_f32(0x00, t.field_00));
        m.push_back(mk_u32(0x01, t.flags));
        m.push_back(mk_u32(0x02, t.sample_spacing));
        m.push_back(mk_f32(0x03, t.u_speed));
        m.push_back(mk_f32(0x04, t.v_speed));
        m.push_back(mk_f32(0x05, t.opacity));
        m.push_back(mk_u32(0x06, t.stored_point_count));
        m.push_back(mk_f32(0x0A, t.first_row_alpha));
        m.push_back(mk_f32(0x0B, t.last_row_alpha));
        m.push_back(mk_f32(0x0C, t.height_offset));
        m.push_back(mk_f32(0x0D, t.repeat_length));
        m.push_back(mk_u8(0x0E, t.clamp_u ? 1 : 0));
        m.push_back(mk_u8(0x0F, t.clamp_v ? 1 : 0));
        m.push_back(mk_u32(0x10, t.height_mode));
        const auto all = merged(std::move(m), t.raw);
        w.mini_leaf(0x101, all);
    }
    {
        std::vector<RawMini> m;
        for (const auto& p : t.points) {
            const float v[3] = {p[0], p[1], p[2]};
            m.push_back(mk_f32x3(0x07, v));
        }
        w.mini_leaf(0x102, m);
    }
    {
        std::vector<uint8_t> name(t.texture.begin(), t.texture.end());
        name.push_back(0);
        w.leaf(0x103, name);
    }
    {
        std::vector<RawMini> m;
        for (float x : t.widths) m.push_back(mk_f32(0x08, x));
        w.mini_leaf(0x104, m);
    }
    if (t.profile_present) {
        std::vector<RawMini> m;
        for (float x : t.profile) m.push_back(mk_f32(0x09, x));
        w.mini_leaf(0x105, m);
    }
    w.end();
}

void write_terrain(ChunkWriter& w, const Terrain& t) {
    const TerrainHeader& h = t.header;
    w.begin(0x101);
    {
        std::vector<RawMini> m;
        m.push_back(mk_u32(0x00, t.width));
        m.push_back(mk_u32(0x01, t.height));
        m.push_back(mk_u32(0x04, t.width * t.height));
        m.push_back(mk_u32(0x05, kMaterialSlots));
        m.push_back(mk_f32(0x0F, h.cloud_scale));
        m.push_back(mk_f32x3(0x10, h.cloud_velocity));
        m.push_back(mk_string(0x0E, h.cloud_texture));
        m.push_back(mk_u8(0x11, h.use_vertex_brightness ? 1 : 0));
        m.push_back(mk_f32(0x15, h.water_height));
        m.push_back(mk_u32(0x16, h.water_type == WaterType::None ? h.water_type_raw : static_cast<uint32_t>(h.water_type)));
        m.push_back(mk_f32(0x17, h.reflection_fraction));
        m.push_back(mk_f32(0x20, h.water_alpha));
        m.push_back(mk_f32(0x18, h.reflection_distortion));
        m.push_back(mk_f32(0x19, h.refraction_distortion));
        m.push_back(mk_f32x3(0x1A, h.water_color));
        m.push_back(mk_f32x3(0x1B, h.unused_1b));
        m.push_back(mk_string(0x1D, h.water_texture[0]));
        m.push_back(mk_string(0x1E, h.water_texture[1]));
        m.push_back(mk_string(0x1F, h.water_texture[2]));
        m.push_back(mk_f32(0x22, h.shore_min_alpha));
        m.push_back(mk_f32(0x21, h.shore_fade_depth));
        const auto all = merged(std::move(m), h.raw);
        w.mini_leaf(0, all);
    }
    write_scroll(w, 7, t.water_scroll[0]);
    write_scroll(w, 8, t.water_scroll[1]);
    {
        const size_t n = static_cast<size_t>(t.width) * t.height;
        std::vector<uint8_t> v(n * 4);
        for (size_t i = 0; i < n; ++i) {
            const int16_t hh = i < t.heights.size() ? t.heights[i] : int16_t{0};
            std::memcpy(v.data() + 4 * i, &hh, 2);
            v[4 * i + 2] = i < t.material.size() ? t.material[i] : uint8_t{0};
            v[4 * i + 3] = i < t.brightness.size() ? t.brightness[i] : uint8_t{0xFF};
        }
        w.leaf(5, v);
    }
    w.begin(2);
    for (const TerrainMaterial& m : t.materials) write_material(w, m);
    w.end();
    if (!t.decals.empty()) {
        w.begin(6);
        for (const TerrainDecal& d : t.decals) write_decal(w, d);
        w.end();
    } else {
        w.leaf(6, {});
    }
    if (!t.tracks.empty()) {
        w.begin(9);
        for (const TerrainTrack& k : t.tracks) write_track(w, k);
        w.end();
    } else {
        w.leaf(9, {});
    }
    w.end();
}

void write_object(ChunkWriter& w, const ObjectPlacement& o) {
    w.begin(0x44C);
    {
        const RawMini m[] = {mk_u32(0, o.list_id[0]), mk_u32(1, o.list_id[1])};
        w.mini_leaf(0x454, m);
    }
    w.begin(0x459);
    {
        std::vector<RawMini> m;
        m.push_back(mk_u32(0x02, o.owner));
        m.push_back(mk_u32(0x00, o.object_id));
        m.push_back(mk_u32(0x01, o.type_crc));
        m.push_back(mk_f32(0x03, o.health_fraction));
        m.push_back(mk_f32x3(0x04, o.position));
        m.push_back(mk_f32x3(0x05, o.rotation_deg));
        m.push_back(mk_f32x3(0x12, o.rotation_copy));
        if (o.has_secondary) m.push_back(mk_f32x3(0x15, o.secondary));
        auto all = merged(std::move(m), o.placement_raw);
        if (!o.has_secondary)
            all.erase(std::remove_if(all.begin(), all.end(), [](const RawMini& r) { return r.id == 0x15; }), all.end());
        w.mini_leaf(0x4B0, all);
    }
    w.leaf(0x4B9, {});
    w.begin(0x4BA);
    const uint32_t slots = std::max<uint32_t>(o.attachment_slots, static_cast<uint32_t>(o.attachments.size()));
    for (uint32_t i = 0; i < slots; ++i) {
        const std::string name = i < o.attachments.size() ? o.attachments[i] : std::string("NONE");
        const RawMini m[] = {mk_string(1, name), mk_u8(0, 0), mk_u8(2, 0)};
        w.mini_leaf(0, m);
    }
    w.end();
    w.end();  // 0x459
    w.begin(0x476);
    w.mini_leaf(1, o.owner_raw);
    w.end();
    w.end();  // 0x44C
}

void write_zone(ChunkWriter& w, const AiZone& z) {
    w.begin(0);
    {
        std::vector<RawMini> m;
        m.push_back(mk_u32(0x02, z.id));
        m.push_back(mk_u32(0x03, z.stored_cells));
        m.push_back(mk_string(0x01, z.name));
        m.push_back(mk_f32x3(0x05, z.center));
        const auto all = merged(std::move(m), z.raw);
        w.mini_leaf(0, all);
    }
    w.begin(0xE);
    for (const AiZone::Link& l : z.links) {
        const RawMini m[] = {mk_u32(1, l.zone), mk_u32(2, l.kind)};
        w.mini_leaf(0, m);
    }
    w.end();
    w.begin(0x7);
    for (const AiZone::Cell& c : z.cells) {
        const RawMini m[] = {mk_u32(1, static_cast<uint32_t>(c.x) | (static_cast<uint32_t>(c.y) << 16)), mk_u8(2, c.flag)};
        w.mini_leaf(0, m);
    }
    w.end();
    w.leaf(0xF, {});
    w.end();
}

}  // namespace

std::vector<uint8_t> write_ted(const MapData& map) {
    ChunkWriter w;
    {  // 0 map info
        const MapInfo& mi = map.info;
        std::vector<RawMini> m;
        m.push_back(mk_u32(0x00, mi.format));
        m.push_back(mk_u32(0x01, mi.map_type));
        for (uint8_t i = 0; i < 6; ++i) m.push_back(mk_u32(static_cast<uint8_t>(0x02 + i), mi.counts[i]));
        m.push_back(mk_u8(0x0B, mi.flag_0b));
        m.push_back(mk_utf16(0x08, mi.name_key));
        m.push_back(mk_utf16(0x09, mi.planet_name));
        m.push_back(mk_utf16(0x0A, mi.text_0a));
        m.push_back(mk_f32(0x10, mi.world_width));
        m.push_back(mk_f32(0x11, mi.world_height));
        m.push_back(mk_u8(0x12, mi.flag_12));
        const auto all = merged(std::move(m), mi.raw);
        w.mini_leaf(0, all);
    }
    {  // 3 start positions
        std::vector<uint8_t> p(4 + 8 * map.start_positions.size());
        const uint32_t n = static_cast<uint32_t>(map.start_positions.size());
        std::memcpy(p.data(), &n, 4);
        for (size_t i = 0; i < map.start_positions.size(); ++i) {
            std::memcpy(p.data() + 4 + 8 * i, &map.start_positions[i].x, 4);
            std::memcpy(p.data() + 8 + 8 * i, &map.start_positions[i].y, 4);
        }
        w.leaf(3, p);
    }
    if (!map.preview_objects.empty()) {  // 2 preview
        w.begin(2);
        for (const PreviewObject& o : map.preview_objects) {
            const RawMini m[] = {mk_u32(0x0C, o.object_id), mk_u32(0x0D, o.type_crc), mk_f32(0x0E, o.position.x),
                                 mk_f32(0x0F, o.position.y)};
            w.mini_leaf(0xB, m);
        }
        w.end();
    }
    w.begin(1);
    {  // 0x100 environments
        const EnvironmentSet& es = map.environments;
        w.begin(0x100);
        w.begin(4);
        for (const Environment& e : es.environments) write_environment(w, e);
        w.leaf(5, {});
        w.end();
        if (es.scene_fog_present) {
            w.begin(2);
            const RawMini m[] = {mk_f32x3(0, es.scene_fog_color), mk_u8(1, es.scene_fog_enabled ? 1 : 0),
                                 mk_f32(2, es.scene_fog_start), mk_f32(3, es.scene_fog_end)};
            w.mini_leaf(0, m);
            w.end();
        }
        const RawMini idx[] = {mk_u32(0x25, es.start_index), mk_u32(0x2A, es.field_2a)};
        w.mini_leaf(8, idx);
        w.end();
    }
    if (map.terrain.present) write_terrain(w, map.terrain);
    if (map.passability_present && map.terrain.present) {  // 0x10A column-major
        const uint32_t W = map.terrain.width, H = map.terrain.height;
        std::vector<uint8_t> p(static_cast<size_t>(W) * H * 4);
        for (uint32_t x = 0; x < W; ++x)
            for (uint32_t y = 0; y < H; ++y) {
                const uint32_t v = map.passability_at(x, y);
                std::memcpy(p.data() + 4 * (static_cast<size_t>(x) * H + y), &v, 4);
            }
        w.leaf(0x10A, p);
    }
    {  // 0x102 objects
        w.begin(0x102);
        w.leaf(0, {});
        w.begin(1);
        for (const ObjectPlacement& o : map.objects) write_object(w, o);
        w.end();
        w.leaf(8, {});
        w.end();
    }
    if (map.playable.present) {
        std::vector<RawMini> m;
        const PlayableArea& p = map.playable;
        if (map.terrain.present) {
            if (p.has_box) {
                m.push_back(mk_f32x3(0x05, p.box_min));
                m.push_back(mk_f32x3(0x06, p.box_max));
            }
        } else {
            if (p.has_outer_box) {
                m.push_back(mk_f32x3(0x04, p.outer_min));
                m.push_back(mk_f32x3(0x05, p.outer_max));
            }
            if (p.has_box) {
                m.push_back(mk_f32x3(0x07, p.box_min));
                m.push_back(mk_f32x3(0x08, p.box_max));
            }
        }
        const auto all = merged(std::move(m), p.raw);
        w.mini_leaf(0x103, all);
    }
    if (!map.legacy_passability.empty()) w.leaf(0x104, map.legacy_passability);
    if (map.space_camera.present) w.mini_leaf(0x105, map.space_camera.raw);
    if (!map.zones.empty()) {
        w.begin(0x10B);
        for (const AiZone& z : map.zones) write_zone(w, z);
        w.end();
    }
    if (map.tactical_camera.present) w.mini_leaf(0x10C, map.tactical_camera.raw);
    w.end();  // world
    return w.take();
}

}  // namespace alamo::terrain

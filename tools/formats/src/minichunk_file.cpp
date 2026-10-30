#include "alamo/formats/minichunk_file.hpp"

#include <cstring>

namespace alamo::minifile {

namespace {
void walk(ChunkCursor cur, File& f) {
    while (auto c = cur.next()) {
        if (c->has_children) {
            walk(c->children(), f);
            continue;
        }
        Record r;
        r.chunk_id = c->id;
        auto mc = c->minis();
        while (auto m = mc.next()) {
            r.mini_ids.push_back(m->id);
            r.mini_data.emplace_back(m->data.begin(), m->data.end());
        }
        f.records.push_back(std::move(r));
    }
}
}  // namespace

File parse(Bytes data) {
    File f;
    walk(top_level_chunks(data), f);
    return f;
}

File load(const std::filesystem::path& p) {
    auto b = read_file(p);
    return parse(b);
}

const std::vector<std::uint8_t>* Record::get(std::uint8_t id) const {
    for (std::size_t i = 0; i < mini_ids.size(); ++i)
        if (mini_ids[i] == id) return &mini_data[i];
    return nullptr;
}

std::vector<std::string> Record::strings() const {
    std::vector<std::string> out;
    for (const auto& d : mini_data) {
        if (d.size() < 2 || d.back() != 0) continue;
        bool printable = true;
        for (std::size_t i = 0; i + 1 < d.size(); ++i)
            if (d[i] < 0x20 || d[i] > 0x7E) { printable = false; break; }
        if (printable) out.emplace_back(reinterpret_cast<const char*>(d.data()), d.size() - 1);
    }
    return out;
}

std::optional<float> Record::f32(std::uint8_t id) const {
    auto* d = get(id);
    if (!d || d->size() != 4) return std::nullopt;
    float v;
    std::memcpy(&v, d->data(), 4);
    return v;
}
std::optional<std::uint32_t> Record::u32(std::uint8_t id) const {
    auto* d = get(id);
    if (!d || d->empty() || d->size() > 4 || d->size() == 3) return std::nullopt;
    std::uint32_t v = 0;
    std::memcpy(&v, d->data(), d->size());
    return v;
}
std::optional<Vec3> Record::vec3(std::uint8_t id) const {
    auto* d = get(id);
    if (!d || d->size() != 12) return std::nullopt;
    Vec3 v;
    std::memcpy(&v, d->data(), 12);
    return v;
}
std::optional<std::string> Record::str(std::uint8_t id) const {
    auto* d = get(id);
    if (!d) return std::nullopt;
    return to_cstring(Bytes(d->data(), d->size()));
}

TerrainMaterial decode_terrain_material(const Record& r) {
    TerrainMaterial m;
    if (auto v = r.str(0x0C)) m.diffuse_texture = *v;
    if (auto v = r.str(0x13)) m.normal_texture = *v;
    if (auto v = r.f32(0x07)) m.tile_size = *v;
    if (auto v = r.f32(0x08)) m.uv_rotation_z = *v;
    if (auto v = r.f32(0x09)) m.uv_rotation_x = *v;
    if (auto v = r.f32(0x06)) m.specular_power = *v;
    if (auto v = r.vec3(0x0A)) m.diffuse_color = *v;
    if (auto v = r.vec3(0x0B)) m.specular_color = *v;
    if (auto v = r.u32(0x14)) m.surface_type = *v;
    if (auto v = r.u32(0x0D)) m.flag_0d = static_cast<std::uint8_t>(*v);
    if (auto v = r.u32(0x12)) m.full_coverage = *v != 0;
    if (auto v = r.u32(0x23)) m.not_rendered = *v != 0;
    return m;
}

Environment decode_environment(const Record& r) {
    Environment e;
    if (auto v = r.str(0x14)) e.name = *v;
    for (std::uint8_t i = 0; i < 3; ++i) {
        if (auto v = r.vec3(i)) e.light_color[i] = *v;
        if (auto v = r.f32(std::uint8_t(0x05 + i))) e.light_intensity[i] = *v;
        if (auto v = r.f32(std::uint8_t(0x08 + i))) e.light_angle_z[i] = *v;
        if (auto v = r.f32(std::uint8_t(0x0B + i))) e.light_angle_x[i] = *v;
    }
    if (auto v = r.vec3(0x03)) e.specular_color = *v;
    if (auto v = r.vec3(0x04)) e.ambient_color = *v;
    if (auto v = r.vec3(0x0F)) e.fog_color = *v;
    if (auto v = r.u32(0x10)) e.fog_enabled = *v != 0;
    const float a = r.f32(0x11).value_or(0.f), b = r.f32(0x12).value_or(0.f);
    e.fog_start = a < b ? a : b;
    e.fog_end = a < b ? b : a;
    if (auto v = r.str(0x19)) e.preset_name = *v;
    if (auto v = r.str(0x29)) e.weather_effect = *v;
    if (auto v = r.str(0x2F)) e.cloud_texture = *v;
    return e;
}

}  // namespace alamo::minifile

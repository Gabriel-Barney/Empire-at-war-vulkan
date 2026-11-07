// R5 runtime terrain decorations data + runtime decals. See surface_fx.hpp.
#include "alamo/render/surface_fx.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::render {

namespace {

data::XmlDocument parse_doc(std::span<const uint8_t> xml, const char* file, std::string* error) {
    data::XmlDocument doc = data::XmlDocument::parse(xml, file);
    if (!doc.ok() && error) *error = std::string(file) + ": no XML root element";
    return doc;
}

std::string name_attr(const data::XmlNode& n) {
    std::string_view v = n.attribute("Name");
    if (v.empty()) v = n.attribute("name");
    return std::string(data::trim(v));
}

std::string none_to_empty(std::string_view s) { return is_none_name(s) ? std::string() : std::string(data::trim(s)); }

void read_settings(const data::XmlNode& n, std::vector<SurfaceSettings>& out) {
    SurfaceSettings s;
    s.surface = ascii_lower(name_attr(n));
    for (const data::XmlNode c : n.children()) {
        const std::string_view tag = c.name();
        const std::string_view text = c.text_trimmed();
        if (ascii_iequals(tag, "Dynamic_Track_Name")) s.dynamic_track = none_to_empty(text);
        else if (ascii_iequals(tag, "Dynamic_Track_Left_Bone")) s.left_bone = none_to_empty(text);
        else if (ascii_iequals(tag, "Dynamic_Track_Right_Bone")) s.right_bone = none_to_empty(text);
        else if (ascii_iequals(tag, "Decal_Name")) s.decal = none_to_empty(text);
        else if (ascii_iequals(tag, "Particle_Name")) s.particle = none_to_empty(text);
        else if (ascii_iequals(tag, "SoundFX_Name")) s.sound = none_to_empty(text);
        else if (ascii_iequals(tag, "Particle_Min_Energy")) s.particle_min_energy = data::parse_float_or(text, 0.0f);
    }
    out.push_back(std::move(s));
    // [port] nested <Surface_Settings> (unclosed tags in the shipped file) become siblings after their parent.
    for (const data::XmlNode c : n.children())
        if (ascii_iequals(c.name(), "Surface_Settings")) read_settings(c, out);
}

}  // namespace

bool is_none_name(std::string_view s) {
    s = data::trim(s);
    return s.empty() || ascii_iequals(s, "none");
}

const SurfaceSettings* SurfaceFxDef::find(std::string_view surface) const {
    for (const SurfaceSettings& s : settings)
        if (ascii_iequals(s.surface, surface)) return &s;
    for (const SurfaceSettings& s : settings)
        if (s.surface == "default") return &s;
    return nullptr;
}

bool parse_surface_fx(std::span<const uint8_t> xml, std::vector<SurfaceFxDef>& inout, std::string* error) {
    const data::XmlDocument doc = parse_doc(xml, "SurfaceFX.xml", error);
    if (!doc.ok()) return false;
    for (const data::XmlNode n : doc.root().children()) {
        if (!ascii_iequals(n.name(), "SurfaceFX")) continue;
        SurfaceFxDef d;
        d.name = name_attr(n);
        for (const data::XmlNode c : n.children())
            if (ascii_iequals(c.name(), "Surface_Settings")) read_settings(c, d.settings);
        inout.push_back(std::move(d));
    }
    return true;
}

bool parse_terrain_decal_fx(std::span<const uint8_t> xml, std::vector<DecalFxDef>& inout, std::string* error) {
    const data::XmlDocument doc = parse_doc(xml, "TerrainDecalFX.xml", error);
    if (!doc.ok()) return false;
    for (const data::XmlNode n : doc.root().children()) {
        if (!ascii_iequals(n.name(), "Decal")) continue;
        DecalFxDef d;
        d.name = name_attr(n);
        for (const data::XmlNode c : n.children()) {
            const std::string_view tag = c.name();
            const std::string_view text = c.text_trimmed();
            if (ascii_iequals(tag, "Render_Mode")) {
                d.mode = terrain::render_mode_from_string(text);
            } else if (ascii_iequals(tag, "Intensity")) {
                d.intensity = data::parse_float_or(text, d.intensity);
            } else if (ascii_iequals(tag, "Scale")) {
                float v[2] = {1, 1};
                const size_t k = data::parse_floats(text, v, 2);
                if (k >= 1) d.scale_w = v[0];
                d.scale_h = k >= 2 ? v[1] : d.scale_w;
            } else if (ascii_iequals(tag, "UV_Slot")) {
                float v[64];
                const size_t k = data::parse_floats(text, v, 64);
                for (size_t i = 0; i + 1 < k; i += 2) {
                    const float a = std::max(0.0f, v[i]), b = std::max(0.0f, v[i + 1]);
                    d.uv_slots.push_back({static_cast<uint32_t>(std::min(a, 1024.0f)), static_cast<uint32_t>(std::min(b, 1024.0f))});
                }
            } else if (ascii_iequals(tag, "Slot_Scale")) {
                const int32_t s = data::parse_int_or(text, 1);
                d.slot_scale = s > 0 ? static_cast<uint32_t>(s) : 1u;
            } else if (ascii_iequals(tag, "Fadeout_Time")) {
                d.fadeout_time = data::parse_float_or(text, 0.0f);
            } else if (ascii_iequals(tag, "Permanent")) {
                d.permanent = data::parse_bool_or(text, false);
            } else if (ascii_iequals(tag, "Random_Rotate")) {
                d.random_rotate = data::parse_bool_or(text, false);
            } else if (ascii_iequals(tag, "Random_Size_Offset")) {
                d.random_size_offset = data::parse_float_or(text, 0.0f);
            }
        }
        for (float* f : {&d.intensity, &d.scale_w, &d.scale_h, &d.fadeout_time, &d.random_size_offset})
            if (!std::isfinite(*f)) *f = 0.0f;
        inout.push_back(std::move(d));
    }
    return true;
}

bool SurfaceFxLibrary::load(const FileProvider& files) {
    bool any = false;
    std::vector<uint8_t> xml;
    if (files.read("Data\\XML\\SurfaceFX.xml", xml)) any |= parse_surface_fx(xml, surface_fx_);
    xml.clear();
    if (files.read("Data\\XML\\TerrainDecalFX.xml", xml)) any |= parse_terrain_decal_fx(xml, decals_);
    xml.clear();
    if (files.read("Data\\XML\\DynamicTrackFX.xml", xml)) any |= terrain::parse_dynamic_track_fx(xml, tracks_);
    return any;
}

const SurfaceFxDef* SurfaceFxLibrary::find_surface_fx(std::string_view name) const {
    if (is_none_name(name)) return nullptr;
    name = data::trim(name);
    for (const SurfaceFxDef& d : surface_fx_)
        if (ascii_iequals(d.name, name)) return &d;
    return nullptr;
}

const DecalFxDef* SurfaceFxLibrary::find_decal(std::string_view name) const {
    if (is_none_name(name)) return nullptr;
    name = data::trim(name);
    for (const DecalFxDef& d : decals_)
        if (ascii_iequals(d.name, name)) return &d;
    return nullptr;
}

const terrain::DynamicTrackParams* SurfaceFxLibrary::find_track(std::string_view name) const {
    if (is_none_name(name)) return nullptr;
    name = data::trim(name);
    for (const terrain::DynamicTrackParams& d : tracks_)
        if (ascii_iequals(d.name, name)) return &d;
    return nullptr;
}

std::vector<std::string> SurfaceFxLibrary::track_textures() const {
    std::vector<std::string> out;
    for (const terrain::DynamicTrackParams& p : tracks_) {
        if (p.texture.empty()) continue;
        bool dup = false;
        for (const std::string& t : out) dup = dup || ascii_iequals(t, p.texture);
        if (!dup) out.push_back(p.texture);
    }
    return out;
}

// ------------------------------------------------------------------------------------------- surface type
uint32_t terrain_surface_type(const terrain::Terrain& t, float x, float y) {
    if (t.width < 2 || t.height < 2 || !std::isfinite(x) || !std::isfinite(y)) return 0;
    if (t.header.water_type != terrain::WaterType::None && t.sample_height(x, y) < t.header.water_height) {
        switch (t.header.water_type) {
            case terrain::WaterType::Water: return 4;
            case terrain::WaterType::Lava: return 8;
            case terrain::WaterType::Ice: return 7;
            default: break;
        }
    }
    const float gx = std::clamp(x / terrain::kVertexSpacing, 0.0f, static_cast<float>(t.width - 1));
    const float gy = std::clamp(y / terrain::kVertexSpacing, 0.0f, static_cast<float>(t.height - 1));
    const uint8_t m = t.material_at(static_cast<int64_t>(gx + 0.5f), static_cast<int64_t>(gy + 0.5f));
    return m < t.materials.size() ? t.materials[m].surface_type : 0u;
}

std::string_view surface_type_name(uint32_t s) {
    static constexpr std::string_view kNames[] = {"default", "grass", "dirt", "snow", "water", "concrete", "mud", "ice", "lava"};
    return s < std::size(kNames) ? kNames[s] : std::string_view("default");
}

// ------------------------------------------------------------------------------------------- runtime decals
uint64_t RenderRng::next() {
    uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float RenderRng::unit() { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); }

float RenderRng::range(float lo, float hi) { return lo + unit() * (hi - lo); }

uint32_t RenderRng::below(uint32_t n) { return n == 0 ? 0u : static_cast<uint32_t>(next() % n); }

DecalSpawn RuntimeDecals::spawn(const terrain::Terrain& terrain, const DecalFxDef& fx, float x, float y, float z, float heading,
                                float now) {
    if (!terrain.present || terrain.width < 2 || terrain.height < 2) return DecalSpawn::NoTerrain;
    // Size = Scale exactly. Random_Size_Offset is parsed but not applied: the g1f t05 capture (RENDER.md 7.3,
    // DECORATIONS 4.2) showed every drawn EXPLOSION_SCORCH_SMALL / _TINY decal at its Scale within 0.005 (offset 0.1 in
    // the XML), i.e. a size factor of 1 in this build.
    // Random values: slot pair, rotation [port order].
    const float w = fx.scale_w, h = fx.scale_h;
    std::array<uint32_t, 2> slot{0, 0};
    if (!fx.uv_slots.empty()) slot = fx.uv_slots[fx.uv_slots.size() > 1 ? rng_.below(static_cast<uint32_t>(fx.uv_slots.size())) : 0u];
    const float kTwoPi = 6.28318530718f;
    const float rot = fx.random_rotate ? rng_.range(-kTwoPi, kTwoPi) : heading;
    // Rejections (§4.2).
    if (std::fabs(terrain.sample_height(x, y) - z) > 0.53f * std::max(w, h)) {
        ++stats_.rejected_height;
        return DecalSpawn::HeightMismatch;
    }
    uint32_t covering = 0;
    for (const Live& l : live_) {
        if (l.alpha < 0.3f || !l.frame.valid) continue;
        const std::array<float, 2> p = l.frame.local(x, y);
        if (p[0] >= 0.0f && p[0] <= 1.0f && p[1] >= 0.0f && p[1] <= 1.0f) ++covering;
    }
    if (covering >= 2) {
        ++stats_.rejected_covered;
        return DecalSpawn::Covered;
    }
    Live l;
    terrain::TerrainDecal& r = l.record;
    r.permanent = fx.permanent;
    r.render_mode = static_cast<uint32_t>(fx.mode);
    r.position[0] = x;
    r.position[1] = y;
    r.position[2] = z;
    r.rotation = rot;
    r.width = w;
    r.height = h;
    r.intensity = fx.intensity;
    r.slot_column = slot[0];
    r.slot_row = slot[1];
    r.fade_time = fx.fadeout_time;
    r.slot_scale = fx.slot_scale;
    r.slot_count = 16;
    if (!terrain::build_decal_geometry(terrain, r, l.geometry) || l.geometry.mesh.empty()) {
        ++stats_.no_geometry;
        return DecalSpawn::NoGeometry;
    }
    l.frame = terrain::DecorationFrame::make(x, y, w, h, rot);
    l.birth = now;
    l.alpha = fx.intensity;
    terrain::set_decal_alpha(l.geometry, l.alpha);
    if (live_.size() >= max_decals && max_decals > 0) {  // [port] cap: oldest non-permanent first, else the oldest
        auto it = std::find_if(live_.begin(), live_.end(), [](const Live& v) { return !v.record.permanent; });
        if (it == live_.end()) it = live_.begin();
        live_.erase(it);
        ++stats_.evicted;
    }
    live_.push_back(std::move(l));
    ++stats_.spawned;
    stats_.live = static_cast<uint32_t>(live_.size());
    return DecalSpawn::Spawned;
}

void RuntimeDecals::update(float now) {
    for (size_t i = 0; i < live_.size();) {
        Live& l = live_[i];
        const terrain::DecalFade f = terrain::decal_fade(l.record, now - l.birth);
        if (f.dead) {
            live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats_.expired;
            continue;
        }
        if (f.alpha != l.alpha) {
            l.alpha = f.alpha;
            terrain::set_decal_alpha(l.geometry, l.alpha);
        }
        ++i;
    }
    stats_.live = static_cast<uint32_t>(live_.size());
}

std::vector<terrain::DecalGeometry> RuntimeDecals::geometry() const {
    std::vector<terrain::DecalGeometry> out;
    out.reserve(live_.size());
    for (const Live& l : live_) out.push_back(l.geometry);
    return out;
}

}  // namespace alamo::render

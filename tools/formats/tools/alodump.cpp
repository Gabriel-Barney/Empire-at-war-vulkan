// alodump: print a JSON summary of an Alamo asset (.alo model/particles, .ala, .mtd, .dat, .tem, .tee).
//
//   alodump [--full] [--max N] <file>
//     --full   include bone matrices, every material parameter, track data, all strings
//     --max N  cap list lengths in the output (default 64, 0 = unlimited)
#include "alamo/formats/ala.hpp"
#include "alamo/formats/alo.hpp"
#include "alamo/formats/minichunk_file.hpp"
#include "alamo/formats/text.hpp"
#include "json_writer.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

using namespace alamo;
using tools::JsonWriter;

namespace {

bool g_full = false;
std::size_t g_max = 64;

std::size_t cap(std::size_t n) { return (g_max == 0 || g_full) ? n : std::min(n, g_max); }

void vec3(JsonWriter& j, const Vec3& v) { j.floats({v.x, v.y, v.z}); }
void aabb(JsonWriter& j, const AABB& b) {
    j.begin_object();
    j.key("min"); vec3(j, b.min);
    j.key("max"); vec3(j, b.max);
    j.end_object();
}

void param_value(JsonWriter& j, const alo::MaterialParam& p) {
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::int32_t>) j.value(static_cast<long long>(v));
        else if constexpr (std::is_same_v<T, float>) j.value(v);
        else if constexpr (std::is_same_v<T, Vec3>) j.floats({v.x, v.y, v.z});
        else if constexpr (std::is_same_v<T, Vec4>) j.floats({v.x, v.y, v.z, v.w});
        else j.value(v);
    }, p.value);
}

const char* param_type(alo::ParamType t) {
    switch (t) {
        case alo::ParamType::Int: return "int";
        case alo::ParamType::Float: return "float";
        case alo::ParamType::Float3: return "float3";
        case alo::ParamType::Texture: return "texture";
        case alo::ParamType::Float4: return "float4";
    }
    return "?";
}

void dump_model(JsonWriter& j, const alo::Model& m) {
    j.kv("type", "alo-model");
    const auto& s = m.skeleton;
    j.key("skeleton").begin_object();
    j.kv("bone_count", s.size());
    j.key("bones").begin_array();
    for (std::size_t i = 0; i < cap(s.size()); ++i) {
        j.begin_object();
        j.kv("index", i).kv("name", s.names[i]).kv("parent", static_cast<long long>(s.parents[i]));
        if (!s.visible[i]) j.kv("visible", false);
        if (s.billboard_mode[i]) j.kv("billboard", alo::billboard_mode_name(s.billboard_mode[i]));
        if (g_full) {
            j.key("local").begin_array();
            for (auto& row : s.local[i].m) j.floats({row[0], row[1], row[2], row[3]});
            j.end_array();
        } else {
            const auto& t = s.local[i].m;
            j.key("translation").floats({t[0][3], t[1][3], t[2][3]});
        }
        j.end_object();
    }
    j.end_array().end_object();

    std::size_t total_v = 0, total_t = 0;
    j.key("meshes").begin_array();
    for (std::size_t mi = 0; mi < m.meshes.size(); ++mi) {
        const auto& mesh = m.meshes[mi];
        j.begin_object();
        j.kv("name", mesh.name).kv("bone", m.mesh_bone(static_cast<std::uint32_t>(mi)));
        j.key("bounds"); aabb(j, mesh.bounds);
        j.kv("hidden", mesh.hidden).kv("collision", mesh.collision_enabled);
        j.key("submeshes").begin_array();
        for (const auto& sm : mesh.submeshes) {
            total_v += sm.vertices.size();
            total_t += sm.indices.size() / 3;
            j.begin_object();
            j.kv("shader", sm.material.shader).kv("vertex_format", sm.vertex_format);
            if (!sm.format.known) j.kv("vertex_format_known", false);
            j.kv("vertices", sm.vertices.size()).kv("triangles", sm.indices.size() / 3);
            if (!sm.bone_map.empty()) {
                j.key("bone_map").begin_array();
                for (auto b : sm.bone_map) j.value(b);
                j.end_array();
            }
            if (sm.collision)
                j.key("collision_tree").begin_object().kv("nodes", sm.collision->nodes.size())
                    .kv("triangles", sm.collision->triangles.size()).end_object();
            j.key("params").begin_object();
            for (const auto& p : sm.material.params) {
                if (!g_full && p.type != alo::ParamType::Texture) continue;
                j.key(p.name);
                if (g_full) {
                    j.begin_object().kv("type", param_type(p.type)).key("value");
                    param_value(j, p);
                    j.end_object();
                } else {
                    param_value(j, p);
                }
            }
            j.end_object();
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }
    j.end_array();

    j.key("lights").begin_array();
    for (std::size_t li = 0; li < m.lights.size(); ++li) {
        const auto& l = m.lights[li];
        j.begin_object().kv("name", l.name).kv("type", static_cast<unsigned>(l.type));
        j.key("color"); vec3(j, l.color);
        j.kv("intensity", l.intensity).kv("far_attenuation_start", l.far_attenuation_start)
            .kv("far_attenuation_end", l.far_attenuation_end).kv("hotspot", l.hotspot_size)
            .kv("falloff", l.falloff_size);
        j.end_object();
    }
    j.end_array();

    j.key("proxies").begin_array();
    for (std::size_t i = 0; i < cap(m.proxies.size()); ++i) {
        const auto& p = m.proxies[i];
        j.begin_object().kv("name", p.name).kv("bone", p.bone).kv("bone_name", s.names.at(p.bone));
        if (p.hidden) j.kv("hidden", true);
        if (p.alt_decrease_stay_hidden) j.kv("alt_decrease_stay_hidden", true);
        j.end_object();
    }
    j.end_array();

    j.key("stats").begin_object();
    j.kv("meshes", m.meshes.size()).kv("lights", m.lights.size()).kv("connections", m.connections.size())
        .kv("proxies", m.proxies.size()).kv("vertices", total_v).kv("triangles", total_t);
    if (m.declared_dazzles) j.kv("declared_dazzles", *m.declared_dazzles);
    j.end_object();

    j.key("unknown_chunks").begin_array();
    for (const auto& u : m.unknown)
        j.begin_object().kv("path", u.path).kv("offset", u.offset).kv("size", u.size).end_object();
    j.end_array();
}

void dump_shape(JsonWriter& j, const particle::SpawnShape& s) {
    static const char* kinds[] = {"point", "box", "cube", "sphere", "cylinder", "kind5"};
    const auto k = static_cast<std::uint32_t>(s.kind);
    j.begin_object().kv("kind", k < 6 ? kinds[k] : "?");
    switch (s.kind) {
        case particle::ShapeKind::Point: j.key("value").floats({s.point_value.x, s.point_value.y, s.point_value.z}); break;
        case particle::ShapeKind::Box:
            j.key("min").floats({s.box_min.x, s.box_min.y, s.box_min.z});
            j.key("max").floats({s.box_max.x, s.box_max.y, s.box_max.z});
            break;
        case particle::ShapeKind::Cube: j.kv("half_size", s.cube_half_size); break;
        case particle::ShapeKind::Sphere: j.kv("radius", s.sphere_radius).kv("surface", s.sphere_surface); break;
        case particle::ShapeKind::Cylinder:
            j.kv("radius", s.cylinder_radius).kv("surface", s.cylinder_surface).kv("height", s.cylinder_height);
            break;
        default: break;
    }
    j.end_object();
}

void dump_properties(JsonWriter& j, const particle::Emitter& e) {
    j.begin_object();
    MiniChunkCursor mc(e.properties);
    while (auto m = mc.next()) {
        const auto* info = particle::emitter_field(m->id);
        if (!info) {
            char k[16];
            std::snprintf(k, sizeof k, "unknown_%02X", m->id);
            j.kv(k, static_cast<unsigned>(m->data.size()));
            continue;
        }
        j.key(info->name);
        if (g_full) j.begin_object().kv("id", static_cast<unsigned>(m->id)).kv("confidence", particle::to_string(info->confidence)).key("value");
        switch (info->type) {
            case particle::FieldType::Bool: j.value(m->data[0] != 0); break;
            case particle::FieldType::U32: j.value(m->as_u32()); break;
            case particle::FieldType::F32: j.value(m->as_float()); break;
            case particle::FieldType::Vec3: { auto v = m->as_vec3(); j.floats({v.x, v.y, v.z}); break; }
            case particle::FieldType::Vec4: { auto v = m->as_vec4(); j.floats({v.x, v.y, v.z, v.w}); break; }
        }
        if (g_full) j.end_object();
    }
    j.end_object();
}

void dump_particles(JsonWriter& j, const particle::ParticleSystem& ps) {
    j.kv("type", "alo-particle-system");
    j.kv("name", ps.name).kv("editor_id", ps.editor_id).kv("leave_particles", static_cast<unsigned>(ps.leave_particles));
    j.key("emitters").begin_array();
    for (const auto& e : ps.emitters) {
        j.begin_object();
        j.kv("name", e.name).kv("color_texture", e.color_texture).kv("bump_texture", e.bump_texture);
        j.kv("property_block_bytes", e.properties.size());
        j.key("properties");
        dump_properties(j, e);
        if (e.spawn_on_death != 0xFFFFFFFFu) j.kv("spawn_on_death", e.spawn_on_death);
        if (e.spawn_during_life != 0xFFFFFFFFu) j.kv("spawn_during_life", e.spawn_during_life);
        static const char* shape_names[] = {"velocity", "lifetime", "position"};
        j.key("shapes").begin_object();
        for (std::size_t i = 0; i < e.shapes.size() && i < 3; ++i) {
            j.key(shape_names[i]);
            dump_shape(j, e.shapes[i]);
        }
        j.end_object();
        j.key("tracks").begin_object();
        for (std::size_t ti = 0; ti < e.tracks.size(); ++ti) {
            const auto& t = e.tracks[ti];
            j.key(particle::track_name(ti));
            j.begin_object().kv("start", t.start).kv("end", t.end).kv("interpolation", t.interpolation)
                .kv("keys", t.keys.size());
            if (g_full) {
                j.key("key_data").begin_array();
                for (const auto& k : t.keys) j.floats({k.value, k.time});  // file order (SPEC 3.3)
                j.end_array();
            }
            j.end_object();
        }
        j.end_object();
        j.end_object();
    }
    j.end_array();
}

void dump_anim(JsonWriter& j, const ala::Animation& a) {
    j.kv("type", "ala-animation");
    j.kv("layout", a.layout == ala::Layout::Block ? "block" : "per-bone");
    j.kv("frames", a.num_frames).kv("fps", a.fps).kv("duration_s", a.duration()).kv("bones", a.num_bones);
    if (a.layout == ala::Layout::Block)
        j.kv("rotation_block", a.rotation_block).kv("translation_block", a.translation_block).kv("scale_block", a.scale_block);
    std::size_t nrot = 0, ntr = 0, nvis = 0;
    j.key("bone_tracks").begin_array();
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        const auto& b = a.bones[i];
        const bool r = a.is_rotation_animated(i), t = a.is_translation_animated(i);
        nrot += r; ntr += t; nvis += !b.visibility.empty();
        if (i >= cap(a.bones.size())) continue;
        j.begin_object().kv("name", b.name).kv("bone_index", b.bone_index);
        j.kv("rotation_animated", r).kv("translation_animated", t);
        if (!b.visibility.empty()) j.kv("visibility_track", true);
        if (!b.step_frames.empty()) j.kv("step_track", true);
        const auto t0 = a.translation(i, 0);
        const auto q0 = a.rotation(i, 0);
        j.key("frame0_translation").floats({t0.x, t0.y, t0.z});
        j.key("frame0_rotation").floats({q0.x, q0.y, q0.z, q0.w});
        j.end_object();
    }
    j.end_array();
    j.key("stats").begin_object().kv("rotation_tracks", nrot).kv("translation_tracks", ntr)
        .kv("visibility_tracks", nvis).end_object();
}

void dump_mtd(JsonWriter& j, const mtd::Atlas& a) {
    j.kv("type", "mtd-atlas").kv("entries", a.size());
    j.key("items").begin_array();
    for (std::size_t i = 0; i < cap(a.size()); ++i)
        j.begin_object().kv("name", a.names[i]).kv("x", a.x[i]).kv("y", a.y[i]).kv("w", a.width[i])
            .kv("h", a.height[i]).kv("alpha", static_cast<unsigned>(a.alpha[i])).end_object();
    j.end_array();
}

void dump_dat(JsonWriter& j, const dat::StringTable& t) {
    j.kv("type", "dat-string-table").kv("entries", t.size()).kv("sorted_by_crc", t.sorted_by_crc)
        .kv("crc_mismatches", t.crc_mismatches());
    j.key("items").begin_array();
    for (std::size_t i = 0; i < cap(t.size()); ++i) {
        char crc[16];
        std::snprintf(crc, sizeof crc, "%08X", t.crcs[i]);
        j.begin_object().kv("crc", crc).kv("key", t.keys[i]).kv("value", dat::to_utf8(t.values[i])).end_object();
    }
    j.end_array();
}

void dump_minifile(JsonWriter& j, const minifile::File& f, bool tem) {
    j.kv("type", tem ? "tem-terrain-materials" : "tee-environment-set").kv("records", f.records.size());
    j.key("items").begin_array();
    for (std::size_t i = 0; i < cap(f.records.size()); ++i) {
        const auto& r = f.records[i];
        j.begin_object().kv("chunk", r.chunk_id);
        if (tem && r.chunk_id == 3) j.kv("slot", i);
        // Raw minis only: the field meanings are the engine's (engine/terrain ted_read.cpp).
        j.key("strings").begin_array();
        for (const auto& str : r.strings()) j.value(str);
        j.end_array();
        j.key("mini_ids").begin_array();
        for (auto id : r.mini_ids) j.value(static_cast<unsigned>(id));
        j.end_array();
        j.end_object();
    }
    j.end_array();
}

std::string lower_ext(const std::filesystem::path& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--full") g_full = true;
        else if (a == "--max" && i + 1 < argc) g_max = std::stoul(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: alodump [--full] [--max N] <file.alo|.ala|.mtd|.dat|.tem|.tee>\n";
            return 0;
        } else file = a;
    }
    if (file.empty()) {
        std::cerr << "usage: alodump [--full] [--max N] <file>\n";
        return 2;
    }
    try {
        auto buf = read_file(file);
        JsonWriter j(std::cout);
        j.begin_object();
        j.kv("file", file.filename().string()).kv("bytes", buf.size());
        const auto ext = lower_ext(file);
        if (ext == ".alo") {
            auto f = alo::parse(buf);
            if (f.kind == alo::AloFile::Kind::Model) dump_model(j, f.model);
            else dump_particles(j, f.particles);
        } else if (ext == ".ala") {
            dump_anim(j, ala::parse(buf));
        } else if (ext == ".mtd") {
            dump_mtd(j, mtd::parse(buf));
        } else if (ext == ".dat") {
            dump_dat(j, dat::parse(buf));
        } else if (ext == ".tem") {
            dump_minifile(j, minifile::parse(buf), true);
        } else if (ext == ".tee") {
            dump_minifile(j, minifile::parse(buf), false);
        } else {
            throw std::runtime_error("unknown extension " + ext);
        }
        j.end_object();
        std::cout << "\n";
    } catch (const std::exception& e) {
        std::cerr << "alodump: " << file.string() << ": " << e.what() << "\n";
        return 1;
    }
    return 0;
}

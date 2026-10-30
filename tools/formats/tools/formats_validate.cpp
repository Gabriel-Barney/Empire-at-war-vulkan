// formats_validate: parse many Alamo asset files, run semantic sanity checks, and emit a Markdown
// report with per-file pass/fail and chunk-ID statistics (unknown chunk/mini-chunk IDs flagged).
//
//   formats_validate [--title T] [--csv out.csv] [--no-files] <files...>
#include "alamo/formats/ala.hpp"
#include "alamo/formats/alo.hpp"
#include "alamo/formats/minichunk_file.hpp"
#include "alamo/formats/text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace alamo;

namespace {

struct Result {
    std::string name, kind;
    std::size_t bytes = 0;
    bool ok = false;
    std::string error;
    std::string summary;
    std::vector<std::string> warnings;
};

struct Stats {
    std::map<std::string, std::size_t> chunk_paths;                    // path -> count
    std::map<std::string, std::map<int, std::size_t>> mini_ids;        // path -> id -> count
    std::map<std::string, std::set<std::string>> unknown_examples;     // path -> files
};

// Chunks whose payload is a mini-chunk list, with the set of mini IDs we decode.
const std::map<std::string, std::set<int>>& mini_catalogue() {
    static const std::map<std::string, std::set<int>> m = {
        {"600/601", {1, 4, 9}},
        {"600/602", {2, 3}},
        {"600/603", {5, 6, 7, 8}},
        {"400/10100/10102", {1, 2}}, {"400/10100/10103", {1, 2}}, {"400/10100/10104", {1, 2}},
        {"400/10100/10105", {1, 2}}, {"400/10100/10106", {1, 2}},
        {"400/10000/1200/1201", {0, 1, 2, 3}},
        {"1000/1001", {1, 2, 3, 0xB, 0xC, 0xD}},
        {"1000/1002/1003", {4, 5, 6, 7, 8, 9, 0xA, 0xE, 0xF, 0x10, 0x11}},
        {"900/800/700/1/0", {2, 3, 4}},
        {"900/800/700/1/1", {5}},
        {"900/800/700/36", {0x37, 0x39}},
        {"900/800/700/2", {}},  // checked against particle::emitter_fields() below
    };
    return m;
}
// Mini-chunk containers whose IDs are tallied but not interpreted (no "unknown" flag).
bool generic_mini_container(const std::string& p) { return p == "2/3" || p == "6"; }

void walk(ChunkCursor cur, const std::string& parent, Stats& st, const std::string& file) {
    while (auto c = cur.next()) {
        const std::string path = parent.empty() ? hex_id(c->id) : parent + "/" + hex_id(c->id);
        st.chunk_paths[path]++;
        if (!known_chunk_name(path)) st.unknown_examples[path].insert(file);
        if (c->has_children) {
            walk(c->children(), path, st, file);
        } else {
            auto it = mini_catalogue().find(path);
            if (it != mini_catalogue().end() || generic_mini_container(path)) {
                auto mc = c->minis();
                while (auto m = mc.next()) {
                    st.mini_ids[path][m->id]++;
                    const bool known = path == "900/800/700/2" ? particle::emitter_field(m->id) != nullptr
                                                               : (it == mini_catalogue().end() || it->second.count(m->id));
                    if (!known)
                        st.unknown_examples[path + " mini 0x" + hex_id(m->id)].insert(file);
                }
            }
        }
    }
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void check_model(const alo::Model& m, Result& r) {
    std::size_t verts = 0, tris = 0, bad_pos = 0, bad_nrm = 0, coll_trees = 0, coll_fail = 0, coll_checked = 0;
    for (const auto& mesh : m.meshes) {
        for (const auto& sm : mesh.submeshes) {
            verts += sm.vertices.size();
            tris += sm.indices.size() / 3;
            // Shadow-volume meshes legitimately contain zero normals on their degenerate fin vertices.
            const bool shadow = sm.material.shader.find("ShadowVolume") != std::string::npos;
            for (std::size_t i = 0; i < sm.vertices.size(); ++i) {
                if (!finite(sm.vertices.position[i])) ++bad_pos;
                if (shadow) continue;
                const auto& n = sm.vertices.normal[i];
                const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (!(l > 0.9f && l < 1.1f)) ++bad_nrm;
            }
            if (!sm.format.known) r.warnings.push_back("unknown vertex format " + sm.vertex_format);
            if (sm.collision) {
                ++coll_trees;
                const auto boxes = sm.collision->node_bounds();
                const auto& b = sm.collision->bounds;
                const Vec3 tol{(b.max.x - b.min.x) / 100 + 1e-3f, (b.max.y - b.min.y) / 100 + 1e-3f,
                               (b.max.z - b.min.z) / 100 + 1e-3f};
                for (std::size_t ni = 0; ni < sm.collision->nodes.size(); ++ni) {
                    const auto& node = sm.collision->nodes[ni];
                    if (!node.is_leaf()) continue;
                    for (std::size_t t = node.index; t < std::size_t(node.index) + node.count; ++t) {
                        const auto tri = sm.collision->triangles[t];
                        for (std::size_t k = 0; k < 3; ++k) {
                            const auto& p = sm.vertices.position[sm.indices[std::size_t{tri} * 3 + k]];
                            const auto& bb = boxes[ni];
                            ++coll_checked;
                            if (p.x < bb.min.x - tol.x || p.x > bb.max.x + tol.x || p.y < bb.min.y - tol.y ||
                                p.y > bb.max.y + tol.y || p.z < bb.min.z - tol.z || p.z > bb.max.z + tol.z)
                                ++coll_fail;
                        }
                    }
                }
            }
        }
    }
    if (bad_pos) r.warnings.push_back(std::to_string(bad_pos) + " non-finite positions");
    if (bad_nrm) r.warnings.push_back(std::to_string(bad_nrm) + " non-unit normals");
    if (coll_fail) r.warnings.push_back(std::to_string(coll_fail) + "/" + std::to_string(coll_checked) + " collision-tree vertices outside leaf box");
    if (!m.unknown.empty()) r.warnings.push_back(std::to_string(m.unknown.size()) + " unknown chunks");
    std::ostringstream s;
    s << m.skeleton.size() << " bones, " << m.meshes.size() << " meshes, " << verts << " verts, " << tris << " tris";
    if (!m.lights.empty()) s << ", " << m.lights.size() << " lights";
    if (!m.proxies.empty()) s << ", " << m.proxies.size() << " proxies";
    if (coll_trees) s << ", " << coll_trees << " coll-trees";
    r.summary = s.str();
}

void check_anim(const ala::Animation& a, Result& r) {
    std::size_t bad_q = 0;
    auto mag = [](const std::int16_t* q) {
        const double x = q[0] / 32767.0, y = q[1] / 32767.0, z = q[2] / 32767.0, w = q[3] / 32767.0;
        return std::sqrt(x * x + y * y + z * z + w * w);
    };
    if (a.layout == ala::Layout::Block) {
        for (const auto& b : a.bones) {
            if (b.rotation_index == 0xFFFF) continue;
            for (std::uint32_t f = 0; f < a.num_frames; ++f) {
                const double m = mag(&a.rotation_data[std::size_t(f) * a.rotation_block + b.rotation_index]);
                if (m < 0.95 || m > 1.05) ++bad_q;
            }
        }
    } else {
        for (const auto& b : a.bones)
            for (std::size_t i = 0; i + 4 <= b.rotation_data.size(); i += 4) {
                const double m = mag(&b.rotation_data[i]);
                if (m < 0.95 || m > 1.05) ++bad_q;
            }
    }
    if (bad_q) r.warnings.push_back(std::to_string(bad_q) + " non-unit quantised quaternions");
    std::size_t nr = 0, nt = 0;
    for (std::size_t i = 0; i < a.bones.size(); ++i) { nr += a.is_rotation_animated(i); nt += a.is_translation_animated(i); }
    std::ostringstream s;
    s << (a.layout == ala::Layout::Block ? "block" : "per-bone") << ", " << a.num_frames << " frames @ " << a.fps
      << " fps, " << a.num_bones << " bones (" << nr << " rot, " << nt << " trans)";
    r.summary = s.str();
}

std::string lower_ext(const std::filesystem::path& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

Result validate(const std::filesystem::path& p, Stats& st) {
    Result r;
    r.name = p.filename().string();
    const auto ext = lower_ext(p);
    try {
        auto buf = read_file(p);
        r.bytes = buf.size();
        if (ext == ".alo" || ext == ".ala" || ext == ".tem" || ext == ".tee") walk(top_level_chunks(buf), "", st, r.name);
        if (ext == ".alo") {
            auto f = alo::parse(buf);
            if (f.kind == alo::AloFile::Kind::Model) {
                r.kind = "ALO model";
                check_model(f.model, r);
            } else {
                r.kind = "ALO particles";
                std::size_t keys = 0;
                for (const auto& e : f.particles.emitters) for (const auto& t : e.tracks) keys += t.keys.size();
                r.summary = std::to_string(f.particles.emitters.size()) + " emitters, " + std::to_string(keys) + " track keys";
                for (std::size_t ei = 0; ei < f.particles.emitters.size(); ++ei) {
                    const auto& e = f.particles.emitters[ei];
                    if (e.tracks.size() != 7) r.warnings.push_back("emitter with " + std::to_string(e.tracks.size()) + " tracks");
                    if (e.shapes.size() != 3) r.warnings.push_back("emitter with " + std::to_string(e.shapes.size()) + " spawn shapes");
                    for (const auto& s : e.shapes)
                        if (static_cast<std::uint32_t>(s.kind) > 5) r.warnings.push_back("unknown spawn shape kind");
                    if (!e.desc.unknown.empty()) r.warnings.push_back("unknown emitter property IDs");
                    if (e.desc.index != ei) r.warnings.push_back("emitter index field != position");
                    if (e.desc.texture_cells == 0) r.warnings.push_back("texture_cells == 0");
                    for (auto link : {e.spawn_on_death, e.spawn_during_life})
                        if (link != 0xFFFFFFFFu && link >= f.particles.emitters.size())
                            r.warnings.push_back("dangling emitter link " + std::to_string(link) + " (ignored by runtime)");
                }
            }
        } else if (ext == ".ala") {
            r.kind = "ALA";
            check_anim(ala::parse(buf), r);
        } else if (ext == ".mtd") {
            r.kind = "MTD";
            auto a = mtd::parse(buf);
            r.summary = std::to_string(a.size()) + " atlas entries";
        } else if (ext == ".dat") {
            r.kind = "DAT";
            auto t = dat::parse(buf);
            r.summary = std::to_string(t.size()) + " strings" + (t.sorted_by_crc ? ", sorted" : ", unsorted") + ", " +
                        std::to_string(t.crc_mismatches()) + " crc!=crc32(key)";
            // spot-check lookup round trip on every key
            std::size_t miss = 0;
            for (std::size_t i = 0; i < t.size(); ++i)
                if (!t.find(t.keys[i])) ++miss;
            if (miss) r.warnings.push_back(std::to_string(miss) + " keys not found by lookup");
        } else if (ext == ".tem" || ext == ".tee") {
            r.kind = ext == ".tem" ? "TEM" : "TEE";
            auto f = minifile::parse(buf);
            r.summary = std::to_string(f.records.size()) + " records";
        } else {
            r.kind = "?";
            throw std::runtime_error("unsupported extension");
        }
        r.ok = true;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    return r;
}

std::string md_escape(std::string s) {
    std::string o;
    for (char c : s) {
        if (c == '|') o += "\\|";
        else o += c;
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    std::string title = "Validation";
    std::filesystem::path csv;
    bool files_table = true;
    std::vector<std::filesystem::path> files;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--title" && i + 1 < argc) title = argv[++i];
        else if (a == "--csv" && i + 1 < argc) csv = argv[++i];
        else if (a == "--no-files") files_table = false;
        else files.emplace_back(a);
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
    Stats st;
    std::vector<Result> results;
    for (const auto& f : files) results.push_back(validate(f, st));

    std::map<std::string, std::array<std::size_t, 3>> by_kind;  // pass, fail, warn
    for (const auto& r : results) {
        auto& k = by_kind[r.kind.empty() ? "?" : r.kind];
        k[r.ok ? 0 : 1]++;
        if (r.ok && !r.warnings.empty()) k[2]++;
    }
    auto& o = std::cout;
    o << "## " << title << "\n\n";
    std::size_t pass = 0;
    for (auto& r : results) pass += r.ok;
    o << "**" << pass << " / " << results.size() << " files parsed without error.**\n\n";
    o << "| Kind | Pass | Fail | Pass with warnings |\n|---|---:|---:|---:|\n";
    for (const auto& [k, v] : by_kind) o << "| " << k << " | " << v[0] << " | " << v[1] << " | " << v[2] << " |\n";
    o << "\n";

    std::vector<const Result*> problems;
    for (const auto& r : results)
        if (!r.ok || !r.warnings.empty()) problems.push_back(&r);
    o << "### Failures and warnings\n\n";
    if (problems.empty()) o << "None.\n\n";
    else {
        o << "| File | Result | Detail |\n|---|---|---|\n";
        for (auto* r : problems) {
            std::string d = r->ok ? "" : r->error;
            for (const auto& w : r->warnings) d += (d.empty() ? "" : "; ") + w;
            o << "| " << md_escape(r->name) << " | " << (r->ok ? "PASS (warn)" : "**FAIL**") << " | " << md_escape(d) << " |\n";
        }
        o << "\n";
    }

    if (files_table) {
        o << "### Per-file results\n\n| File | Kind | Bytes | Result | Summary |\n|---|---|---:|---|---|\n";
        for (const auto& r : results)
            o << "| " << md_escape(r.name) << " | " << r.kind << " | " << r.bytes << " | " << (r.ok ? "PASS" : "**FAIL**")
              << " | " << md_escape(r.ok ? r.summary : r.error) << " |\n";
        o << "\n";
    }

    o << "### Chunk paths encountered\n\n| Path | Count | Meaning |\n|---|---:|---|\n";
    for (const auto& [p, n] : st.chunk_paths) {
        const char* name = known_chunk_name(p);
        o << "| `" << p << "` | " << n << " | " << (name ? name : "**UNKNOWN**") << " |\n";
    }
    o << "\n### Mini-chunk IDs encountered\n\n| Container | Mini IDs (count) |\n|---|---|\n";
    for (const auto& [p, ids] : st.mini_ids) {
        o << "| `" << p << "` | ";
        bool first = true;
        for (const auto& [id, n] : ids) {
            o << (first ? "" : ", ") << "0x" << hex_id(static_cast<std::uint32_t>(id)) << " (" << n << ")";
            first = false;
        }
        o << " |\n";
    }
    o << "\n### Unknown chunk / mini-chunk IDs\n\n";
    if (st.unknown_examples.empty()) o << "None: every chunk path and every mini-chunk ID in a decoded container is in the catalogue.\n\n";
    else {
        o << "| Path | Files (up to 5) |\n|---|---|\n";
        for (const auto& [p, fs] : st.unknown_examples) {
            o << "| `" << p << "` | ";
            std::size_t i = 0;
            for (const auto& f : fs) { if (i++ == 5) { o << " ..."; break; } o << (i > 1 ? ", " : "") << f; }
            o << " |\n";
        }
        o << "\n";
    }

    if (!csv.empty()) {
        std::ofstream c(csv);
        c << "file,kind,bytes,result,detail\n";
        for (const auto& r : results) {
            std::string d = r.ok ? r.summary : r.error;
            for (const auto& w : r.warnings) d += "; WARN " + w;
            std::replace(d.begin(), d.end(), '"', '\'');
            c << r.name << "," << r.kind << "," << r.bytes << "," << (r.ok ? "PASS" : "FAIL") << ",\"" << d << "\"\n";
        }
    }
    return pass == results.size() ? 0 : 1;
}

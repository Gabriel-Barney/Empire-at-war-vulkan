// Minimal JSON + glTF reader. See gltf.hpp.
#include "gltf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace alamo::reftest {

const Json* Json::get(const std::string& k) const {
    for (const auto& kv : obj)
        if (kv.first == k) return &kv.second;
    return nullptr;
}
double Json::number(const std::string& k, double def) const {
    const Json* j = get(k);
    return (j && j->type == Type::Number) ? j->num : def;
}

namespace {
struct Parser {
    const std::string& s;
    size_t p = 0;
    int depth = 0;
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p;
    }
    bool value(Json& v) {
        if (++depth > 200) return false;
        ws();
        if (p >= s.size()) return false;
        const char c = s[p];
        bool ok = false;
        if (c == '{') {
            v.type = Json::Type::Object;
            ++p;
            ws();
            if (p < s.size() && s[p] == '}') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    Json key;
                    ws();
                    if (!string(key.str)) break;
                    ws();
                    if (p >= s.size() || s[p] != ':') break;
                    ++p;
                    Json val;
                    if (!value(val)) break;
                    v.obj.emplace_back(std::move(key.str), std::move(val));
                    ws();
                    if (p < s.size() && s[p] == ',') {
                        ++p;
                        continue;
                    }
                    if (p < s.size() && s[p] == '}') {
                        ++p;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '[') {
            v.type = Json::Type::Array;
            ++p;
            ws();
            if (p < s.size() && s[p] == ']') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    Json val;
                    if (!value(val)) break;
                    v.arr.push_back(std::move(val));
                    ws();
                    if (p < s.size() && s[p] == ',') {
                        ++p;
                        continue;
                    }
                    if (p < s.size() && s[p] == ']') {
                        ++p;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '"') {
            v.type = Json::Type::String;
            ok = string(v.str);
        } else if (s.compare(p, 4, "true") == 0) {
            v.type = Json::Type::Bool;
            v.b = true;
            p += 4;
            ok = true;
        } else if (s.compare(p, 5, "false") == 0) {
            v.type = Json::Type::Bool;
            p += 5;
            ok = true;
        } else if (s.compare(p, 4, "null") == 0) {
            p += 4;
            ok = true;
        } else {
            char* end = nullptr;
            v.type = Json::Type::Number;
            v.num = std::strtod(s.c_str() + p, &end);
            const size_t np = static_cast<size_t>(end - s.c_str());
            ok = np > p;
            p = np;
        }
        --depth;
        return ok;
    }
    bool string(std::string& out) {
        if (p >= s.size() || s[p] != '"') return false;
        ++p;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                ++p;
                const char e = s[p];
                if (e == 'u') {
                    p += 4;  // names in the exports are ASCII; skip the code unit
                    out += '?';
                } else {
                    out += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
                }
                ++p;
            } else {
                out += s[p++];
            }
        }
        if (p >= s.size()) return false;
        ++p;
        return true;
    }
};

Dmat4 trs(const Json& node) {
    Dmat4 m;
    if (const Json* mat = node.get("matrix"); mat && mat->arr.size() == 16) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) m.m[r][c] = mat->arr[static_cast<size_t>(c * 4 + r)].num;
        return m;
    }
    double t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    if (const Json* j = node.get("translation"))
        for (size_t i = 0; i < 3 && i < j->arr.size(); ++i) t[i] = j->arr[i].num;
    if (const Json* j = node.get("rotation"))
        for (size_t i = 0; i < 4 && i < j->arr.size(); ++i) q[i] = j->arr[i].num;
    if (const Json* j = node.get("scale"))
        for (size_t i = 0; i < 3 && i < j->arr.size(); ++i) s[i] = j->arr[i].num;
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m.m[r][c] = R[r][c] * s[c];
        m.m[r][3] = t[r];
    }
    return m;
}
}  // namespace

bool parse_json(const std::string& text, Json& out, std::string* err) {
    Parser ps{text};
    out = Json{};
    if (!ps.value(out)) {
        if (err) *err = "JSON parse error at " + std::to_string(ps.p);
        return false;
    }
    return true;
}

Dmat4 Dmat4::operator*(const Dmat4& b) const {
    Dmat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
void Dmat4::apply(const double p[3], double out[3]) const {
    for (int r = 0; r < 3; ++r) out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
}

bool Gltf::load(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!parse_json(text, json, err)) return false;
    const Json* buffers = json.get("buffers");
    if (!buffers || buffers->arr.empty()) return false;
    const Json* uri = buffers->arr[0].get("uri");
    if (!uri) return false;
    const std::filesystem::path binp = std::filesystem::path(path).parent_path() / uri->str;
    std::ifstream bf(binp, std::ios::binary);
    if (!bf) {
        if (err) *err = "cannot open " + binp.string();
        return false;
    }
    bin.assign(std::istreambuf_iterator<char>(bf), std::istreambuf_iterator<char>());

    auto accessor = [&](int index, std::vector<double>& out, int& comps) -> bool {
        const Json* accs = json.get("accessors");
        const Json* views = json.get("bufferViews");
        if (!accs || !views || index < 0 || static_cast<size_t>(index) >= accs->arr.size()) return false;
        const Json& a = accs->arr[static_cast<size_t>(index)];
        const int view = a.integer("bufferView");
        if (view < 0 || static_cast<size_t>(view) >= views->arr.size()) return false;
        const Json& v = views->arr[static_cast<size_t>(view)];
        const size_t off = static_cast<size_t>(v.number("byteOffset", 0) + a.number("byteOffset", 0));
        const int ct = a.integer("componentType");
        const size_t count = static_cast<size_t>(a.number("count", 0));
        const Json* type = a.get("type");
        const std::string t = type ? type->str : "";
        comps = t == "SCALAR" ? 1 : t == "VEC2" ? 2 : t == "VEC3" ? 3 : t == "VEC4" ? 4 : t == "MAT4" ? 16 : 0;
        const size_t csize = ct == 5126 ? 4 : ct == 5123 ? 2 : ct == 5125 ? 4 : ct == 5121 ? 1 : 0;
        const size_t nc = static_cast<size_t>(comps);
        if (!nc || !csize || off + count * nc * csize > bin.size()) return false;
        out.resize(count * nc);
        for (size_t i = 0; i < count * nc; ++i) {
            const uint8_t* q = &bin[off + i * csize];
            if (ct == 5126) {
                float x;
                std::memcpy(&x, q, 4);
                out[i] = x;
            } else if (ct == 5123) {
                out[i] = q[0] | (q[1] << 8);
            } else if (ct == 5125) {
                uint32_t x;
                std::memcpy(&x, q, 4);
                out[i] = x;
            } else {
                out[i] = q[0];
            }
        }
        return true;
    };

    const Json* nodes = json.get("nodes");
    const Json* meshes = json.get("meshes");
    if (!nodes || !meshes) return false;
    if (const Json* skins = json.get("skins"); skins && !skins->arr.empty()) {
        const Json& sk = skins->arr[0];
        if (const Json* j = sk.get("joints"))
            for (const auto& e : j->arr) skin_joints.push_back(static_cast<int>(e.num));
        std::vector<double> ibm;
        int comps = 0;
        if (accessor(sk.integer("inverseBindMatrices"), ibm, comps) && comps == 16) {
            for (size_t k = 0; k * 16 < ibm.size(); ++k) {
                Dmat4 m;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r) m.m[r][c] = ibm[k * 16 + static_cast<size_t>(c * 4 + r)];
                inverse_bind.push_back(m);
            }
        }
    }
    for (size_t n = 0; n < nodes->arr.size(); ++n) {
        const int mi = nodes->arr[n].integer("mesh");
        if (mi < 0 || static_cast<size_t>(mi) >= meshes->arr.size()) continue;
        const bool has_skin = nodes->arr[n].get("skin") != nullptr;
        const Json* prims = meshes->arr[static_cast<size_t>(mi)].get("primitives");
        if (!prims) continue;
        for (const Json& p : prims->arr) {
            GltfPrimitive gp;
            gp.node = static_cast<int>(n);
            const Json* attrs = p.get("attributes");
            if (!attrs) continue;
            std::vector<double> v;
            int comps = 0;
            if (!accessor(attrs->integer("POSITION"), v, comps) || comps != 3) return false;
            gp.positions.resize(v.size());
            for (size_t i = 0; i < v.size(); ++i) gp.positions[i] = static_cast<float>(v[i]);
            if (has_skin && attrs->get("JOINTS_0")) {
                gp.skinned = true;
                if (!accessor(attrs->integer("JOINTS_0"), v, comps) || comps != 4) return false;
                for (size_t i = 0; i < v.size(); i += 4) gp.joints.push_back(static_cast<uint16_t>(v[i]));
            }
            if (!accessor(p.integer("indices"), v, comps)) return false;
            for (double x : v) gp.indices.push_back(static_cast<uint32_t>(x));
            primitives.push_back(std::move(gp));
        }
    }
    if (const Json* anims = json.get("animations"); anims && !anims->arr.empty()) {
        if (const Json* nm = anims->arr[0].get("name")) animation_name = nm->str;
        if (const Json* smps = anims->arr[0].get("samplers"); smps && !smps->arr.empty()) {
            std::vector<double> t;
            int comps = 0;
            if (accessor(smps->arr[0].integer("input"), t, comps)) animation_frames = static_cast<int>(t.size());
        }
    }
    return true;
}

std::vector<Dmat4> Gltf::globals(int frame, bool skip_root_rotation) const {
    const Json& nodes = *json.get("nodes");
    std::vector<Json> nd = nodes.arr;  // copy: animation overrides TRS
    if (frame >= 0) {
        const Json* anims = json.get("animations");
        if (anims && !anims->arr.empty()) {
            const Json& a = anims->arr[0];
            const Json* chans = a.get("channels");
            const Json* smps = a.get("samplers");
            const Json* accs = json.get("accessors");
            const Json* views = json.get("bufferViews");
            for (const Json& ch : chans->arr) {
                const Json& smp = smps->arr[static_cast<size_t>(ch.integer("sampler"))];
                const Json& acc = accs->arr[static_cast<size_t>(smp.integer("output"))];
                const Json& view = views->arr[static_cast<size_t>(acc.integer("bufferView"))];
                const Json* tgt = ch.get("target");
                const std::string path = tgt->get("path")->str;
                const int node = tgt->integer("node");
                const size_t comps = path == "rotation" ? 4 : 3;
                const size_t count = static_cast<size_t>(acc.number("count"));
                const size_t f = std::min<size_t>(static_cast<size_t>(frame), count - 1);
                const size_t off = static_cast<size_t>(view.number("byteOffset", 0) + acc.number("byteOffset", 0)) + f * comps * 4;
                Json arr;
                arr.type = Json::Type::Array;
                for (size_t k = 0; k < comps; ++k) {
                    float x;
                    std::memcpy(&x, &bin[off + k * 4], 4);
                    Json e;
                    e.type = Json::Type::Number;
                    e.num = x;
                    arr.arr.push_back(e);
                }
                Json& n = nd[static_cast<size_t>(node)];
                bool found = false;
                for (auto& kv : n.obj)
                    if (kv.first == path) {
                        kv.second = arr;
                        found = true;
                    }
                if (!found) n.obj.emplace_back(path, arr);
                for (size_t k = 0; k < n.obj.size(); ++k)
                    if (n.obj[k].first == "matrix") n.obj.erase(n.obj.begin() + static_cast<std::ptrdiff_t>(k));
            }
        }
    }
    std::vector<Dmat4> g(nd.size());
    std::vector<int> parent(nd.size(), -1);
    for (size_t i = 0; i < nd.size(); ++i)
        if (const Json* c = nd[i].get("children"))
            for (const auto& e : c->arr) parent[static_cast<size_t>(e.num)] = static_cast<int>(i);
    std::vector<int> done(nd.size(), 0);
    // Parents can come after children in index order: resolve recursively.
    std::function<void(size_t)> visit = [&](size_t i) {
        if (done[i]) return;
        Dmat4 local = trs(nd[i]);
        if (parent[i] < 0 && skip_root_rotation) local = Dmat4{};
        if (parent[i] >= 0) {
            visit(static_cast<size_t>(parent[i]));
            g[i] = g[static_cast<size_t>(parent[i])] * local;
        } else {
            g[i] = local;
        }
        done[i] = 1;
    };
    for (size_t i = 0; i < nd.size(); ++i) visit(i);
    return g;
}

std::vector<std::vector<double>> Gltf::posed_positions(int frame, bool skip_root_rotation) const {
    const std::vector<Dmat4> g = globals(frame, skip_root_rotation);
    std::vector<Dmat4> jm(skin_joints.size());
    for (size_t k = 0; k < skin_joints.size() && k < inverse_bind.size(); ++k)
        jm[k] = g[static_cast<size_t>(skin_joints[k])] * inverse_bind[k];
    std::vector<std::vector<double>> out;
    for (const auto& p : primitives) {
        std::vector<double> v(p.positions.size());
        for (size_t i = 0; i * 3 < p.positions.size(); ++i) {
            const double in[3] = {p.positions[i * 3], p.positions[i * 3 + 1], p.positions[i * 3 + 2]};
            const Dmat4& m = p.skinned ? jm[p.joints[i] < jm.size() ? p.joints[i] : 0] : g[static_cast<size_t>(p.node)];
            m.apply(in, &v[i * 3]);
        }
        out.push_back(std::move(v));
    }
    return out;
}

}  // namespace alamo::reftest

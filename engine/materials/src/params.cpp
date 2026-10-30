// Effect parameter values and material records. See params.hpp.
#include "alamo/materials/params.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "alamo/core/hash.hpp"
#include "fx2_reader.hpp"

namespace alamo::materials {

void matrix_rows_from_d3dx(const float* m, float4* out, int rows) {
    for (int r = 0; r < rows && r < 4; ++r)
        out[r] = float4{m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r]};
}

namespace {
bool is_numeric(fx::ParamType t) {
    return t == fx::ParamType::Bool || t == fx::ParamType::Int || t == fx::ParamType::Float;
}
std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}
}  // namespace

size_t param_float_count(const fx::Parameter& p) {
    if (p.type.cls == fx::ParamClass::Object || p.type.cls == fx::ParamClass::Struct) return 0;
    if (!is_numeric(p.type.type)) return 0;
    return size_t(p.type.rows) * p.type.columns * std::max<uint32_t>(1, p.type.elements);
}

void param_value(const fx::Parameter& p, const ParamLookup& lookup, std::span<float> out) {
    std::fill(out.begin(), out.end(), 0.0f);
    if (lookup && lookup(p, out)) return;
    const auto& w = p.value.words;
    for (size_t i = 0; i < out.size() && i < w.size(); ++i) {
        switch (p.type.type) {
        case fx::ParamType::Float: out[i] = std::bit_cast<float>(w[i]); break;
        case fx::ParamType::Int: out[i] = static_cast<float>(static_cast<int32_t>(w[i])); break;
        case fx::ParamType::Bool: out[i] = w[i] ? 1.0f : 0.0f; break;
        default: break;
        }
    }
}

std::vector<float> param_value(const fx::Parameter& p, const ParamLookup& lookup) {
    std::vector<float> v(param_float_count(p));
    param_value(p, lookup, v);
    return v;
}

void ParamStore::set(std::string_view name, std::vector<float> values) {
    by_name_[upper(name)] = std::move(values);
}
void ParamStore::set_semantic(std::string_view semantic, std::vector<float> values) {
    by_semantic_[upper(semantic)] = std::move(values);
}
bool ParamStore::get(const fx::Parameter& p, std::span<float> out) const {
    const std::vector<float>* v = nullptr;
    if (auto it = by_name_.find(upper(p.type.name)); it != by_name_.end()) v = &it->second;
    else if (!p.type.semantic.empty()) {
        if (auto is = by_semantic_.find(upper(p.type.semantic)); is != by_semantic_.end()) v = &is->second;
    }
    if (!v) return false;
    std::fill(out.begin(), out.end(), 0.0f);
    std::copy_n(v->begin(), std::min(v->size(), out.size()), out.begin());
    return true;
}
ParamLookup ParamStore::lookup() const {
    return [this](const fx::Parameter& p, std::span<float> out) { return get(p, out); };
}

bool is_engine_parameter(const fx::Parameter& p) {
    if (p.type.name.rfind("_ALAMO_", 0) == 0) return true;
    static const char* const kSemantics[] = {
        "CLOUD_TEXTURE", "CLOUD_TEX_U", "CLOUD_TEX_V", "DIR_LIGHT_DIFFUSE_0", "DIR_LIGHT_OBJ_VEC_0",
        "DIR_LIGHT_SPECULAR_0", "DIR_LIGHT_VEC_0", "DISTANCE_FADE_VALS", "EYE_OBJ_POSITION",
        "EYE_POSITION", "FOG_VALS", "FOW_TEXTURE", "FOW_TEX_U", "FOW_TEX_V", "GLOBAL_AMBIENT",
        "LIGHT_SCALE", "PROJECTION", "RESOLUTION_CONSTANTS", "SKY_CUBE_TEXTURE", "SPH_LIGHT_ALL",
        "SPH_LIGHT_FILL", "TIME", "VIEW", "VIEWINVERSE", "VIEWPROJECTION", "WIND_BEND_VECTOR",
        "WIND_GRASS_PARAMS", "WORLD", "WORLDINVERSE", "WORLDVIEW", "WORLDVIEWINVERSE",
        "WORLDVIEWPROJECTION", "SKINMATRIXARRAY", "DEPTH_SCALE", "REFLECTION",
        "SHADOW_EXTRUSION_DISTANCE", "DIFFUSEMAP", "HEAT_TEXTURE", "MATERIALDIFFUSE",
        "MATERIALSPECULAR", "POWER", "REFRACTION"};
    if (p.type.semantic.empty()) return false;
    const std::string s = upper(p.type.semantic);
    for (const char* k : kSemantics)
        if (s == k) return true;
    return false;
}

bool is_material_parameter(const fx::Parameter& p) {
    return param_float_count(p) > 0 && !is_engine_parameter(p);
}

GenericLayout build_generic_layout(const fx::Effect& e) {
    GenericLayout L;
    uint32_t off = 0;  // bytes
    for (const auto& p : e.parameters) {
        if (!is_material_parameter(p)) continue;
        GenericField f;
        f.name = p.type.name;
        f.columns = p.type.columns;
        f.rows = p.type.rows;
        f.elements = p.type.elements;
        f.floats = static_cast<uint32_t>(param_float_count(p));
        const bool aggregate = p.type.elements > 0 || p.type.rows > 1;
        if (aggregate) {
            off = (off + 15u) & ~15u;
            const uint32_t per = std::max<uint32_t>(1, p.type.rows) * std::max<uint32_t>(1, p.type.elements);
            f.registers = per;
            f.offset = off;
            off += per * 16u;
        } else {
            const uint32_t bytes = 4u * p.type.columns;
            if ((off % 16u) + bytes > 16u) off = (off + 15u) & ~15u;  // no straddling
            f.offset = off;
            f.registers = 1;
            off += bytes;
        }
        L.fields.push_back(std::move(f));
    }
    L.size = (off + 15u) & ~15u;
    return L;
}

void fill_generic_record(const fx::Effect& e, const GenericLayout& layout, const ParamLookup& lookup,
                         std::span<float> out) {
    std::fill(out.begin(), out.end(), 0.0f);
    for (const auto& f : layout.fields) {
        const fx::Parameter* p = e.findParameter(f.name);
        if (!p) continue;
        std::vector<float> v = param_value(*p, lookup);
        const size_t base = f.offset / 4u;
        if (f.registers > 1 || f.elements > 0 || f.rows > 1) {
            // one register per row (per element for vectors/scalars)
            const uint32_t cols = std::max<uint32_t>(1, f.columns);
            const size_t nrows = v.size() / cols;
            for (size_t r = 0; r < nrows; ++r)
                for (uint32_t c = 0; c < cols; ++c)
                    if (base + r * 4 + c < out.size()) out[base + r * 4 + c] = v[r * cols + c];
        } else {
            for (size_t i = 0; i < v.size() && base + i < out.size(); ++i) out[base + i] = v[i];
        }
    }
}

namespace {
struct Field { const char* name; int vec; int comp; int count; };
// vec: 0 diffuse 1 emissive 2 specular 3 colorization 4 uv_offset_scroll
constexpr Field kMeshFields[] = {
    {"Diffuse", 0, 0, 4},      {"DebugColor", 0, 0, 4}, {"Color", 0, 0, 4},
    {"Emissive", 1, 0, 3},     {"Shininess", 1, 3, 1},  {"AtmospherePower", 1, 3, 1},
    {"Specular", 2, 0, 3},     {"BendScale", 2, 3, 1},
    {"Colorization", 3, 0, 4}, {"Atmosphere", 3, 0, 4},
    {"UVOffset", 4, 0, 2},     {"UVScrollRate", 4, 2, 2},
    {"CityColor", 4, 0, 3},    {"CloudScrollRate", 4, 3, 1},
};
}  // namespace

MeshMaterial build_mesh_material(const fx::Effect& e, const ParamLookup& lookup) {
    MeshMaterial m;
    m.diffuse = {1, 1, 1, 1};
    m.specular = {1, 1, 1, 0};
    float4* vecs[5] = {&m.diffuse, &m.emissive, &m.specular, &m.colorization, &m.uv_offset_scroll};
    for (const auto& f : kMeshFields) {
        const fx::Parameter* p = e.findParameter(f.name);
        if (!p || !is_material_parameter(*p)) continue;
        std::vector<float> v = param_value(*p, lookup);
        float* dst = &vecs[f.vec]->x;
        for (int i = 0; i < f.count && i < static_cast<int>(v.size()) && f.comp + i < 4; ++i)
            dst[f.comp + i] = v[static_cast<size_t>(i)];
    }
    return m;
}

uint32_t MaterialTable::intern(const MeshMaterial& m) {
    ++requests_;
    const uint64_t h = alamo::fnv1a64(&m, sizeof m);
    auto& ids = index_[h];
    for (uint32_t id : ids)
        if (std::memcmp(&records_[id], &m, sizeof m) == 0) return id;
    const uint32_t id = static_cast<uint32_t>(records_.size());
    records_.push_back(m);
    ids.push_back(id);
    return id;
}

}  // namespace alamo::materials

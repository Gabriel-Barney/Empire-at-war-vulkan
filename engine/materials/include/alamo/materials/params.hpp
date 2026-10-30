// Effect parameter values, material records and their deduplication (PORTING.md 2.3).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "alamo/materials/constants.hpp"

namespace alamo::fx {
struct Effect;
struct Parameter;
}  // namespace alamo::fx

namespace alamo::materials {

// Supplies the current value of an effect parameter (engine semantic or ALO material value).
// Write param_float_count(p) floats in declaration order (matrices row-major = D3DXMATRIX) into
// `out` and return true, or return false to use the effect's default value.
using ParamLookup = std::function<bool(const fx::Parameter& p, std::span<float> out)>;

// Number of floats of a numeric parameter (rows * columns * max(1, elements)); 0 for objects.
size_t param_float_count(const fx::Parameter& p);
// Current value: lookup if it answers, else the effect default (ints/bools converted to float).
void param_value(const fx::Parameter& p, const ParamLookup& lookup, std::span<float> out);
std::vector<float> param_value(const fx::Parameter& p, const ParamLookup& lookup);

// Simple name/semantic keyed value store (ALO material values, engine semantics for tests and
// tools). Name match first, then semantic; both ASCII case-insensitive.
class ParamStore {
public:
    void set(std::string_view name, std::vector<float> values);
    void set_semantic(std::string_view semantic, std::vector<float> values);
    bool get(const fx::Parameter& p, std::span<float> out) const;
    ParamLookup lookup() const;

private:
    std::map<std::string, std::vector<float>> by_name_, by_semantic_;  // upper-case keys
};

// True for the engine-supplied global block (CATALOG section 3) and _ALAMO_ metadata, i.e.
// parameters that are not per-material.
bool is_engine_parameter(const fx::Parameter& p);
bool is_material_parameter(const fx::Parameter& p);  // numeric, not engine, not _ALAMO_*

// Per-effect generic material record (effects outside the mesh family): material parameters in
// declaration order with HLSL cbuffer packing (no straddling of 16-byte registers; arrays and
// matrices start on a register and use one register per row/element).
struct GenericField {
    std::string name;
    uint32_t offset = 0;      // bytes
    uint32_t floats = 0;      // value floats
    uint32_t registers = 0;   // 16-byte registers spanned
    uint32_t columns = 0, rows = 0, elements = 0;
};
struct GenericLayout {
    std::vector<GenericField> fields;
    uint32_t size = 0;        // bytes, multiple of 16
};
GenericLayout build_generic_layout(const fx::Effect& e);
// Packs the current values per `layout` (out.size() == layout.size / 4).
void fill_generic_record(const fx::Effect& e, const GenericLayout& layout, const ParamLookup& lookup,
                         std::span<float> out);

// Mesh-family record: maps the effect's material parameters onto MeshMaterial (mapping table in
// params.cpp: Emissive/Diffuse/Specular/Shininess/Colorization/UVOffset/UVScrollRate/Color/
// BendScale/DebugColor, Planet: Atmosphere/CityColor/AtmospherePower/CloudScrollRate).
// `tex` is left zero (= kNoTexture, white slot 0, sampler slot 0); the caller fills the indices.
MeshMaterial build_mesh_material(const fx::Effect& e, const ParamLookup& lookup);

// Global deduplicated MaterialTable: one record per distinct content, insertion order (index =
// first insertion). Content hash is FNV-1a over the record bytes (deterministic).
class MaterialTable {
public:
    uint32_t intern(const MeshMaterial& m);
    const std::vector<MeshMaterial>& records() const { return records_; }
    size_t requests() const { return requests_; }

private:
    std::vector<MeshMaterial> records_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> index_;  // hash -> record ids (lookup only)
    size_t requests_ = 0;
};

}  // namespace alamo::materials

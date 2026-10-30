// Minimal JSON + glTF 2.0 reader for the Phase 1 alo2gltf exports (docs/formats/examples):
// node TRS hierarchy, float/u16/u32 accessors, triangle lists, one rigid skin, animation 0
// sampled at a keyframe index (exactly what tools/formats/python/render_gltf.py does).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace alamo::reftest {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const std::string& k) const;
    double number(const std::string& k, double def = 0) const;
    int integer(const std::string& k, int def = -1) const { return static_cast<int>(number(k, def)); }
};
bool parse_json(const std::string& text, Json& out, std::string* err = nullptr);

struct Dmat4 {  // column-vector, m[row][col]
    double m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    Dmat4 operator*(const Dmat4& b) const;
    void apply(const double p[3], double out[3]) const;
};

struct GltfPrimitive {
    int node = -1;              // mesh node
    bool skinned = false;
    std::vector<float> positions;     // xyz
    std::vector<uint16_t> joints;     // JOINTS_0.x (rigid: weight 1 on joint 0)
    std::vector<uint32_t> indices;
};

struct Gltf {
    Json json;
    std::vector<uint8_t> bin;
    std::vector<GltfPrimitive> primitives;  // in node order, then primitive order
    std::vector<int> skin_joints;           // node per joint
    std::vector<Dmat4> inverse_bind;        // per joint
    int animation_frames = 0;
    std::string animation_name;

    bool load(const std::string& gltf_path, std::string* err = nullptr);
    // Node global matrices with animation 0 applied at keyframe `frame` (-1 = bind/rest TRS).
    // `skip_root_rotation`: replace the scene root's matrix with identity (back to Alamo Z-up).
    std::vector<Dmat4> globals(int frame, bool skip_root_rotation) const;
    // World positions of every primitive vertex for that pose (render_gltf.py collect()).
    std::vector<std::vector<double>> posed_positions(int frame, bool skip_root_rotation) const;
};

}  // namespace alamo::reftest

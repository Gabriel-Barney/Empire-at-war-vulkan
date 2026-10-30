// ALO: Alamo Object files. An .alo is either
//   * a model   (top-level chunks 0x200 skeleton, 0x400 mesh*, 0x1300 light*, 0x600 connections), or
//   * a particle system (single top-level chunk 0x900, see particle.hpp).
#pragma once

#include "alamo/formats/chunk.hpp"
#include "alamo/formats/particle.hpp"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace alamo::alo {

// Bone billboard modes (runtime switch on the bone flag bits 3..6; names follow the exporter).
enum class BillboardMode : std::uint32_t {
    None = 0,
    Parallel = 1,     // aligned with the camera's right/up axes (screen-parallel)
    Face = 2,         // faces the camera position
    ZAxisView = 3,    // rotates about local Z toward the view direction
    ZAxisWind = 4,    // rotates about local Z toward the scene wind vector
    ZAxisLight = 5,   // rotates about local Z toward the sun/light direction
    SunGlow = 6,      // placed along the sun direction from the parent bone (translation zeroed on load)
    Sun = 7,          // placed at camera + sun direction * bone distance (translation zeroed on load)
};
const char* billboard_mode_name(std::uint32_t mode);

// ---------------------------------------------------------------------------------------------
// Skeleton (chunk 0x200). Structure-of-arrays; index 0 is always the root ("Root").
struct Skeleton {
    std::vector<std::string> names;
    std::vector<std::int32_t> parents;          // -1 for root
    std::vector<std::uint8_t> visible;          // bone visibility flag
    std::vector<std::uint32_t> billboard_mode;  // BillboardMode (0x206 bones only, else 0)
    std::vector<Mat34> local;                   // transform relative to parent bone
    std::uint32_t declared_count = 0;           // value from 0x201 header

    std::size_t size() const { return names.size(); }
    // Bone-to-model transform (product of locals up the parent chain).
    Mat34 world(std::size_t bone) const;
    std::vector<Mat34> world_all() const;
    std::optional<std::size_t> find(std::string_view name) const;  // case-insensitive
};

// ---------------------------------------------------------------------------------------------
// Vertex data.
//
// Every mesh stores vertices in one "master" layout: 144 bytes in chunk 0x10007, or 128 bytes in
// the EaW-era chunk 0x10005 (same fields without the unknown float4; SPEC.md 2.5). The vertex
// format name (chunk 0x10002, e.g. "alD3dVertNU2U3U3") tells the runtime which subset of those
// fields to upload to the GPU; see VertexFormatInfo.
struct VertexFormatInfo {
    bool known = false;
    bool normal = false;
    int uv_sets = 0;          // number of meaningful texcoord sets
    bool tangent_frame = false;  // tangent + binormal
    bool color = false;
    bool skinned = false;     // bone_influences > 0
    // Bone influences per vertex (indices into the bone map): 0 static, 1 "RSkin" rigid skinning
    // (bone_index[0], weight 1), 4 "B4I4" (4 indices + 4 weights; shipped data has weights 1,0,0,0).
    int bone_influences = 0;
    bool grass = false;
};
// Table lookup (case-insensitive); unknown names get known = false and fields inferred from the
// tokens U2, U3U3, RSkin/B4I4 and a trailing C.
VertexFormatInfo vertex_format_info(std::string_view name);

// Structure-of-arrays view of the vertex record (128-byte records: extra = 1,1,1,1).
struct VertexBuffer {
    std::vector<Vec3> position;
    std::vector<Vec3> normal;
    std::array<std::vector<Vec2>, 4> uv;
    std::vector<Vec3> tangent;
    std::vector<Vec3> binormal;
    std::vector<Vec4> color;
    std::vector<Vec4> extra;   // unknown float4 (observed always 1,1,1,1; not stored in 128-byte records)
    std::vector<std::array<std::uint32_t, 4>> bone_index;  // indices into SubMesh::bone_map
    std::vector<Vec4> bone_weight;
    std::size_t size() const { return position.size(); }
};

// Material parameter value types (chunks 0x10102..0x10106).
enum class ParamType : std::uint8_t { Int = 0, Float = 1, Float3 = 2, Texture = 3, Float4 = 4 };
using ParamValue = std::variant<std::int32_t, float, Vec3, std::string, Vec4>;
struct MaterialParam {
    std::string name;
    ParamType type = ParamType::Float;
    ParamValue value;
};
struct Material {
    std::string shader;  // effect file, e.g. "MeshBumpColorize.fx"
    std::vector<MaterialParam> params;
    const MaterialParam* find(std::string_view name) const;
};

// Collision tree (chunk 0x1200, child of a submesh). A binary AABB tree over the submesh's
// triangles. Each node box is quantised to 0..255 *relative to its parent's box* (verified).
struct CollisionTree {
    AABB bounds;                    // root box (mini 0x00/0x01 of chunk 0x1201)
    std::uint32_t declared_nodes = 0;      // mini 0x02
    std::uint32_t declared_triangles = 0;  // mini 0x03
    struct Node {
        std::uint8_t qmin[3];
        std::uint8_t qmax[3];
        std::uint16_t count;   // 0 => internal node; >0 => leaf with `count` triangles
        std::uint16_t index;   // internal: first child (children are index, index+1);
                               // leaf: first entry in `triangles`
        bool is_leaf() const { return count != 0; }
    };
    std::vector<Node> nodes;
    std::vector<std::uint16_t> triangles;  // primitive indices into SubMesh::indices / 3
    // Dequantised box of every node (computed top-down).
    std::vector<AABB> node_bounds() const;
};

struct SubMesh {
    Material material;
    std::string vertex_format;
    VertexFormatInfo format;
    std::uint32_t declared_vertices = 0;
    std::uint32_t declared_primitives = 0;
    std::uint32_t vertex_chunk_id = 0;   // 0x10007 (current) or 0x10005 (legacy)
    std::uint32_t vertex_stride = 0;     // bytes per stored vertex: 144, or 128 (legacy 0x10005)
    VertexBuffer vertices;
    std::vector<std::uint16_t> indices;  // triangle list, 3 per primitive
    std::vector<std::uint32_t> bone_map; // chunk 0x10006: local skin index -> skeleton bone
    std::optional<CollisionTree> collision;
};

struct Mesh {
    std::string name;
    AABB bounds;
    std::uint32_t declared_submeshes = 0;
    std::uint32_t unknown = 0;          // u32 at +28 of 0x402 (always 0; never read by the runtime loader)
    bool hidden = false;
    bool collision_enabled = false;
    std::vector<SubMesh> submeshes;
};

enum class LightType : std::uint32_t { Omni = 0, Directional = 1, Spot = 2 };
struct Light {
    std::string name;
    LightType type = LightType::Omni;
    Vec3 color;
    float intensity = 0;
    float far_attenuation_end = 0;
    float far_attenuation_start = 0;
    float hotspot_size = 0;
    float falloff_size = 0;
};

// An "object" is a mesh or a light, numbered in file order; connections refer to object indices.
struct ObjectRef {
    enum class Kind : std::uint8_t { Mesh, Light } kind;
    std::uint32_t index;  // into Model::meshes or Model::lights
};

struct Connection {
    std::uint32_t object = 0;  // index into Model::objects
    std::uint32_t bone = 0;
};

struct Proxy {
    std::string name;  // name of the .alo/.alp to attach (no extension)
    std::uint32_t bone = 0;
    bool hidden = false;
    bool alt_decrease_stay_hidden = false;
};

// Unparsed/unknown chunk retained for diagnostics.
struct UnknownChunk {
    std::string path;  // e.g. "400/10000/12345"
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct Model {
    Skeleton skeleton;
    std::vector<Mesh> meshes;
    std::vector<Light> lights;
    std::vector<ObjectRef> objects;  // meshes & lights in file order
    std::vector<Connection> connections;
    std::vector<Proxy> proxies;
    // 0x601 header counts (mini id -> value): 0x01 objects/connections, 0x04 proxies, 0x09 dazzles?
    std::uint32_t declared_connections = 0;
    std::uint32_t declared_proxies = 0;
    std::optional<std::uint32_t> declared_dazzles;
    std::vector<UnknownChunk> unknown;

    // Bone that an object is attached to (0 = root if no connection).
    std::uint32_t object_bone(std::uint32_t object) const;
    std::uint32_t mesh_bone(std::uint32_t mesh_index) const;
};

struct AloFile {
    enum class Kind { Model, ParticleSystem } kind = Kind::Model;
    Model model;                                    // valid when kind == Model
    particle::ParticleSystem particles;             // valid when kind == ParticleSystem
};

// Parse an in-memory .alo. Throws FormatError.
AloFile parse(Bytes data);
AloFile load(const std::filesystem::path& path);

}  // namespace alamo::alo

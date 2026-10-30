// alamo_assets - small float math used by the asset data (no dependency on tools/formats headers,
// so renderer code that includes alamo/assets/* never sees the alamo_formats types).
//
// Conventions (docs/formats/SPEC.md 2.2):
//   * Alamo is right-handed, Z-up; triangles are CCW with respect to the stored normals.
//   * Mat3x4 is row-major 3x4 with column vectors: p'[r] = m[r][0]*x + m[r][1]*y + m[r][2]*z + m[r][3].
//     Its memory layout (12 floats, row by row) is exactly an HLSL `float3x4` row-major / GLSL
//     `mat3x4` with row_major, i.e. 48 bytes per bone in the skin palette (PORTING.md 2.4).
//   * a * b applies b first.
//   * Quat is (x, y, z, w), Hamilton convention, as stored in ALA files.
// Rendering math only: plain IEEE single, no determinism guarantee is needed or given.
#pragma once
#include <cstdint>

namespace alamo::assets {

struct Float2 { float x = 0, y = 0; };
struct Float3 { float x = 0, y = 0, z = 0; };
struct Float4 { float x = 0, y = 0, z = 0, w = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };

struct Mat3x4 {
    float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

    static Mat3x4 identity() { return {}; }
    // T * R * S (scale first, then rotation, then translation).
    static Mat3x4 from_trs(const Float3& t, const Quat& r, const Float3& s);
    Float3 transform_point(const Float3& p) const;
    Float3 transform_dir(const Float3& d) const;
    Mat3x4 operator*(const Mat3x4& rhs) const;
    // General affine inverse. A singular matrix returns identity (never NaN/UB).
    Mat3x4 inverse() const;
    Float3 translation() const { return {m[0][3], m[1][3], m[2][3]}; }
};
static_assert(sizeof(Mat3x4) == 48, "Mat3x4 must be 48 bytes (float3x4 palette entry)");

struct Aabb {
    Float3 min{0, 0, 0}, max{0, 0, 0};
    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
};
struct Sphere {
    Float3 center{0, 0, 0};
    float radius = 0;
};

// Normalised linear interpolation along the shortest arc (good enough between 30 fps keys).
Quat nlerp(const Quat& a, const Quat& b, float t);
Quat normalize(const Quat& q);
// Transform an AABB by an affine matrix (bounds of the 8 transformed corners).
Aabb transform_aabb(const Mat3x4& m, const Aabb& box);
Aabb merge(const Aabb& a, const Aabb& b);

}  // namespace alamo::assets

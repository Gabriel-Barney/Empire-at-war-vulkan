#include "alamo/anim/anim_math.hpp"

#include <bit>

#include "alamo/core/dmath.hpp"

namespace alamo::anim {

Quat dequantize_rotation(const int16_t q[4]) {
    return {static_cast<float>(q[0]) / 32767.0f, static_cast<float>(q[1]) / 32767.0f, static_cast<float>(q[2]) / 32767.0f,
            static_cast<float>(q[3]) / 32767.0f};
}

Float3 dequantize_translation(const Float3& offset, const Float3& scale, const uint16_t q[3]) {
    const float px = scale.x * static_cast<float>(q[0]);
    const float py = scale.y * static_cast<float>(q[1]);
    const float pz = scale.z * static_cast<float>(q[2]);
    return {offset.x + px, offset.y + py, offset.z + pz};
}

Quat nlerp_fast(const Quat& a, const Quat& b0, float t) {
    float dot = a.x * b0.x;
    dot += a.y * b0.y;
    dot += a.z * b0.z;
    dot += a.w * b0.w;
    Quat b = b0;
    if (dot < 0.0f) b = {-b0.x, -b0.y, -b0.z, -b0.w};
    Quat r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    float len2 = r.x * r.x;
    len2 += r.y * r.y;
    len2 += r.z * r.z;
    len2 += r.w * r.w;
    const float inv = dmath::fast_rsqrt_retail(len2);
    r.x *= inv;
    r.y *= inv;
    r.z *= inv;
    r.w *= inv;
    return r;
}

Float3 lerp(const Float3& a, const Float3& b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }

Mat34 mat_from_rt(const Quat& q, const Float3& t) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat34 o;
    o.m[0][0] = 1.0f - 2.0f * (yy + zz);
    o.m[0][1] = 2.0f * (xy - wz);
    o.m[0][2] = 2.0f * (xz + wy);
    o.m[0][3] = t.x;
    o.m[1][0] = 2.0f * (xy + wz);
    o.m[1][1] = 1.0f - 2.0f * (xx + zz);
    o.m[1][2] = 2.0f * (yz - wx);
    o.m[1][3] = t.y;
    o.m[2][0] = 2.0f * (xz - wy);
    o.m[2][1] = 2.0f * (yz + wx);
    o.m[2][2] = 1.0f - 2.0f * (xx + yy);
    o.m[2][3] = t.z;
    return o;
}

Mat34 mul(const Mat34& a, const Mat34& b) {
    Mat34 o;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = a.m[r][0] * b.m[0][c];
            v += a.m[r][1] * b.m[1][c];
            v += a.m[r][2] * b.m[2][c];
            if (c == 3) v += a.m[r][3];
            o.m[r][c] = v;
        }
    }
    return o;
}

Float3 transform_point(const Mat34& m, const Float3& p) {
    float v[3];
    for (int r = 0; r < 3; ++r) {
        float s = m.m[r][0] * p.x;
        s += m.m[r][1] * p.y;
        s += m.m[r][2] * p.z;
        s += m.m[r][3];
        v[r] = s;
    }
    return {v[0], v[1], v[2]};
}

uint32_t float_bits(float f) { return std::bit_cast<uint32_t>(f); }

}  // namespace alamo::anim

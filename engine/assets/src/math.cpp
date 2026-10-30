#include "alamo/assets/math.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::assets {

Mat3x4 Mat3x4::from_trs(const Float3& t, const Quat& q, const Float3& s) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const float r[3][3] = {
        {1.f - 2.f * (yy + zz), 2.f * (xy - wz), 2.f * (xz + wy)},
        {2.f * (xy + wz), 1.f - 2.f * (xx + zz), 2.f * (yz - wx)},
        {2.f * (xz - wy), 2.f * (yz + wx), 1.f - 2.f * (xx + yy)},
    };
    const float sc[3] = {s.x, s.y, s.z};
    Mat3x4 o;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) o.m[row][col] = r[row][col] * sc[col];
    o.m[0][3] = t.x;
    o.m[1][3] = t.y;
    o.m[2][3] = t.z;
    return o;
}

Float3 Mat3x4::transform_point(const Float3& p) const {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}

Float3 Mat3x4::transform_dir(const Float3& d) const {
    return {m[0][0] * d.x + m[0][1] * d.y + m[0][2] * d.z, m[1][0] * d.x + m[1][1] * d.y + m[1][2] * d.z,
            m[2][0] * d.x + m[2][1] * d.y + m[2][2] * d.z};
}

Mat3x4 Mat3x4::operator*(const Mat3x4& b) const {
    Mat3x4 o;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = m[r][0] * b.m[0][c] + m[r][1] * b.m[1][c] + m[r][2] * b.m[2][c];
            if (c == 3) v += m[r][3];
            o.m[r][c] = v;
        }
    }
    return o;
}

Mat3x4 Mat3x4::inverse() const {
    const float a = m[0][0], b = m[0][1], c = m[0][2];
    const float d = m[1][0], e = m[1][1], f = m[1][2];
    const float g = m[2][0], h = m[2][1], i = m[2][2];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-30f) || !std::isfinite(det)) return identity();
    const float inv = 1.f / det;
    Mat3x4 o;
    o.m[0][0] = A * inv;
    o.m[0][1] = -(b * i - c * h) * inv;
    o.m[0][2] = (b * f - c * e) * inv;
    o.m[1][0] = B * inv;
    o.m[1][1] = (a * i - c * g) * inv;
    o.m[1][2] = -(a * f - c * d) * inv;
    o.m[2][0] = C * inv;
    o.m[2][1] = -(a * h - b * g) * inv;
    o.m[2][2] = (a * e - b * d) * inv;
    for (int r = 0; r < 3; ++r)
        o.m[r][3] = -(o.m[r][0] * m[0][3] + o.m[r][1] * m[1][3] + o.m[r][2] * m[2][3]);
    return o;
}

Quat normalize(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-20f) || !std::isfinite(n)) return Quat{};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

Quat nlerp(const Quat& a, const Quat& b, float t) {
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float sb = dot < 0 ? -t : t;
    const float sa = 1.f - t;
    return normalize({a.x * sa + b.x * sb, a.y * sa + b.y * sb, a.z * sa + b.z * sb, a.w * sa + b.w * sb});
}

Aabb transform_aabb(const Mat3x4& m, const Aabb& box) {
    if (!box.valid()) return box;
    Aabb o;
    bool first = true;
    for (int k = 0; k < 8; ++k) {
        const Float3 p{(k & 1) ? box.max.x : box.min.x, (k & 2) ? box.max.y : box.min.y,
                       (k & 4) ? box.max.z : box.min.z};
        const Float3 q = m.transform_point(p);
        if (first) {
            o.min = o.max = q;
            first = false;
        } else {
            o.min = {std::min(o.min.x, q.x), std::min(o.min.y, q.y), std::min(o.min.z, q.z)};
            o.max = {std::max(o.max.x, q.x), std::max(o.max.y, q.y), std::max(o.max.z, q.z)};
        }
    }
    return o;
}

Aabb merge(const Aabb& a, const Aabb& b) {
    if (!a.valid()) return b;
    if (!b.valid()) return a;
    return {{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
            {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

}  // namespace alamo::assets

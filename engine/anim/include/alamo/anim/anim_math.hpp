// R3 deterministic pose math (docs/re/ANIM_PARTICLES.md §A.6). Gameplay reads animated bone transforms
// (projectile spawn points, Lua Get_Bone_Position, hardpoint effects), so every operation here is a fixed
// sequence of IEEE float +, -, * (no FMA: the engine builds with /fp:precise / -ffp-contract=off) and the
// retail fast inverse square root (dmath::fast_rsqrt_retail: magic 0x5F375A86, two Newton steps), never
// 1/sqrt. The functions live in anim_math.cpp (not inline) so every caller - sim or render, any compiler
// flags - runs the same compiled code.
//
// Conventions (docs/formats/SPEC.md §2.2, same as alamo_assets): Mat34 is row-major 3x4 with column
// vectors, p'[r] = m[r][0]*x + m[r][1]*y + m[r][2]*z + m[r][3]; a * b applies b first. Mat34 has the
// memory layout of assets::Mat3x4 (12 floats, 48 bytes), so render code may copy poses byte for byte.
// Quat is (x, y, z, w), Hamilton, as stored in ALA files.
#pragma once
#include <cstdint>

namespace alamo::anim {

struct Float3 {
    float x = 0, y = 0, z = 0;
};
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
struct Mat34 {
    float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    Float3 translation() const { return {m[0][3], m[1][3], m[2][3]}; }
};
static_assert(sizeof(Mat34) == 48, "Mat34 must match assets::Mat3x4 (48 bytes)");

// ALA rotation dequantisation (SPEC §4.3): component = float(i16) / 32767.0f (no normalisation here;
// the nlerp below normalises). [port] division (not multiplication by 1/32767) is assumed.
Quat dequantize_rotation(const int16_t q[4]);
// ALA translation: offset + scale * float(u16), per component (product, then sum).
Float3 dequantize_translation(const Float3& offset, const Float3& scale, const uint16_t q[3]);

// Shortest-arc nlerp (0x235750): when dot(a, b) < 0 the second quaternion is negated; component-wise
// lerp a + (b - a) * t; normalised with fast_rsqrt_retail(x*x + y*y + z*z + w*w) (sums left to right).
// [port] the lerp form a + (b - a) * t and the summation order are assumed (bit parity with the
// original needs a disassembly check; listed in docs/PHASE4.md R3 open items).
Quat nlerp_fast(const Quat& a, const Quat& b, float t);
// a + (b - a) * t per component.
Float3 lerp(const Float3& a, const Float3& b, float t);

// Rotation (not re-normalised) + translation -> local matrix (classic quaternion -> matrix).
Mat34 mat_from_rt(const Quat& r, const Float3& t);
// a * b (b applied first); fixed summation order m0*b0 + m1*b1 + m2*b2 (+ m3).
Mat34 mul(const Mat34& a, const Mat34& b);
Float3 transform_point(const Mat34& m, const Float3& p);

// Bit patterns (tests, hashes).
uint32_t float_bits(float f);

}  // namespace alamo::anim

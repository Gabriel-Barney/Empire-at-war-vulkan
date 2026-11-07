// D3D9 SM1-3 -> SM6 HLSL emitter. Semantic choices (each mirrored by interpreter.cpp, which is the
// independent CPU reference used by the differential test):
//
// | topic                 | choice |
// |-----------------------|--------|
// | multiplication        | D3D9 "legacy" rule 0 * x = 0 for every x (incl. inf/NaN) in mul, mad, dp3, dp4, dp2add, m*x*, lrp, crs, nrm, dst, pow's y*log2(x) (AMD/NVIDIA D3D9 drivers, DXVK floatEmulation). Texture-unit math (texbem, texm3x*, bem) uses IEEE products. |
// | rcp / rsq             | rcp(0) = rsq(0) = +inf (also for -0); rsq uses |x|; scalar operand = swizzled .w (selector 3) |
// | log / logp(vs_2_0)    | log2(|x|), log(0) = -FLT_MAX |
// | exp / expp(vs_2_0)    | exp2(x) full precision (partial precision is not emulated: >= the 10 bits D3D requires) |
// | expp vs_1_x           | (2^floor(w), w - floor(w), 2^w with the low 8 mantissa bits cleared, 1) (refrast) |
// | logp vs_1_x           | (exponent, mantissa in [1,2), log2|w| with the low 8 bits cleared, 1); 0 -> (-FLT_MAX, 1, -FLT_MAX, 1) |
// | frc                   | x - floor(x) (not frac(), whose rounding differs) |
// | min / max             | (a < b) ? a : b / (a >= b) ? a : b, per component (D3D9 definition, NaN-deterministic) |
// | lit                   | x = 1, y = max-gated src.x, z = src.y^clamp(src.w, +-127.9961) when src.x > 0 && src.y > 0, w = 1 |
// | sincos (SM2)          | exact cos/sin of the scalar operand; the macro constants src1/src2 are ignored |
// | pow                   | exp2(y * log2|x|) with the legacy multiply, so pow(0, 0) = 1 |
// | nrm                   | src * rsq(dp3(src, src)) with the legacy multiply, so nrm(0) = 0 |
// | a0 (vs_1_x mov)       | floor (D3D9 vs_1_1 rule; the bone-index code feeds non-negative integers, so floor = truncation there) |
// | a0 (vs_2_x mova)      | round to nearest, floor(x + 0.5); both clamped to +-65536 before the int conversion |
// | relative c[a0 + n]    | out-of-range index reads (0,0,0,0); def constants are visible through relative reads |
// | def / defi / defb     | inlined literals; they override the constant buffer at that register (D3D9 rule) |
// | ps_1_x ranges         | every write to r#/t# is clamped to [-1, 1] (ps_1_1..1_3) or [-8, 8] (ps_1_4) after the shift and _sat; c# reads clamp to [-1, 1]; v# reads saturate (all models) |
// | ps_1_x modifiers      | _bias x-0.5, _bx2 2(x-0.5), 1-x, _x2, neg; dest _x2/_x4/_x8/_d2/_d4/_d8 then _sat |
// | cnd / cmp             | cnd: src0 > 0.5 per component (ps_1_1..1_3 src0 is r0.a replicated); cmp: src0 >= 0 |
// | co-issue (+)          | both instructions read their sources before either writes |
// | texcoord (1.1-1.3)    | saturate(float4(tc.xyz, 1)); texcrd (1.4): float4(tc.xyz, 1) (_dz/_dw divide xy), clamped to +-8 |
// | texkill               | ps_1_1..1_3: texture coordinate set n, xyz < 0; ps_1_4: register xyz; ps_2_x: register, masked components |
// | tex (1.1-1.3)         | stage n texture at texcoord n (xy for 2D, xyz cube/volume); D3DTTFF_PROJECTED via TranslateOptions |
// | texbem/texbeml/bem    | bump matrix from PSConstants c16+stage = (M00, M01, M10, M11), luminance c24+stage = (scale, offset); luminance saturated |
// | texm3x2pad/3x3pad     | the dot product is hidden state (pad register value is not readable, as in D3D) |
// | texm3x3spec/vspec     | R = 2 N (N.E) / (N.N) - E; N.N = 0 gives R = -E |
// | texm3x2depth/texdepth | z / w, w = 0 gives 1.0; oDepth saturated at output |
// | texldp / texldb       | coord.xyz / coord.w; SampleBias(coord, coord.w) |
// | oPos                  | written as is (D3D9 clip z is already 0..w); xy += PC.PixelOffset.xy * w (engine-supplied half-pixel fix, 0 by default); no Y flip (engine/render uses a negative-height viewport) |
// | oD0/oD1/oFog          | saturated at VS output; unwritten: COLOR0 = 1, COLOR1 = 0, TEXCOORDn = (0,0,0,1), FOG = 1 |
// | alpha test            | push AlphaFunc (VkCompareOp order, 7 = off) on floor(saturate(a) * 255 + 0.5) vs AlphaRef |
// | fog                   | push Flags bit 0: rgb = FogColor + f (rgb - FogColor); f = saturate(oFog) or table EXP/EXP2/LINEAR of SV_Position.z |
// | rep / loop            | iteration count clamped to [0, 255] (D3D9 limit) |
// | partial precision     | _pp and min precision are ignored (full float) |
// | SM3 outputs (vs_3_0)  | o0-o11 by dcl usage: POSITION0 -> oPos, COLOR0/1, TEXCOORD0-7, FOG0 (.x), PSIZE0 (.x); a register keeps its whole float4 (components stay in place); defaults as for SM1-2; no saturation of colors or fog (vs_3_0 outputs are not clamped); other usages, output relative addressing and vertex texture fetch are rejected |
// | SM3 inputs (ps_3_0)   | v0-v9 by dcl usage from the same varyings, not saturated; FOG = (fog, 0, 0, 0); vPos = (SV_Position.xy - 0.5, 0, 0) (D3D9 pixel centres are integers); vFace = +1 front / -1 back (SV_IsFrontFace) |
// | SM3 constants         | ps_3_0 c0-c31 from PSConstants plus def literals anywhere in c0-c223; a non-def read of c32+ is rejected (the engine's PS register file is 32) |
// | SM3 fog               | ps_3_0: no fixed-function fog (D3D9 leaves fog to the ps_3_0 shader); the alpha test still applies |
// | texldl                | SampleLevel(coord, coord.w) (ps_3_0; vs_3_0 vertex texture fetch is rejected) |
#include "alamo/shaderxlat/translate.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "alamo/core/hash.hpp"

namespace alamo::shaderxlat {

namespace {

constexpr char kComp[4] = {'x', 'y', 'z', 'w'};

// Correctly rounded 9-significant-digit form (parses back, via double, to the same float).
std::string flt(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    if (!std::isfinite(f) || (f != 0.0f && std::fabs(f) < 1.17549435e-38f)) {
        char b[32];
        std::snprintf(b, sizeof(b), "asfloat(0x%08Xu)", bits);
        return b;
    }
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::general, 9);
    std::string s(buf, r.ptr);
    // Trim trailing zeros of the fraction ("0.500000000" -> "0.5"), keep a '.' or exponent.
    const size_t e = s.find_first_of("eE");
    std::string mant = e == std::string::npos ? s : s.substr(0, e);
    const std::string expo = e == std::string::npos ? std::string() : s.substr(e);
    if (mant.find('.') != std::string::npos) {
        while (!mant.empty() && mant.back() == '0') mant.pop_back();
        if (!mant.empty() && mant.back() == '.') mant += '0';
    } else {
        mant += ".0";
    }
    return mant + expo;
}

std::string fbits(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return flt(f);
}

std::string mask_str(uint8_t m) {
    std::string s = ".";
    for (int i = 0; i < 4; ++i)
        if (m & (1u << i)) s += kComp[i];
    return s;
}

const char* kHelpers = R"(float4 xl_mul(float4 a, float4 b) { return select(or(a == 0.0, b == 0.0), float4(0.0, 0.0, 0.0, 0.0), a * b); }
float xl_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float xl_dp3(float4 a, float4 b) { float4 m = xl_mul(a, b); return m.x + m.y + m.z; }
float xl_dp4(float4 a, float4 b) { float4 m = xl_mul(a, b); return m.x + m.y + m.z + m.w; }
float4 xl_min(float4 a, float4 b) { return select(a < b, a, b); }
float4 xl_max(float4 a, float4 b) { return select(a >= b, a, b); }
float4 xl_slt(float4 a, float4 b) { return select(a < b, float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0)); }
float4 xl_sge(float4 a, float4 b) { return select(a >= b, float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0)); }
float xl_rcp(float x) { return x == 0.0 ? asfloat(0x7F800000u) : 1.0 / x; }
float xl_rsq(float x) { float a = abs(x); return a == 0.0 ? asfloat(0x7F800000u) : rsqrt(a); }
float xl_log(float x) { float a = abs(x); return a == 0.0 ? -asfloat(0x7F7FFFFFu) : log2(a); }
float xl_pow(float a, float b) { return exp2(xl_mul1(b, log2(abs(a)))); }
float4 xl_frc(float4 v) { return v - floor(v); }
float4 xl_sgn(float4 v) { return float4(sign(v)); }
float4 xl_expp1(float w) {
    float f = floor(w);
    return float4(exp2(f), w - f, asfloat(asuint(exp2(w)) & 0xFFFFFF00u), 1.0);
}
float4 xl_logp1(float w) {
    float v = abs(w);
    if (v == 0.0) return float4(-asfloat(0x7F7FFFFFu), 1.0, -asfloat(0x7F7FFFFFu), 1.0);
    uint b = asuint(v);
    float e = (float)((int)(b >> 23) - 127);
    float m = asfloat((b & 0x007FFFFFu) | 0x3F800000u);
    return float4(e, m, asfloat(asuint(log2(v)) & 0xFFFFFF00u), 1.0);
}
float4 xl_lit(float4 s) {
    float4 r = float4(1.0, 0.0, 0.0, 1.0);
    float p = clamp(s.w, -127.9961, 127.9961);
    if (s.x > 0.0) {
        r.y = s.x;
        if (s.y > 0.0) r.z = exp2(p * log2(s.y));
    }
    return r;
}
float4 xl_dst(float4 a, float4 b) { return float4(1.0, xl_mul1(a.y, b.y), a.z, b.w); }
float4 xl_crs(float4 a, float4 b) {
    return float4(xl_mul1(a.y, b.z) - xl_mul1(a.z, b.y), xl_mul1(a.z, b.x) - xl_mul1(a.x, b.z),
                  xl_mul1(a.x, b.y) - xl_mul1(a.y, b.x), 0.0);
}
float4 xl_nrm(float4 s) { float r = xl_rsq(xl_dp3(s, s)); return xl_mul(s, float4(r, r, r, r)); }
float4 xl_sincos(float x) { return float4(cos(x), sin(x), 0.0, 0.0); }
float xl_dp2add(float4 a, float4 b, float c) { return xl_mul1(a.x, b.x) + xl_mul1(a.y, b.y) + c; }
int4 xl_addr_floor(float4 v) { return int4(clamp(floor(v), -65536.0, 65536.0)); }
int4 xl_addr_round(float4 v) { return int4(clamp(floor(v + 0.5), -65536.0, 65536.0)); }
float4 xl_proj(float4 v) { return float4(v.xyz / v.w, v.w); }
)";

const char* kPush = R"(struct XlatPush {
    uint DrawIndex;
    uint AlphaFunc;
    uint AlphaRef;
    uint Flags;
    float4 FogColor;
    float4 FogParams;
    float4 PixelOffset;
};
[[vk::push_constant]] XlatPush PC;
)";

const char* kEpilogue = R"(float4 xl_epilogue(float4 c, float fog, float4 vpos) {
    if (PC.AlphaFunc < 7u) {
        uint a = (uint)floor(saturate(c.w) * 255.0 + 0.5);
        uint r = PC.AlphaRef;
        bool pass = false;
        if (PC.AlphaFunc == 1u) pass = a < r;
        else if (PC.AlphaFunc == 2u) pass = a == r;
        else if (PC.AlphaFunc == 3u) pass = a <= r;
        else if (PC.AlphaFunc == 4u) pass = a > r;
        else if (PC.AlphaFunc == 5u) pass = a != r;
        else if (PC.AlphaFunc == 6u) pass = a >= r;
        if (!pass) discard;
    }
    if ((PC.Flags & 1u) != 0u) {
        uint mode = (PC.Flags >> 1) & 3u;
        float f = saturate(fog);
        if (mode != 0u) {
            float d = vpos.z;
            if (mode == 1u) f = exp(-(d * PC.FogParams.z));
            else if (mode == 2u) f = exp(-((d * PC.FogParams.z) * (d * PC.FogParams.z)));
            else f = (PC.FogParams.y - d) / (PC.FogParams.y - PC.FogParams.x);
            f = saturate(f);
        }
        c.xyz = PC.FogColor.xyz + f * (c.xyz - PC.FogColor.xyz);
    }
    return c;
}
)";

const char* kEpilogue3 = R"(float4 xl_epilogue(float4 c) {
    if (PC.AlphaFunc < 7u) {
        uint a = (uint)floor(saturate(c.w) * 255.0 + 0.5);
        uint r = PC.AlphaRef;
        bool pass = false;
        if (PC.AlphaFunc == 1u) pass = a < r;
        else if (PC.AlphaFunc == 2u) pass = a == r;
        else if (PC.AlphaFunc == 3u) pass = a <= r;
        else if (PC.AlphaFunc == 4u) pass = a > r;
        else if (PC.AlphaFunc == 5u) pass = a != r;
        else if (PC.AlphaFunc == 6u) pass = a >= r;
        if (!pass) discard;
    }
    return c;
}
)";

// Default value of a vs_3_0 output register by its dcl usage (the SM1-2 defaults of that varying).
const char* sm3_output_default(Usage u, uint8_t index) {
    switch (u) {
    case Usage::Color: return index == 0 ? "float4(1.0, 1.0, 1.0, 1.0)" : "float4(0.0, 0.0, 0.0, 0.0)";
    case Usage::Texcoord: return "float4(0.0, 0.0, 0.0, 1.0)";
    case Usage::Fog: case Usage::PSize: return "float4(1.0, 0.0, 0.0, 0.0)";
    default: return "float4(0.0, 0.0, 0.0, 0.0)";
    }
}
// Fixed register the vs_3_0 output feeds (the SM1-2 output names of the VS epilogue).
std::string sm3_output_target(Usage u, uint8_t index) {
    switch (u) {
    case Usage::Position: return "oPos";
    case Usage::Color: return "oD" + std::to_string(index);
    case Usage::Texcoord: return "oT" + std::to_string(index);
    case Usage::Fog: return "oFog";
    default: return "oPts";
    }
}

struct Decl {
    bool present = false;
    Usage usage = Usage::Position;
    uint8_t index = 0;
    uint8_t mask = 0xF;
    bool centroid = false;
};

struct Emitter {
    const Program& p;
    const TranslateOptions& opt;
    std::string* err;
    bool vs = false, ps1 = false, ps14 = false, sm3 = false;
    const char* range = "1.0";

    std::array<bool, 256> cdef{};
    std::array<std::array<uint32_t, 4>, 256> cval{};
    std::array<bool, 16> idef{}, bdef{};
    std::array<std::array<int32_t, 4>, 16> ival{};
    std::array<uint32_t, 16> bval{};
    std::array<Decl, 16> vin{};    // VS inputs; PS v0/v1 (ps_2_x dcl); ps_3_0 v0-v9 (dcl usage)
    std::array<Decl, 8> tin{};     // PS ps_2_x t# dcl
    std::array<Decl, 12> vout{};   // vs_3_0 o0-o11 (dcl usage)
    std::array<TextureDim, 16> dims{};

    uint32_t temps = 0;
    uint8_t tregs = 0;
    uint8_t vread = 0;
    std::array<uint8_t, 2> vread_mask{};
    uint8_t tcread = 0;
    std::array<uint8_t, 8> tc_mask{};
    uint16_t samplers = 0;
    uint16_t sampler_bias = 0, sampler_grad = 0, sampler_lod = 0;
    uint16_t oregs = 0;             // vs_3_0 o# written
    uint16_t sm3_in = 0;            // ps_3_0 v# read
    bool vpos = false, vface = false;
    uint16_t vin_read = 0;
    uint16_t pads = 0;
    bool a0 = false, aL = false, p0 = false, depth = false, discard_ = false, bump = false, intbool = false,
         relative = false;
    bool tc_out_of_range = false;   // tc() was asked for a coordinate set >= 8 (rejected after emit)
    uint32_t max_c = 0;
    uint8_t opos = 0, ofog = 0, opts = 0, odepth = 0;
    std::array<uint8_t, 2> od{};
    std::array<uint8_t, 8> ot{};
    std::array<uint8_t, 4> oc{};

    std::string main_body;
    std::map<uint32_t, std::string> subs;
    std::string* cur = &main_body;
    int indent = 1;
    uint32_t tmp = 0, loops = 0;
    struct Cf {
        char kind;      // 'i' if, 'r' rep, 'l' loop
        uint32_t id;
        bool has_else;
    };
    std::vector<Cf> cf;
    bool in_sub = false, main_done = false;
    std::vector<uint32_t> called;

    Emitter(const Program& prog, const TranslateOptions& o, std::string* e) : p(prog), opt(o), err(e) {
        vs = p.is_vs();
        ps1 = p.ps1();
        ps14 = ps1 && p.minor >= 4;
        sm3 = p.major >= 3;
        range = ps14 ? "8.0" : "1.0";
    }

    bool bad(const Instr& in, const char* what) {
        if (err) *err = std::string(what) + ": '" + disassemble(p, in) + "' (token " + std::to_string(in.token) + ")";
        return false;
    }

    void line(const std::string& s) {
        cur->append(static_cast<size_t>(indent) * 4, ' ');
        *cur += s;
        *cur += '\n';
    }
    void line_to(std::string& out, const std::string& s) const {
        out.append(static_cast<size_t>(indent) * 4, ' ');
        out += s;
        out += '\n';
    }
    std::string newtmp() { return "x" + std::to_string(tmp++); }
    // Float constant register c# (Q3 instanced form: the per-instance image in the storage buffer).
    std::string creg(uint32_t idx) const {
        return opt.instanced ? "xl_c[xl_cb + " + std::to_string(idx) + "u]" : "c[" + std::to_string(idx) + "]";
    }

    // ------------------------------------------------------------ operands
    bool const_ref(const Instr& in, const Src& s, uint32_t offset, std::string& out) {
        if (s.relative) {
            relative = true;
            std::string a;
            if (s.rel_type == reg::kLoop) {
                aL = true;
                a = "aL";
            } else {
                if (!vs) return bad(in, "relative addressing outside a vertex shader");
                a0 = true;
                a = std::string("a0.") + kComp[s.rel_comp];
            }
            out = "xl_crel(" + a + " + " + std::to_string(s.index + offset) + ")";
            max_c = vs ? 256u : 32u;
            return true;
        }
        const uint32_t idx = s.index + offset;
        if (sm3 && !vs && idx < 224u && !cdef[idx] && idx >= 32u)
            return bad(in, "ps_3_0 constant beyond c31 (the engine's PS register file has 32 registers)");
        if (idx >= (vs ? 256u : sm3 ? 224u : 32u)) return bad(in, "constant register out of range");
        if (cdef[idx]) {
            out = "k" + std::to_string(idx);
        } else {
            // Not a bump-env read: bump_env is set only by texbem/texbeml/bem (ps_1_x).
            out = creg(idx);
            if (idx + 1 > max_c) max_c = idx + 1;
        }
        if (ps1) out = "clamp(" + out + ", -1.0, 1.0)";
        return true;
    }

    std::string int_reg(uint32_t n) {
        if (idef[n]) {
            const auto& v = ival[n];
            return "int4(" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", " + std::to_string(v[2]) + ", " +
                   std::to_string(v[3]) + ")";
        }
        intbool = true;
        return "xi[" + std::to_string(n) + "]";
    }
    std::string bool_reg(uint32_t n) {
        if (bdef[n]) return bval[n] ? "true" : "false";
        intbool = true;
        return "(xb[" + std::to_string(n / 4) + "]." + kComp[n % 4] + " != 0u)";
    }

    // Base register value (float4, before swizzle/modifier). offset = row offset for m4x4 & co.
    bool base(const Instr& in, const Src& s, uint32_t offset, std::string& out) {
        switch (s.type) {
        case reg::kTemp:
            if (s.index + offset >= 32) return bad(in, "temp register out of range");
            temps |= 1u << (s.index + offset);
            out = "r" + std::to_string(s.index + offset);
            return true;
        case reg::kInput:
            if (offset) return bad(in, "matrix rows on an input register");
            if (s.index >= (vs ? 16u : sm3 ? 10u : 2u)) return bad(in, "input register out of range");
            if (sm3 && !vs) {
                const Decl& d = vin[s.index];
                if (!d.present) return bad(in, "input register read without dcl");
                if (d.usage == Usage::Color) {
                    vread |= static_cast<uint8_t>(1u << d.index);
                    vread_mask[d.index] |= d.mask;
                } else if (d.usage == Usage::Texcoord) {
                    tcread |= static_cast<uint8_t>(1u << d.index);
                    tc_mask[d.index] |= d.mask;
                }
                sm3_in |= static_cast<uint16_t>(1u << s.index);
                out = "v" + std::to_string(s.index);
                return true;
            }
            if (vs) {
                if (!vin[s.index].present) return bad(in, "vertex input read without dcl");
                vin_read |= static_cast<uint16_t>(1u << s.index);
            } else {
                if (p.major >= 2 && !vin[s.index].present) return bad(in, "color input read without dcl");
                vread |= static_cast<uint8_t>(1u << s.index);
                vread_mask[s.index] = 0xF;
            }
            out = "v" + std::to_string(s.index);
            return true;
        case reg::kConst:
            return const_ref(in, s, offset, out);
        case reg::kAddr:
            if (offset) return bad(in, "matrix rows on an address/texture register");
            if (!vs && s.index >= 8) return bad(in, "texture register out of range");
            if (vs) {
                a0 = true;
                out = "float4(a0)";
            } else if (ps1 && !ps14) {
                tregs |= static_cast<uint8_t>(1u << s.index);
                out = "t" + std::to_string(s.index);
            } else if (ps14) {
                tcread |= static_cast<uint8_t>(1u << s.index);
                tc_mask[s.index] = 0xF;
                out = "tc" + std::to_string(s.index);
            } else {
                if (!tin[s.index].present) return bad(in, "texture coordinate read without dcl");
                tcread |= static_cast<uint8_t>(1u << s.index);
                tc_mask[s.index] |= tin[s.index].mask;
                out = "tc" + std::to_string(s.index);
            }
            return true;
        case reg::kConstInt:
            if (offset) return bad(in, "matrix rows on an integer register");
            if (s.index >= 16) return bad(in, "integer register out of range");
            out = "float4(" + int_reg(s.index) + ")";
            return true;
        case reg::kMiscType:
            if (offset || vs || !sm3 || s.index >= 2) return bad(in, "vPos / vFace outside ps_3_0");
            if (s.index == 0) vpos = true;
            else vface = true;
            out = s.index == 0 ? "vPos" : "vFace";
            return true;
        default:
            return bad(in, "register type not readable in arithmetic");
        }
    }

    bool apply_mod(const Instr& in, const Src& s, const std::string& b, std::string& out) {
        switch (s.mod) {
        case SrcMod::None: out = b; return true;
        case SrcMod::Neg: out = "(-" + b + ")"; return true;
        case SrcMod::Bias: out = "(" + b + " - 0.5)"; return true;
        case SrcMod::BiasNeg: out = "(-(" + b + " - 0.5))"; return true;
        case SrcMod::Sign: out = "((" + b + " - 0.5) * 2.0)"; return true;
        case SrcMod::SignNeg: out = "(-((" + b + " - 0.5) * 2.0))"; return true;
        case SrcMod::Comp: out = "(1.0 - " + b + ")"; return true;
        case SrcMod::X2: out = "(" + b + " * 2.0)"; return true;
        case SrcMod::X2Neg: out = "(-(" + b + " * 2.0))"; return true;
        case SrcMod::Abs: out = "abs(" + b + ")"; return true;
        case SrcMod::AbsNeg: out = "(-abs(" + b + "))"; return true;
        case SrcMod::Dz: case SrcMod::Dw: case SrcMod::Not:
            return bad(in, "source modifier not valid on this instruction");
        }
        return bad(in, "unknown source modifier");
    }

    // Swizzled + modified float4 source.
    bool src(const Instr& in, int i, std::string& out, uint32_t offset = 0) {
        const Src& s = in.src[static_cast<size_t>(i)];
        std::string b;
        if (!base(in, s, offset, b)) return false;
        if (!(s.swz[0] == 0 && s.swz[1] == 1 && s.swz[2] == 2 && s.swz[3] == 3)) {
            b += '.';
            for (int k = 0; k < 4; ++k) b += kComp[s.swz[static_cast<size_t>(k)]];
        }
        return apply_mod(in, s, b, out);
    }
    // Scalar operand: swizzle selector 3 (".w" without a swizzle; any replicate swizzle).
    bool sca(const Instr& in, int i, std::string& out) {
        const Src& s = in.src[static_cast<size_t>(i)];
        std::string b;
        if (!base(in, s, 0, b)) return false;
        b += '.';
        b += kComp[s.swz[3]];
        return apply_mod(in, s, b, out);
    }
    // Source for texture-addressing ops: modifiers _dz/_dw are handled by the caller.
    bool tex_src(const Instr& in, int i, std::string& out, SrcMod* mod_out = nullptr) {
        Src s = in.src[static_cast<size_t>(i)];
        const SrcMod m = s.mod;
        if (m == SrcMod::Dz || m == SrcMod::Dw) s.mod = SrcMod::None;
        std::string b;
        if (!base(in, s, 0, b)) return false;
        if (!(s.swz[0] == 0 && s.swz[1] == 1 && s.swz[2] == 2 && s.swz[3] == 3)) {
            b += '.';
            for (int k = 0; k < 4; ++k) b += kComp[s.swz[static_cast<size_t>(k)]];
        }
        if (mod_out) *mod_out = m;
        if (m == SrcMod::Dz || m == SrcMod::Dw) {
            out = b;
            return true;
        }
        return apply_mod(in, s, b, out);
    }

    // ------------------------------------------------------------ destination
    bool dst_name(const Instr& in, std::string& out) {
        const Dst& d = in.dst;
        switch (d.type) {
        case reg::kTemp:
            if (d.index >= 32) return bad(in, "temp register out of range");
            temps |= 1u << d.index;
            out = "r" + std::to_string(d.index);
            return true;
        case reg::kAddr:
            if (vs) {
                a0 = true;
                out = "a0";
            } else {
                if (d.index >= 8) return bad(in, "texture register out of range");
                tregs |= static_cast<uint8_t>(1u << d.index);
                out = "t" + std::to_string(d.index);
            }
            return true;
        case reg::kRastOut:
            if (d.index == 0) { opos |= d.mask; out = "oPos"; }
            else if (d.index == 1) { ofog |= d.mask; out = "oFog"; }
            else { opts |= d.mask; out = "oPts"; }
            return true;
        case reg::kAttrOut:
            if (d.index >= 2) return bad(in, "output register out of range");
            od[d.index] |= d.mask;
            out = "oD" + std::to_string(d.index);
            return true;
        case reg::kTexCrdOut:
            if (sm3) {
                if (d.index >= 12 || !vout[d.index].present) return bad(in, "output register written without dcl");
                oregs |= static_cast<uint16_t>(1u << d.index);
                out = "o" + std::to_string(d.index);
                return true;
            }
            if (d.index >= 8) return bad(in, "output register out of range");
            ot[d.index] |= d.mask;
            out = "oT" + std::to_string(d.index);
            return true;
        case reg::kColorOut:
            if (d.index >= 4) return bad(in, "output register out of range");
            oc[d.index] |= d.mask;
            out = "oC" + std::to_string(d.index);
            return true;
        case reg::kDepthOut:
            depth = true;
            odepth |= d.mask;
            out = "oDepth";
            return true;
        case reg::kPredicate:
            p0 = true;
            out = "p0";
            return true;
        default:
            return bad(in, "register type not writable");
        }
    }

    // value: name of a float4 temporary (or bool4 for setp).
    bool write(const Instr& in, const std::string& value, std::string& post) {
        const Dst& d = in.dst;
        if (d.mask == 0) return true;
        std::string name;
        if (!dst_name(in, name)) return false;
        const std::string m = d.mask == 0xF ? std::string() : mask_str(d.mask);
        std::string v = value;
        if (d.type == reg::kPredicate) {
            if (d.shift || d.saturate) return bad(in, "modifier on a predicate write");
        } else {
            static const char* const kShift[] = {"0.125", "0.25", "0.5", "", "2.0", "4.0", "8.0"};
            if (d.shift) v = "(" + v + " * " + kShift[d.shift + 3] + ")";
            if (d.saturate) v = "saturate(" + v + ")";
            if (ps1 && (d.type == reg::kTemp || d.type == reg::kTexture)) v = "clamp(" + v + ", -" + range + ", " + range + ")";
            if (vs && d.type == reg::kAddr) v = (p.major >= 2 ? "xl_addr_round(" : "xl_addr_floor(") + v + ")";
        }
        const std::string vm = d.mask == 0xF ? v : "(" + v + ")" + m;
        if (in.predicated) {
            p0 = true;
            std::string pm;
            for (int i = 0; i < 4; ++i)
                if (d.mask & (1u << i)) pm += kComp[in.pred.swz[static_cast<size_t>(i)]];
            std::string pr = "p0." + pm;
            if (in.pred.mod == SrcMod::Not) pr = "!" + pr;
            else if (in.pred.mod != SrcMod::None) return bad(in, "predicate modifier");
            line_to(post, name + m + " = select(" + pr + ", " + vm + ", " + name + m + ");");
        } else {
            line_to(post, name + m + " = " + vm + ";");
        }
        return true;
    }

    // ------------------------------------------------------------ texture sampling
    bool use_sampler(const Instr& in, uint32_t slot) {
        if (slot >= 16) return bad(in, "sampler out of range");
        samplers |= static_cast<uint16_t>(1u << slot);
        return true;
    }
    // Texture coordinate input n (ps_1_x stage coordinates, ps_2_x t#).
    std::string tc(uint32_t n, uint8_t mask = 0xF) {
        if (n >= 8) {
            tc_out_of_range = true;
            return "tc0";
        }
        tcread |= static_cast<uint8_t>(1u << n);
        tc_mask[n] |= mask;
        return "tc" + std::to_string(n);
    }
    std::string sample(uint32_t slot, const std::string& coord) { return "xl_tex" + std::to_string(slot) + "(" + coord + ")"; }

    // ------------------------------------------------------------ instructions
    bool emit(const Instr& in, std::string& pre, std::string& post) {
        std::string s0, s1, s2, s3;
        auto value = [&](const std::string& expr) {
            const std::string t = newtmp();
            line_to(pre, "float4 " + t + " = " + expr + ";");
            return write(in, t, post);
        };
        auto scalar = [&](const std::string& expr) { return value("(" + expr + ").xxxx"); };
        const uint32_t m = in.dst.index;  // texture stage of ps_1_x texture ops
        // ps_1_x destinations are r0-r5 / t0-t7; m indexes stage-sized (8 / 16) tables below.
        if (ps1 && in.has_dst && m >= 8) return bad(in, "ps_1_x register index out of range");
        switch (in.op) {
        case Op::Nop: case Op::Phase: return true;
        case Op::Mov:
            if (!src(in, 0, s0)) return false;
            return value(s0);
        case Op::Mova:
            if (!vs || in.dst.type != reg::kAddr) return bad(in, "mova outside a0");
            if (!src(in, 0, s0)) return false;
            return value(s0);
        case Op::Add: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("(" + s0 + " + " + s1 + ")");
        case Op::Sub: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("(" + s0 + " - " + s1 + ")");
        case Op::Mul: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("xl_mul(" + s0 + ", " + s1 + ")");
        case Op::Mad:
            if (!src(in, 0, s0) || !src(in, 1, s1) || !src(in, 2, s2)) return false;
            return value("(xl_mul(" + s0 + ", " + s1 + ") + " + s2 + ")");
        case Op::Dp3: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return scalar("xl_dp3(" + s0 + ", " + s1 + ")");
        case Op::Dp4: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return scalar("xl_dp4(" + s0 + ", " + s1 + ")");
        case Op::Min: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("xl_min(" + s0 + ", " + s1 + ")");
        case Op::Max: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("xl_max(" + s0 + ", " + s1 + ")");
        case Op::Slt: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("xl_slt(" + s0 + ", " + s1 + ")");
        case Op::Sge: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("xl_sge(" + s0 + ", " + s1 + ")");
        case Op::Rcp: if (!sca(in, 0, s0)) return false; return scalar("xl_rcp(" + s0 + ")");
        case Op::Rsq: if (!sca(in, 0, s0)) return false; return scalar("xl_rsq(" + s0 + ")");
        case Op::Exp: if (!sca(in, 0, s0)) return false; return scalar("exp2(" + s0 + ")");
        case Op::Log: if (!sca(in, 0, s0)) return false; return scalar("xl_log(" + s0 + ")");
        case Op::ExpP:
            if (!sca(in, 0, s0)) return false;
            if (vs && p.major == 1) return value("xl_expp1(" + s0 + ")");
            return scalar("exp2(" + s0 + ")");
        case Op::LogP:
            if (!sca(in, 0, s0)) return false;
            if (vs && p.major == 1) return value("xl_logp1(" + s0 + ")");
            return scalar("xl_log(" + s0 + ")");
        case Op::Frc: if (!src(in, 0, s0)) return false; return value("xl_frc(" + s0 + ")");
        case Op::Abs: if (!src(in, 0, s0)) return false; return value("abs(" + s0 + ")");
        case Op::Sgn: if (!src(in, 0, s0)) return false; return value("xl_sgn(" + s0 + ")");
        case Op::Nrm: if (!src(in, 0, s0)) return false; return value("xl_nrm(" + s0 + ")");
        case Op::Lit: if (!src(in, 0, s0)) return false; return value("xl_lit(" + s0 + ")");
        case Op::Dst: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("xl_dst(" + s0 + ", " + s1 + ")");
        case Op::Crs: if (!src(in, 0, s0) || !src(in, 1, s1)) return false; return value("xl_crs(" + s0 + ", " + s1 + ")");
        case Op::Lrp:
            if (!src(in, 0, s0) || !src(in, 1, s1) || !src(in, 2, s2)) return false;
            return value("(xl_mul(" + s0 + ", (" + s1 + " - " + s2 + ")) + " + s2 + ")");
        case Op::Pow:
            if (!sca(in, 0, s0) || !sca(in, 1, s1)) return false;
            return scalar("xl_pow(" + s0 + ", " + s1 + ")");
        case Op::SinCos:
            if (!sca(in, 0, s0)) return false;
            return value("xl_sincos(" + s0 + ")");
        case Op::Dp2Add:
            if (!src(in, 0, s0) || !src(in, 1, s1) || !sca(in, 2, s2)) return false;
            return scalar("xl_dp2add(" + s0 + ", " + s1 + ", " + s2 + ")");
        case Op::Cmp:
            if (!src(in, 0, s0) || !src(in, 1, s1) || !src(in, 2, s2)) return false;
            return value("select(" + s0 + " >= 0.0, " + s1 + ", " + s2 + ")");
        case Op::Cnd:
            if (!src(in, 0, s0) || !src(in, 1, s1) || !src(in, 2, s2)) return false;
            return value("select(" + s0 + " > 0.5, " + s1 + ", " + s2 + ")");
        case Op::M4x4: case Op::M4x3: case Op::M3x4: case Op::M3x3: case Op::M3x2: {
            const uint32_t rows = in.op == Op::M4x4 || in.op == Op::M3x4 ? 4u : in.op == Op::M3x2 ? 2u : 3u;
            const bool dp4 = in.op == Op::M4x4 || in.op == Op::M4x3;
            if (!src(in, 0, s0)) return false;
            const Src& r = in.src[1];
            if (!r.relative && r.type == reg::kConst && r.index + rows > (vs ? 256u : 32u)) return bad(in, "matrix rows out of range");
            if (r.type == reg::kTemp && r.index + rows > 32u) return bad(in, "matrix rows out of range");
            std::string e = "float4(";
            for (uint32_t k = 0; k < 4; ++k) {
                if (k) e += ", ";
                if (k < rows) {
                    std::string row;
                    if (!src(in, 1, row, k)) return false;
                    e += std::string(dp4 ? "xl_dp4(" : "xl_dp3(") + s0 + ", " + row + ")";
                } else {
                    e += "0.0";
                }
            }
            return value(e + ")");
        }
        case Op::Dsx: if (!src(in, 0, s0)) return false; return value("ddx(" + s0 + ")");
        case Op::Dsy: if (!src(in, 0, s0)) return false; return value("ddy(" + s0 + ")");
        case Op::Setp: {
            if (!src(in, 0, s0) || !src(in, 1, s1)) return false;
            static const char* const ops[] = {"", ">", "==", ">=", "<", "!=", "<=", ""};
            const uint8_t c = in.specific & 7u;
            if (c == 0 || c == 7) return bad(in, "setp comparison");
            const std::string t = newtmp();
            line_to(pre, "bool4 " + t + " = (" + s0 + " " + ops[c] + " " + s1 + ");");
            return write(in, t, post);
        }
        case Op::Bem: {
            if (!ps14) return bad(in, "bem outside ps_1_4");
            if (!src(in, 0, s0) || !src(in, 1, s1)) return false;
            bump = true;
            const std::string b = creg(16 + m);
            return value("float4(" + s0 + ".x + " + b + ".x * " + s1 + ".x + " + b + ".z * " + s1 + ".y, " + s0 + ".y + " + b +
                         ".y * " + s1 + ".x + " + b + ".w * " + s1 + ".y, 0.0, 0.0)");
        }
        // ---------------------------------------------------- pixel texture ops
        case Op::TexCoord:
            if (vs) return bad(in, "texture op in a vertex shader");
            if (!ps14) return value("saturate(float4(" + tc(m, 7) + ".xyz, 1.0))");
            {
                SrcMod md = SrcMod::None;
                if (!tex_src(in, 0, s0, &md)) return false;
                if (md == SrcMod::Dz) return value("float4(" + s0 + ".xy / " + s0 + ".z, 1.0, 1.0)");
                if (md == SrcMod::Dw) return value("float4(" + s0 + ".xy / " + s0 + ".w, 1.0, 1.0)");
                return value("float4(" + s0 + ".xyz, 1.0)");
            }
        case Op::Tex: {
            if (vs) return bad(in, "texture op in a vertex shader");
            if (p.major >= 2) {
                const Src& sm = in.src[1];
                if (sm.type != reg::kSampler) return bad(in, "texld without a sampler");
                if (!use_sampler(in, sm.index) || !tex_src(in, 0, s0)) return false;
                const std::string n = std::to_string(sm.index);
                if (in.specific & kTexldProject) return value(sample(sm.index, "xl_proj(" + s0 + ")"));
                if (in.specific & kTexldBias) {
                    sampler_bias |= static_cast<uint16_t>(1u << sm.index);
                    return value("xl_texb" + n + "(" + s0 + ")");
                }
                return value(sample(sm.index, s0));
            }
            if (!use_sampler(in, m)) return false;
            if (!ps14) {
                const std::string c = tc(m, sampler_dim(p, opt, m) == TextureDim::Tex2D ? 3 : 7);
                if (opt.projected_stages & (1u << m)) {
                    tc_mask[m] |= 0xF;
                    return value(sample(m, "xl_proj(" + c + ")"));
                }
                return value(sample(m, c));
            }
            SrcMod md = SrcMod::None;
            if (!tex_src(in, 0, s0, &md)) return false;
            if (md == SrcMod::Dz) return value(sample(m, "float4(" + s0 + ".xy / " + s0 + ".z, " + s0 + ".zw)"));
            if (md == SrcMod::Dw) return value(sample(m, "xl_proj(" + s0 + ")"));
            return value(sample(m, s0));
        }
        case Op::TexLdd: {
            if (vs || p.major < 2) return bad(in, "texldd outside ps_2_x");
            const Src& sm = in.src[1];
            if (sm.type != reg::kSampler) return bad(in, "texldd without a sampler");
            if (!use_sampler(in, sm.index) || !tex_src(in, 0, s0) || !src(in, 2, s2) || !src(in, 3, s3)) return false;
            sampler_grad |= static_cast<uint16_t>(1u << sm.index);
            return value("xl_texg" + std::to_string(sm.index) + "(" + s0 + ", " + s2 + ", " + s3 + ")");
        }
        case Op::TexLdl: {
            if (vs) return bad(in, "texldl in a vertex shader (vertex texture fetch) is not supported");
            const Src& sm = in.src[1];
            if (sm.type != reg::kSampler) return bad(in, "texldl without a sampler");
            if (!use_sampler(in, sm.index) || !tex_src(in, 0, s0)) return false;
            sampler_lod |= static_cast<uint16_t>(1u << sm.index);
            return value("xl_texl" + std::to_string(sm.index) + "(" + s0 + ")");
        }
        case Op::TexKill: {
            if (vs) return bad(in, "texkill in a vertex shader");
            discard_ = true;
            std::string v;
            uint8_t mask = 7;
            if (ps1 && !ps14) {
                v = tc(in.dst.index, 7);
            } else {
                Src s;
                s.type = in.dst.type;
                s.index = in.dst.index;
                if (!base(in, s, 0, v)) return false;
                if (!ps1) mask = in.dst.mask;
            }
            if (mask == 0) return true;
            line_to(pre, "if (any(" + v + mask_str(mask) + " < 0.0)) discard;");
            return true;
        }
        case Op::TexBem: case Op::TexBemL: {
            if (!ps1 || ps14) return bad(in, "texbem outside ps_1_1..1_3");
            if (!use_sampler(in, m) || !tex_src(in, 0, s0)) return false;
            bump = true;
            const std::string b = creg(16 + m);
            const std::string c = tc(m, 0xF);
            const std::string coord = "float4(" + c + ".x + " + b + ".x * " + s0 + ".x + " + b + ".z * " + s0 + ".y, " + c +
                                      ".y + " + b + ".y * " + s0 + ".x + " + b + ".w * " + s0 + ".y, " + c + ".zw)";
            if (in.op == Op::TexBem) return value(sample(m, coord));
            const std::string t = newtmp();
            line_to(pre, "float4 " + t + " = " + sample(m, coord) + ";");
            const std::string l = creg(24 + m);
            return value("float4(" + t + ".xyz * saturate(" + l + ".x * " + s0 + ".z + " + l + ".y), " + t + ".w)");
        }
        case Op::TexReg2Ar: case Op::TexReg2Gb: case Op::TexReg2Rgb: {
            if (!ps1 || ps14) return bad(in, "texreg2* outside ps_1_1..1_3");
            if (!use_sampler(in, m) || !tex_src(in, 0, s0)) return false;
            const char* sw = in.op == Op::TexReg2Ar ? ".wxzw" : in.op == Op::TexReg2Gb ? ".yzzw" : ".xyzw";
            return value(sample(m, s0 + sw));
        }
        case Op::TexM3x2Pad: case Op::TexM3x3Pad: {
            if (!ps1 || ps14) return bad(in, "texm* outside ps_1_1..1_3");
            if (!tex_src(in, 0, s0)) return false;
            pads |= static_cast<uint16_t>(1u << m);
            line_to(post, "pad" + std::to_string(m) + " = dot(" + tc(m, 7) + ".xyz, " + s0 + ".xyz);");
            return true;
        }
        case Op::TexM3x2Tex: case Op::TexM3x2Depth: {
            if (!ps1 || ps14 || m < 1) return bad(in, "texm3x2 outside ps_1_1..1_3");
            if (!(pads & (1u << (m - 1)))) return bad(in, "texm3x2 without texm3x2pad");
            if (!tex_src(in, 0, s0)) return false;
            const std::string u = "pad" + std::to_string(m - 1);
            const std::string v = "dot(" + tc(m, 7) + ".xyz, " + s0 + ".xyz)";
            if (in.op == Op::TexM3x2Tex) {
                if (!use_sampler(in, m)) return false;
                return value(sample(m, "float4(" + u + ", " + v + ", 0.0, 1.0)"));
            }
            depth = true;
            const std::string t = newtmp();
            line_to(pre, "float " + t + " = " + v + ";");
            line_to(post, "oDepth.x = " + t + " == 0.0 ? 1.0 : " + u + " / " + t + ";");
            return true;
        }
        case Op::TexM3x3Tex: case Op::TexM3x3: case Op::TexM3x3Spec: case Op::TexM3x3VSpec: {
            if (!ps1 || ps14 || m < 2) return bad(in, "texm3x3 outside ps_1_1..1_3");
            if (!(pads & (1u << (m - 1))) || !(pads & (1u << (m - 2)))) return bad(in, "texm3x3 without two texm3x3pad");
            if (!tex_src(in, 0, s0)) return false;
            const std::string nvec = "float3(pad" + std::to_string(m - 2) + ", pad" + std::to_string(m - 1) + ", dot(" +
                                     tc(m, 7) + ".xyz, " + s0 + ".xyz))";
            if (in.op == Op::TexM3x3) return value("float4(" + nvec + ", 1.0)");
            if (!use_sampler(in, m)) return false;
            if (in.op == Op::TexM3x3Tex) return value(sample(m, "float4(" + nvec + ", 1.0)"));
            std::string e;
            if (in.op == Op::TexM3x3Spec) {
                if (!src(in, 1, s1)) return false;
                e = s1 + ".xyz";
            } else {
                e = "float3(" + tc(m - 2, 8) + ".w, " + tc(m - 1, 8) + ".w, " + tc(m, 8) + ".w)";
            }
            const std::string n = newtmp(), ev = newtmp(), nn = newtmp(), r = newtmp();
            line_to(pre, "float3 " + n + " = " + nvec + ";");
            line_to(pre, "float3 " + ev + " = " + e + ";");
            line_to(pre, "float " + nn + " = dot(" + n + ", " + n + ");");
            line_to(pre, "float3 " + r + " = " + nn + " != 0.0 ? 2.0 * " + n + " * (dot(" + n + ", " + ev + ") / " + nn +
                             ") - " + ev + " : -" + ev + ";");
            return value(sample(m, "float4(" + r + ", 1.0)"));
        }
        case Op::TexDp3Tex: case Op::TexDp3: {
            if (!ps1 || ps14) return bad(in, "texdp3 outside ps_1_2..1_3");
            if (!tex_src(in, 0, s0)) return false;
            const std::string d = "dot(" + tc(m, 7) + ".xyz, " + s0 + ".xyz)";
            if (in.op == Op::TexDp3) return value("(" + d + ").xxxx");
            if (!use_sampler(in, m)) return false;
            return value(sample(m, "float4(" + d + ", 0.0, 0.0, 1.0)"));
        }
        case Op::TexDepth: {
            if (!ps14 || in.dst.type != reg::kTemp || in.dst.index != 5) return bad(in, "texdepth outside ps_1_4 r5");
            temps |= 1u << 5;
            depth = true;
            line_to(post, "oDepth.x = r5.y == 0.0 ? 1.0 : r5.x / r5.y;");
            return true;
        }
        default:
            return bad(in, "unsupported instruction");
        }
    }

    // ------------------------------------------------------------ control flow
    bool cond_scalar(const Instr& in, int i, std::string& out) { return sca(in, i, out); }

    bool control(const Instr& in) {
        static const char* const ops[] = {"", ">", "==", ">=", "<", "!=", "<=", ""};
        std::string a, b;
        auto pred_expr = [&](const Src& s, std::string& out) {
            if (s.type != reg::kPredicate) return false;
            p0 = true;
            out = std::string("p0.") + kComp[s.swz[0]];
            if (s.mod == SrcMod::Not) out = "!" + out;
            return true;
        };
        auto bool_expr = [&](const Src& s, std::string& out) {
            if (s.type == reg::kConstBool) {
                if (s.index >= 16) return false;
                out = bool_reg(s.index);
                if (s.mod == SrcMod::Not) out = "!" + out;
                return true;
            }
            return pred_expr(s, out);
        };
        switch (in.op) {
        case Op::If:
            if (!bool_expr(in.src[0], a)) return bad(in, "if operand");
            line("if (" + a + ") {");
            ++indent;
            cf.push_back({'i', 0, false});
            return true;
        case Op::Ifc: {
            const uint8_t c = in.specific & 7u;
            if (c == 0 || c == 7) return bad(in, "ifc comparison");
            if (!cond_scalar(in, 0, a) || !cond_scalar(in, 1, b)) return false;
            line("if (" + a + " " + ops[c] + " " + b + ") {");
            ++indent;
            cf.push_back({'i', 0, false});
            return true;
        }
        case Op::Else:
            if (cf.empty() || cf.back().kind != 'i' || cf.back().has_else) return bad(in, "else without if");
            cf.back().has_else = true;
            --indent;
            line("} else {");
            ++indent;
            return true;
        case Op::EndIf:
            if (cf.empty() || cf.back().kind != 'i') return bad(in, "endif without if");
            cf.pop_back();
            --indent;
            line("}");
            return true;
        case Op::Rep: {
            if (in.src[0].type != reg::kConstInt || in.src[0].index >= 16) return bad(in, "rep operand");
            const std::string n = std::to_string(loops);
            line("{");
            ++indent;
            line("int xl_rc" + n + " = clamp(" + int_reg(in.src[0].index) + ".x, 0, 255);");
            line("for (int xl_ri" + n + " = 0; xl_ri" + n + " < xl_rc" + n + "; ++xl_ri" + n + ") {");
            ++indent;
            cf.push_back({'r', loops++, false});
            return true;
        }
        case Op::EndRep:
            if (cf.empty() || cf.back().kind != 'r') return bad(in, "endrep without rep");
            cf.pop_back();
            --indent;
            line("}");
            --indent;
            line("}");
            return true;
        case Op::Loop: {
            if (in.src[0].type != reg::kLoop || in.src[1].type != reg::kConstInt || in.src[1].index >= 16) return bad(in, "loop operands");
            aL = true;
            const std::string n = std::to_string(loops);
            const std::string iv = int_reg(in.src[1].index);
            line("{");
            ++indent;
            line("int4 xl_lp" + n + " = " + iv + ";");
            line("int xl_ls" + n + " = aL;");
            line("aL = xl_lp" + n + ".y;");
            line("for (int xl_li" + n + " = 0; xl_li" + n + " < clamp(xl_lp" + n + ".x, 0, 255); ++xl_li" + n + ", aL += xl_lp" +
                 n + ".z) {");
            ++indent;
            cf.push_back({'l', loops++, false});
            return true;
        }
        case Op::EndLoop: {
            if (cf.empty() || cf.back().kind != 'l') return bad(in, "endloop without loop");
            const std::string n = std::to_string(cf.back().id);
            cf.pop_back();
            --indent;
            line("}");
            line("aL = xl_ls" + n + ";");
            --indent;
            line("}");
            return true;
        }
        case Op::Break: case Op::Breakc: case Op::Breakp: {
            bool inloop = false;
            for (const Cf& c : cf) inloop |= c.kind != 'i';
            if (!inloop) return bad(in, "break outside a loop");
            if (in.op == Op::Break) {
                line("break;");
            } else if (in.op == Op::Breakp) {
                if (!pred_expr(in.src[0], a)) return bad(in, "breakp operand");
                line("if (" + a + ") break;");
            } else {
                const uint8_t c = in.specific & 7u;
                if (c == 0 || c == 7) return bad(in, "breakc comparison");
                if (!cond_scalar(in, 0, a) || !cond_scalar(in, 1, b)) return false;
                line("if (" + a + " " + ops[c] + " " + b + ") break;");
            }
            return true;
        }
        case Op::Call: case Op::CallNz: {
            if (in.src[0].type != reg::kLabel) return bad(in, "call operand");
            called.push_back(in.src[0].index);
            const std::string f = "xl_sub" + std::to_string(in.src[0].index) + "();";
            if (in.op == Op::Call) {
                line(f);
            } else {
                if (!bool_expr(in.src[1], a)) return bad(in, "callnz operand");
                line("if (" + a + ") " + f);
            }
            return true;
        }
        case Op::Ret:
            if (cf.empty()) {
                if (!in_sub) main_done = true;
                else {
                    in_sub = false;
                    cur = nullptr;
                }
                return true;
            }
            line("return;");
            return true;
        case Op::Label: {
            if (!cf.empty()) return bad(in, "label inside a block");
            if (in.src[0].type != reg::kLabel) return bad(in, "label operand");
            const uint32_t l = in.src[0].index;
            if (subs.count(l)) return bad(in, "duplicate label");
            main_done = true;
            in_sub = true;
            cur = &subs[l];
            indent = 1;
            return true;
        }
        default:
            return bad(in, "unsupported control flow");
        }
    }

    static bool is_control(Op op) {
        switch (op) {
        case Op::If: case Op::Ifc: case Op::Else: case Op::EndIf: case Op::Rep: case Op::EndRep: case Op::Loop:
        case Op::EndLoop: case Op::Break: case Op::Breakc: case Op::Breakp: case Op::Call: case Op::CallNz:
        case Op::Ret: case Op::Label:
            return true;
        default:
            return false;
        }
    }

    // ------------------------------------------------------------ driver
    bool declarations() {
        for (const Instr& in : p.instrs) {
            // parse() guarantees these ranges; re-checked for hand-built programs.
            if ((in.op == Op::Def && in.dst.index >= 256) || ((in.op == Op::Defi || in.op == Op::Defb) && in.dst.index >= 16) ||
                (in.op == Op::Dcl && in.dst.type != reg::kSampler && in.dst.index >= (in.dst.type == reg::kTexture && !vs ? 8u : 16u)))
                return bad(in, "declaration register out of range");
            if (in.op == Op::Def) {
                cdef[in.dst.index] = true;
                cval[in.dst.index] = in.literal;
            } else if (in.op == Op::Defi) {
                idef[in.dst.index] = true;
                for (int k = 0; k < 4; ++k) ival[in.dst.index][static_cast<size_t>(k)] = static_cast<int32_t>(in.literal[static_cast<size_t>(k)]);
            } else if (in.op == Op::Defb) {
                bdef[in.dst.index] = true;
                bval[in.dst.index] = in.literal[0];
            } else if (in.op == Op::Dcl) {
                const Dst& d = in.dst;
                if (d.type == reg::kSampler) continue;
                Decl dc;
                dc.present = true;
                dc.usage = in.usage;
                dc.index = in.usage_index;
                dc.mask = d.mask ? d.mask : 0xF;
                dc.centroid = d.centroid;
                if (vs && sm3 && d.type == reg::kOutput) {
                    if (d.index >= 12 || vout[d.index].present) return bad(in, "duplicate or out-of-range output dcl");
                    const bool known = (in.usage == Usage::Position && in.usage_index == 0) ||
                                       (in.usage == Usage::Color && in.usage_index < 2) ||
                                       (in.usage == Usage::Texcoord && in.usage_index < 8) ||
                                       ((in.usage == Usage::Fog || in.usage == Usage::PSize) && in.usage_index == 0);
                    if (!known) return bad(in, "vs_3_0 output usage without a varying slot (POSITION0, COLOR0-1, TEXCOORD0-7, FOG, PSIZE)");
                    for (const Decl& o : vout)
                        if (o.present && o.usage == in.usage && o.index == in.usage_index) return bad(in, "two outputs declare the same usage");
                    vout[d.index] = dc;
                } else if (vs) {
                    if (d.type != reg::kInput) return bad(in, "output dcl in a vertex shader before SM3");
                    if (vin[d.index].present) return bad(in, "duplicate dcl");
                    vin[d.index] = dc;
                } else if (sm3 && d.type == reg::kInput) {
                    if (d.index >= 10 || vin[d.index].present) return bad(in, "duplicate or out-of-range input dcl");
                    const bool known = (in.usage == Usage::Color && in.usage_index < 2) ||
                                       (in.usage == Usage::Texcoord && in.usage_index < 8) ||
                                       (in.usage == Usage::Fog && in.usage_index == 0);
                    if (!known) return bad(in, "ps_3_0 input usage without a varying slot (COLOR0-1, TEXCOORD0-7, FOG)");
                    for (const Decl& o : vin)
                        if (o.present && o.usage == in.usage && o.index == in.usage_index) return bad(in, "two inputs declare the same usage");
                    vin[d.index] = dc;
                } else if (sm3 && d.type == reg::kMiscType) {
                    continue;  // dcl vPos / vFace
                } else if (d.type == reg::kInput) {
                    vin[d.index] = dc;
                } else if (d.type == reg::kTexture) {
                    tin[d.index] = dc;
                } else {
                    return bad(in, "dcl register");
                }
            }
        }
        // Two VS inputs with the same (usage, index) would share a location.
        for (size_t i = 0; i < 16; ++i)
            for (size_t j = i + 1; j < 16; ++j)
                if (vin[i].present && vin[j].present && vs && vin[i].usage == vin[j].usage && vin[i].index == vin[j].index) {
                    if (err) *err = "two vertex inputs declare the same usage";
                    return false;
                }
        if (vs && sm3)
            for (const Instr& in : p.instrs)
                if (in.op == Op::Dcl && in.dst.type == reg::kSampler) return bad(in, "vertex texture fetch (vs_3_0 sampler) is not supported");
        for (uint32_t s = 0; s < 16; ++s) dims[s] = sampler_dim(p, opt, s);
        return true;
    }

    bool body() {
        const auto& v = p.instrs;
        for (size_t i = 0; i < v.size(); ++i) {
            const Instr& in = v[i];
            if (in.op == Op::Dcl || in.op == Op::Def || in.op == Op::Defi || in.op == Op::Defb) continue;
            if (in.coissue) return bad(in, "co-issue without a preceding instruction");
            if (main_done && !in_sub && in.op != Op::Label) {
                if (in.op == Op::Ret) continue;
                return bad(in, "instruction after the end of main");
            }
            if (is_control(in.op)) {
                if (in.predicated && in.op != Op::Breakp && in.op != Op::If && in.op != Op::CallNz) return bad(in, "predicated control flow");
                if (!control(in)) return false;
                continue;
            }
            if (!cur) return bad(in, "instruction outside a subroutine");
            line("// " + disassemble(p, in));
            const bool pair = i + 1 < v.size() && v[i + 1].coissue;
            std::string pre, post;
            if (!emit(in, pre, post)) return false;
            if (tc_out_of_range) return bad(in, "texture coordinate set out of range");
            if (pair) {
                const Instr& in2 = v[i + 1];
                line("// " + disassemble(p, in2));
                std::string pre2, post2;
                if (is_control(in2.op) || !emit(in2, pre2, post2)) return bad(in2, "invalid co-issued instruction");
                if (tc_out_of_range) return bad(in2, "texture coordinate set out of range");
                *cur += pre + pre2 + post + post2;
                ++i;
            } else {
                *cur += pre + post;
            }
        }
        if (!cf.empty()) {
            if (err) *err = "unterminated if/rep/loop block";
            return false;
        }
        for (uint32_t l : called)
            if (!subs.count(l)) {
                if (err) *err = "call to undefined label l" + std::to_string(l);
                return false;
            }
        return true;
    }

    std::string sampler_type(TextureDim d) const {
        return d == TextureDim::Cube ? "TextureCube<float4>" : d == TextureDim::Volume ? "Texture3D<float4>" : "Texture2D<float4>";
    }
    std::string coord_swz(TextureDim d) const { return d == TextureDim::Tex2D ? ".xy" : ".xyz"; }

    std::string assemble(Translation& t) {
        std::string o;
        o += "// Generated by alamo shaderxlat v" + std::to_string(kTranslatorVersion) + " from D3D9 " + p.profile() +
             " bytecode. Do not edit.\n";
        o += "// Semantics: engine/shaderxlat/src/translate.cpp (header table).\n";
        if (opt.instanced) {
            // Q3 instanced form: per-instance register images in a storage buffer (base xl_cb set in the entry point).
            o += vs ? "[[vk::binding(0, 0)]] StructuredBuffer<float4> xl_c;\n" : "[[vk::binding(1, 0)]] StructuredBuffer<float4> xl_c;\n";
            o += "static uint xl_cb;\n";
        } else if (vs) {
            o += "[[vk::binding(0, 0)]] cbuffer VSConstants { float4 c[256]; };\n";
        } else {
            o += "[[vk::binding(1, 0)]] cbuffer PSConstants { float4 c[32]; };\n";
        }
        if (intbool) {
            o += vs ? "[[vk::binding(2, 0)]] cbuffer VSIntConstants" : "[[vk::binding(3, 0)]] cbuffer PSIntConstants";
            o += " { int4 xi[16]; uint4 xb[4]; };\n";
        }
        o += kPush;
        // CTAB names for readability.
        for (const CtabEntry& e : p.ctab) {
            std::string nm;
            for (char ch : e.name) nm += (ch >= 32 && ch < 127 && ch != '\\') ? ch : '?';
            const char* set = e.register_set == 0 ? "b" : e.register_set == 1 ? "i" : e.register_set == 2 ? "c" : "s";
            o += "// CTAB " + std::string(set) + std::to_string(e.register_index) + " x" + std::to_string(e.register_count) + " " +
                 nm + "\n";
        }
        for (uint32_t s = 0; s < 16; ++s) {
            if (!(samplers & (1u << s))) continue;
            const std::string n = std::to_string(s);
            o += "[[vk::combinedImageSampler]][[vk::binding(" + n + ", 1)]] " + sampler_type(dims[s]) + " tex" + n + ";\n";
            o += "[[vk::combinedImageSampler]][[vk::binding(" + n + ", 1)]] SamplerState smp" + n + ";\n";
        }
        for (uint32_t r = 0; r < 256; ++r) {
            if (!cdef[r]) continue;
            const auto& v = cval[r];
            o += "static const float4 k" + std::to_string(r) + " = float4(" + fbits(v[0]) + ", " + fbits(v[1]) + ", " +
                 fbits(v[2]) + ", " + fbits(v[3]) + ");\n";
        }
        o += kHelpers;
        const std::string crel_read = opt.instanced ? "xl_c[xl_cb + uint(i)]" : "c[i]";
        if (vs && relative) {
            o += "float4 xl_crel(int i) {\n    float4 v = float4(0.0, 0.0, 0.0, 0.0);\n    if (uint(i) < 256u) v = " + crel_read + ";\n";
            for (uint32_t r = 0; r < 256; ++r)
                if (cdef[r]) o += "    if (i == " + std::to_string(r) + ") v = k" + std::to_string(r) + ";\n";
            o += "    return v;\n}\n";
        } else if (relative) {
            o += "float4 xl_crel(int i) {\n    float4 v = float4(0.0, 0.0, 0.0, 0.0);\n    if (uint(i) < 32u) v = " + crel_read + ";\n";
            for (uint32_t r = 0; r < (sm3 ? 224u : 32u); ++r)
                if (cdef[r]) o += "    if (i == " + std::to_string(r) + ") v = k" + std::to_string(r) + ";\n";
            o += "    return v;\n}\n";
        }
        for (uint32_t s = 0; s < 16; ++s) {
            if (!(samplers & (1u << s))) continue;
            const std::string n = std::to_string(s), sw = coord_swz(dims[s]);
            o += "float4 xl_tex" + n + "(float4 uv) { return tex" + n + ".Sample(smp" + n + ", uv" + sw + "); }\n";
            if (sampler_bias & (1u << s))
                o += "float4 xl_texb" + n + "(float4 uv) { return tex" + n + ".SampleBias(smp" + n + ", uv" + sw + ", uv.w); }\n";
            if (sampler_grad & (1u << s))
                o += "float4 xl_texg" + n + "(float4 uv, float4 gx, float4 gy) { return tex" + n + ".SampleGrad(smp" + n +
                     ", uv" + sw + ", gx" + sw + ", gy" + sw + "); }\n";
            if (sampler_lod & (1u << s))
                o += "float4 xl_texl" + n + "(float4 uv) { return tex" + n + ".SampleLevel(smp" + n + ", uv" + sw + ", uv.w); }\n";
        }
        if (!vs) o += sm3 ? kEpilogue3 : kEpilogue;

        // Register file (static: shared with subroutines).
        for (uint32_t r = 0; r < 32; ++r)
            if (temps & (1u << r)) o += "static float4 r" + std::to_string(r) + ";\n";
        if (vs) {
            for (uint32_t r = 0; r < 16; ++r)
                if (vin[r].present) o += "static float4 v" + std::to_string(r) + ";\n";
            if (a0) o += "static int4 a0;\n";
            o += "static float4 oPos;\nstatic float4 oFog;\nstatic float4 oPts;\nstatic float4 oD0;\nstatic float4 oD1;\n";
            for (uint32_t r = 0; r < 8; ++r) o += "static float4 oT" + std::to_string(r) + ";\n";
            for (uint32_t r = 0; r < 12; ++r)
                if (vout[r].present) o += "static float4 o" + std::to_string(r) + ";\n";
        } else if (sm3) {
            for (uint32_t r = 0; r < 10; ++r)
                if (sm3_in & (1u << r)) o += "static float4 v" + std::to_string(r) + ";\n";
            if (vpos) o += "static float4 vPos;\n";
            if (vface) o += "static float4 vFace;\n";
            const uint32_t ncol = color_count();
            for (uint32_t r = 0; r < ncol; ++r) o += "static float4 oC" + std::to_string(r) + ";\n";
            if (depth) o += "static float4 oDepth;\n";
        } else {
            for (uint32_t r = 0; r < 2; ++r)
                if (vread & (1u << r)) o += "static float4 v" + std::to_string(r) + ";\n";
            for (uint32_t r = 0; r < 8; ++r)
                if (tcread & (1u << r)) o += "static float4 tc" + std::to_string(r) + ";\n";
            for (uint32_t r = 0; r < 8; ++r)
                if (tregs & (1u << r)) o += "static float4 t" + std::to_string(r) + ";\n";
            for (uint32_t r = 0; r < 16; ++r)
                if (pads & (1u << r)) o += "static float pad" + std::to_string(r) + ";\n";
            const uint32_t ncol = ps1 ? 1u : color_count();
            for (uint32_t r = 0; r < ncol; ++r) o += "static float4 oC" + std::to_string(r) + ";\n";
            if (depth) o += "static float4 oDepth;\n";
        }
        if (aL) o += "static int aL;\n";
        if (p0) o += "static bool4 p0;\n";

        for (const auto& kv : subs) o += "void xl_sub" + std::to_string(kv.first) + "();\n";
        o += "void xl_main() {\n" + main_body + "}\n";
        for (const auto& kv : subs) o += "void xl_sub" + std::to_string(kv.first) + "() {\n" + kv.second + "}\n";

        auto zero_regs = [&](std::string& s) {
            for (uint32_t r = 0; r < 32; ++r)
                if (temps & (1u << r)) s += "    r" + std::to_string(r) + " = float4(0.0, 0.0, 0.0, 0.0);\n";
            if (aL) s += "    aL = 0;\n";
            if (p0) s += "    p0 = bool4(false, false, false, false);\n";
        };
        if (vs) {
            o += "struct VSInput {\n";
            for (const VertexInputDesc& d : t.iface.inputs)
                o += "    [[vk::location(" + std::to_string(d.location) + ")]] float4 v" + std::to_string(d.reg) + " : " +
                     usage_name(d.usage) + std::to_string(d.usage_index) + ";\n";
            if (t.iface.inputs.empty()) o += "    uint vid : SV_VertexID;\n";
            if (opt.instanced) o += "    uint xl_iid : SV_InstanceID;\n";
            o += "};\nstruct VSOutput {\n    float4 pos : SV_Position;\n";
            if (opt.instanced) o += "    [[vk::location(" + std::to_string(kVaryingInstance) + ")]] nointerpolation uint xl_inst : XLINST;\n";
            o += "    [[vk::location(0)]] float4 oD0 : COLOR0;\n    [[vk::location(1)]] float4 oD1 : COLOR1;\n";
            for (uint32_t r = 0; r < 8; ++r)
                o += "    [[vk::location(" + std::to_string(2 + r) + ")]] float4 oT" + std::to_string(r) + " : TEXCOORD" +
                     std::to_string(r) + ";\n";
            o += "    [[vk::location(10)]] float oFog : FOG;\n";
            if (opts) o += "    [[vk::builtin(\"PointSize\")]] float psize : PSIZE;\n";
            o += "};\nVSOutput VSMain(VSInput IN) {\n";
            if (opt.instanced) o += "    xl_cb = (PC.Flags & " + std::to_string(kPushInstancedVs) + "u) != 0u ? IN.xl_iid * 256u : 0u;\n";
            for (const VertexInputDesc& d : t.iface.inputs)
                o += "    v" + std::to_string(d.reg) + " = IN.v" + std::to_string(d.reg) + ";\n";
            zero_regs(o);
            if (a0) o += "    a0 = int4(0, 0, 0, 0);\n";
            o += "    oPos = float4(0.0, 0.0, 0.0, 0.0);\n    oFog = float4(1.0, 0.0, 0.0, 0.0);\n"
                 "    oPts = float4(1.0, 0.0, 0.0, 0.0);\n    oD0 = float4(1.0, 1.0, 1.0, 1.0);\n"
                 "    oD1 = float4(0.0, 0.0, 0.0, 0.0);\n";
            for (uint32_t r = 0; r < 8; ++r) o += "    oT" + std::to_string(r) + " = float4(0.0, 0.0, 0.0, 1.0);\n";
            for (uint32_t r = 0; r < 12; ++r)
                if (vout[r].present) o += "    o" + std::to_string(r) + " = " + sm3_output_default(vout[r].usage, vout[r].index) + ";\n";
            o += "    xl_main();\n";
            for (uint32_t r = 0; r < 12; ++r)
                if (vout[r].present && (oregs & (1u << r)))
                    o += "    " + sm3_output_target(vout[r].usage, vout[r].index) + " = o" + std::to_string(r) + ";\n";
            o += "    VSOutput OUT;\n    OUT.pos = oPos;\n    OUT.pos.xy = oPos.xy + PC.PixelOffset.xy * oPos.w;\n";
            o += sm3 ? "    OUT.oD0 = oD0;\n    OUT.oD1 = oD1;\n" : "    OUT.oD0 = saturate(oD0);\n    OUT.oD1 = saturate(oD1);\n";
            if (opt.instanced) o += "    OUT.xl_inst = IN.xl_iid;\n";
            for (uint32_t r = 0; r < 8; ++r) o += "    OUT.oT" + std::to_string(r) + " = oT" + std::to_string(r) + ";\n";
            o += sm3 ? "    OUT.oFog = oFog.x;\n" : "    OUT.oFog = saturate(oFog.x);\n";
            if (opts) o += "    OUT.psize = oPts.x;\n";
            o += "    return OUT;\n}\n";
        } else {
            o += "struct PSInput {\n    float4 vpos : SV_Position;\n";
            for (uint32_t r = 0; r < 2; ++r)
                if (vread & (1u << r))
                    o += "    [[vk::location(" + std::to_string(r) + ")]] " + std::string(centroid_in(Usage::Color, r) ? "centroid " : "") +
                         "float4 v" + std::to_string(r) + " : COLOR" + std::to_string(r) + ";\n";
            for (uint32_t r = 0; r < 8; ++r)
                if (tcread & (1u << r))
                    o += "    [[vk::location(" + std::to_string(2 + r) + ")]] " + std::string(centroid_in(Usage::Texcoord, r) ? "centroid " : "") +
                         "float4 t" + std::to_string(r) + " : TEXCOORD" + std::to_string(r) + ";\n";
            o += "    [[vk::location(10)]] float fog : FOG;\n";
            if (opt.instanced) o += "    [[vk::location(" + std::to_string(kVaryingInstance) + ")]] nointerpolation uint xl_inst : XLINST;\n";
            if (vface) o += "    bool xl_face : SV_IsFrontFace;\n";
            o += "};\n";
            const uint32_t ncol = ps1 ? 1u : color_count();
            o += "struct PSOutput {\n";
            for (uint32_t r = 0; r < ncol; ++r)
                o += "    float4 color" + std::string(r ? std::to_string(r) : "") + " : SV_Target" + std::to_string(r) + ";\n";
            if (depth) o += "    float depth : SV_Depth;\n";
            o += "};\nPSOutput PSMain(PSInput IN) {\n";
            if (opt.instanced) o += "    xl_cb = (PC.Flags & " + std::to_string(kPushInstancedPs) + "u) != 0u ? IN.xl_inst * 32u : 0u;\n";
            if (sm3) {
                for (uint32_t r = 0; r < 10; ++r) {
                    if (!(sm3_in & (1u << r))) continue;
                    const Decl& d = vin[r];
                    const std::string src = d.usage == Usage::Color      ? "IN.v" + std::to_string(d.index)
                                            : d.usage == Usage::Texcoord ? "IN.t" + std::to_string(d.index)
                                                                         : std::string("float4(IN.fog, 0.0, 0.0, 0.0)");
                    o += "    v" + std::to_string(r) + " = " + src + ";\n";
                }
                if (vpos) o += "    vPos = float4(IN.vpos.xy - 0.5, 0.0, 0.0);\n";
                if (vface) o += "    vFace = IN.xl_face ? float4(1.0, 1.0, 1.0, 1.0) : float4(-1.0, -1.0, -1.0, -1.0);\n";
            }
            for (uint32_t r = 0; r < 2; ++r)
                if (!sm3 && (vread & (1u << r))) o += "    v" + std::to_string(r) + " = saturate(IN.v" + std::to_string(r) + ");\n";
            for (uint32_t r = 0; r < 8; ++r)
                if (!sm3 && (tcread & (1u << r))) o += "    tc" + std::to_string(r) + " = IN.t" + std::to_string(r) + ";\n";
            for (uint32_t r = 0; r < 8; ++r)
                if (tregs & (1u << r)) o += "    t" + std::to_string(r) + " = float4(0.0, 0.0, 0.0, 0.0);\n";
            for (uint32_t r = 0; r < 16; ++r)
                if (pads & (1u << r)) o += "    pad" + std::to_string(r) + " = 0.0;\n";
            zero_regs(o);
            for (uint32_t r = 0; r < ncol; ++r) o += "    oC" + std::to_string(r) + " = float4(0.0, 0.0, 0.0, 0.0);\n";
            if (depth) o += "    oDepth = float4(0.0, 0.0, 0.0, 0.0);\n";
            o += "    xl_main();\n    PSOutput OUT;\n";
            o += sm3 ? std::string("    OUT.color = xl_epilogue(oC0);\n")
                     : std::string("    OUT.color = xl_epilogue(") + (ps1 ? "r0" : "oC0") + ", IN.fog, IN.vpos);\n";
            for (uint32_t r = 1; r < ncol; ++r) o += "    OUT.color" + std::to_string(r) + " = oC" + std::to_string(r) + ";\n";
            if (depth) o += "    OUT.depth = saturate(oDepth.x);\n";
            o += "    return OUT;\n}\n";
        }
        return o;
    }

    // _centroid on the PS input dcl of COLOR r / TEXCOORD r (ps_3_0: whichever v# declares that usage).
    bool centroid_in(Usage u, uint32_t r) const {
        if (!sm3) return u == Usage::Color ? vin[r].centroid : tin[r].centroid;
        for (const Decl& d : vin)
            if (d.present && d.usage == u && d.index == r) return d.centroid;
        return false;
    }

    uint32_t color_count() const {
        uint32_t n = 1;
        for (uint32_t r = 0; r < 4; ++r)
            if (oc[r]) n = r + 1;
        return n;
    }

    void interface(ShaderInterface& f) {
        f.stage = p.type;
        f.major = p.major;
        f.minor = p.minor;
        f.profile = p.profile();
        f.inputs.clear();
        f.varyings.clear();
        f.samplers.clear();
        if (vs && sm3) {
            for (uint32_t r = 0; r < 12; ++r) {
                if (!vout[r].present || !(oregs & (1u << r))) continue;
                const Decl& d = vout[r];
                if (d.usage == Usage::Color) od[d.index] |= d.mask;
                else if (d.usage == Usage::Texcoord) ot[d.index] |= d.mask;
                else if (d.usage == Usage::Fog) ofog |= 1;
                else if (d.usage == Usage::PSize) opts |= 1;
                else opos |= d.mask;
            }
        }
        if (vs) {
            uint32_t overflow = 16;
            // Fixed locations first, then overflow slots in register order (deterministic).
            for (uint32_t r = 0; r < 16; ++r) {
                if (!vin[r].present) continue;
                VertexInputDesc d;
                d.location = vertex_location(vin[r].usage, vin[r].index);
                if (d.location == 0xFFFFFFFFu) d.location = overflow++;
                d.usage = vin[r].usage;
                d.usage_index = vin[r].index;
                d.reg = static_cast<uint8_t>(r);
                d.mask = vin[r].mask;
                d.components = static_cast<uint8_t>(((d.mask >> 0) & 1) + ((d.mask >> 1) & 1) + ((d.mask >> 2) & 1) + ((d.mask >> 3) & 1));
                f.inputs.push_back(d);
            }
            std::sort(f.inputs.begin(), f.inputs.end(), [](const VertexInputDesc& a, const VertexInputDesc& b) { return a.location < b.location; });
            for (uint32_t i = 0; i < 2; ++i)
                if (od[i]) f.varyings.push_back({kVaryingColor0 + i, VaryingSemantic::Color, static_cast<uint8_t>(i), od[i]});
            for (uint32_t i = 0; i < 8; ++i)
                if (ot[i]) f.varyings.push_back({kVaryingTexcoord0 + i, VaryingSemantic::Texcoord, static_cast<uint8_t>(i), ot[i]});
            if (ofog) f.varyings.push_back({kVaryingFog, VaryingSemantic::Fog, 0, 1});
            f.writes_point_size = opts != 0;
            f.float_constants = relative ? 256u : max_c;
        } else {
            for (uint32_t i = 0; i < 2; ++i)
                if (vread & (1u << i)) f.varyings.push_back({kVaryingColor0 + i, VaryingSemantic::Color, static_cast<uint8_t>(i), vread_mask[i]});
            for (uint32_t i = 0; i < 8; ++i)
                if (tcread & (1u << i)) f.varyings.push_back({kVaryingTexcoord0 + i, VaryingSemantic::Texcoord, static_cast<uint8_t>(i), tc_mask[i]});
            f.varyings.push_back({kVaryingFog, VaryingSemantic::Fog, 0, 1});
            for (uint32_t s = 0; s < 16; ++s) {
                if (!(samplers & (1u << s))) continue;
                SamplerDesc d;
                d.slot = static_cast<uint8_t>(s);
                d.dim = dims[s];
                for (const CtabEntry& e : p.ctab)
                    if (e.register_set == 3 && s >= e.register_index && s < static_cast<uint32_t>(e.register_index) + e.register_count) d.name = e.name;
                f.samplers.push_back(d);
            }
            f.float_constants = relative ? 32u : max_c;
            f.writes_depth = depth;
            f.can_discard = discard_;
            f.bump_env = bump;
            f.color_outputs = ps1 ? 1u : color_count();
        }
        f.relative_addressing = relative;
        f.int_bool_constants = intbool;
        f.push_constant_bytes = kTranslatedPushBytes;
        f.instanced = opt.instanced;
    }
};

}  // namespace

uint64_t TranslateOptions::key() const {
    uint64_t h = alamo::kFnv1a64Offset;
    auto mix = [&](uint8_t b) { h = alamo::fnv1a64(std::span<const uint8_t>(&b, 1), h); };
    for (TextureDim d : sampler_dims) mix(static_cast<uint8_t>(d));
    mix(static_cast<uint8_t>(projected_stages & 0xFF));
    mix(static_cast<uint8_t>(projected_stages >> 8));
    if (instanced) mix(0x51);  // Q3: only when set, so the default form's keys stay as they were
    return h;
}

uint32_t vertex_location(Usage usage, uint32_t usage_index) {
    switch (usage) {
    case Usage::Position: return usage_index == 0 ? 0u : 0xFFFFFFFFu;
    case Usage::Normal: return usage_index == 0 ? 1u : 0xFFFFFFFFu;
    case Usage::Texcoord:
        if (usage_index == 0) return 2;
        if (usage_index < 8) return 5 + usage_index;  // TEXCOORD1..7 -> 6..12
        return 0xFFFFFFFFu;
    case Usage::Tangent: return usage_index == 0 ? 3u : 0xFFFFFFFFu;
    case Usage::Binormal: return usage_index == 0 ? 4u : 0xFFFFFFFFu;
    case Usage::Color: return usage_index == 0 ? 5u : usage_index == 1 ? 13u : 0xFFFFFFFFu;
    case Usage::BlendWeight: return usage_index == 0 ? 14u : 0xFFFFFFFFu;
    case Usage::BlendIndices: return usage_index == 0 ? 15u : 0xFFFFFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

TextureDim sampler_dim(const Program& program, const TranslateOptions& options, uint32_t slot) {
    if (slot >= 16) return TextureDim::Tex2D;
    if (options.sampler_dims[slot] != TextureDim::Unknown) return options.sampler_dims[slot];
    for (const Instr& in : program.instrs)
        if (in.op == Op::Dcl && in.dst.type == reg::kSampler && in.dst.index == slot && in.tex_dim != TextureDim::Unknown)
            return in.tex_dim;
    for (const CtabEntry& e : program.ctab) {
        if (e.register_set != 3 || slot < e.register_index || slot >= static_cast<uint32_t>(e.register_index) + e.register_count) continue;
        if (e.type == 13) return TextureDim::Volume;
        if (e.type == 14) return TextureDim::Cube;
        return TextureDim::Tex2D;
    }
    return TextureDim::Tex2D;
}

bool translate(const Program& program, const TranslateOptions& options, Translation& out, std::string* err) {
    out = Translation{};
    Emitter e(program, options, err);
    if (!e.declarations() || !e.body()) return false;
    e.interface(out.iface);
    out.hlsl = e.assemble(out);
    out.entry = program.is_vs() ? "VSMain" : "PSMain";
    out.target = program.is_vs() ? "vs_6_0" : "ps_6_0";
    return true;
}

std::vector<std::string> dxc_arguments(const Translation& t) {
    return {"-spirv", "-fvk-use-dx-layout", "-fspv-target-env=vulkan1.3", "-O3", "-WX", "-HV", "2021",
            "-T", t.target, "-E", t.entry};
}

// ---------------------------------------------------------------- interface text forms

namespace {
const char* dim_name(TextureDim d) {
    return d == TextureDim::Cube ? "cube" : d == TextureDim::Volume ? "volume" : "2d";
}
const char* sem_name(VaryingSemantic s) {
    return s == VaryingSemantic::Color ? "COLOR" : s == VaryingSemantic::Fog ? "FOG" : "TEXCOORD";
}

// Sampler names are untrusted CTAB bytes: percent-encode everything outside printable ASCII plus
// space and '%', so a name is always one whitespace-free token. "-" stands for the empty name, so a
// literal "-" is encoded too.
std::string escape_name(const std::string& n) {
    if (n.empty()) return "-";
    if (n == "-") return "%2D";
    static const char kHex[] = "0123456789ABCDEF";
    std::string o;
    for (const char ch : n) {
        const auto u = static_cast<unsigned char>(ch);
        if (u <= 0x20 || u >= 0x7F || u == '%') {
            o += '%';
            o += kHex[u >> 4];
            o += kHex[u & 15u];
        } else {
            o += ch;
        }
    }
    return o;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool unescape_name(std::string_view t, std::string& out) {
    out.clear();
    if (t == "-") return true;
    if (t.empty() || t.size() > 3 * 1024) return false;
    for (size_t i = 0; i < t.size(); ++i) {
        const auto u = static_cast<unsigned char>(t[i]);
        if (u <= 0x20 || u >= 0x7F) return false;
        if (t[i] != '%') {
            out += t[i];
            continue;
        }
        if (i + 2 >= t.size()) return false;
        const int hi = hex_digit(t[i + 1]), lo = hex_digit(t[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(static_cast<unsigned char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}
}  // namespace

std::string ShaderInterface::serialize() const {
    std::string s;
    s += "profile " + profile + "\n";
    s += std::string("stage ") + (stage == ShaderType::Vertex ? "vs" : "ps") + "\n";
    s += "version " + std::to_string(major) + " " + std::to_string(minor) + "\n";
    for (const VertexInputDesc& d : inputs)
        s += "input " + std::to_string(d.location) + " " + std::to_string(static_cast<unsigned>(d.usage)) + " " +
             std::to_string(d.usage_index) + " " + std::to_string(d.reg) + " " + std::to_string(d.mask) + "\n";
    for (const VaryingDesc& d : varyings)
        s += "varying " + std::to_string(d.location) + " " + std::to_string(static_cast<unsigned>(d.semantic)) + " " +
             std::to_string(d.index) + " " + std::to_string(d.mask) + "\n";
    for (const SamplerDesc& d : samplers)
        s += "sampler " + std::to_string(d.slot) + " " + std::to_string(static_cast<unsigned>(d.dim)) + " " +
             escape_name(d.name) + "\n";
    s += "constants " + std::to_string(float_constants) + "\n";
    s += "flags " + std::to_string(relative_addressing ? 1 : 0) + " " + std::to_string(int_bool_constants ? 1 : 0) + " " +
         std::to_string(bump_env ? 1 : 0) + " " + std::to_string(writes_depth ? 1 : 0) + " " +
         std::to_string(can_discard ? 1 : 0) + " " + std::to_string(writes_point_size ? 1 : 0) + "\n";
    s += "colors " + std::to_string(color_outputs) + "\n";
    s += "push " + std::to_string(push_constant_bytes) + "\n";
    if (instanced) s += "instanced 1\n";  // Q3 instanced form only (the default form's text is unchanged)
    return s;
}

bool ShaderInterface::deserialize(std::string_view text, ShaderInterface& out) {
    out = ShaderInterface{};
    size_t pos = 0;
    bool have_profile = false;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string ln(text.substr(pos, end - pos));
        pos = end + 1;
        if (ln.empty()) continue;
        char key[16] = {};
        unsigned a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        if (std::sscanf(ln.c_str(), "%15s", key) != 1) return false;
        const std::string k = key;
        if (k == "profile") {
            if (ln.size() < 9 || ln.size() > 24) return false;
            out.profile = ln.substr(8);
            have_profile = true;
        } else if (k == "stage") {
            out.stage = ln.find(" ps") != std::string::npos ? ShaderType::Pixel : ShaderType::Vertex;
        } else if (k == "version") {
            if (std::sscanf(ln.c_str(), "version %u %u", &a, &b) != 2 || a > 3 || b > 255) return false;
            out.major = static_cast<uint8_t>(a);
            out.minor = static_cast<uint8_t>(b);
        } else if (k == "input") {
            if (std::sscanf(ln.c_str(), "input %u %u %u %u %u", &a, &b, &c, &d, &e) != 5 || b > 13 || c > 15 || d > 15 || e > 15) return false;
            VertexInputDesc v;
            v.location = a;
            v.usage = static_cast<Usage>(b);
            v.usage_index = static_cast<uint8_t>(c);
            v.reg = static_cast<uint8_t>(d);
            v.mask = static_cast<uint8_t>(e);
            v.components = static_cast<uint8_t>((e & 1) + ((e >> 1) & 1) + ((e >> 2) & 1) + ((e >> 3) & 1));
            out.inputs.push_back(v);
        } else if (k == "varying") {
            if (std::sscanf(ln.c_str(), "varying %u %u %u %u", &a, &b, &c, &d) != 4 || b > 2 || c > 15 || d > 15) return false;
            out.varyings.push_back({a, static_cast<VaryingSemantic>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d)});
        } else if (k == "sampler") {
            // "sampler <slot> <dim> <escaped name>": exactly three single-space-separated fields.
            int consumed = 0;
            if (std::sscanf(ln.c_str(), "sampler %u %u %n", &a, &b, &consumed) != 2 || consumed <= 0 || a > 15 || b > 3) return false;
            const std::string_view rest = std::string_view(ln).substr(static_cast<size_t>(consumed));
            SamplerDesc s;
            s.slot = static_cast<uint8_t>(a);
            s.dim = static_cast<TextureDim>(b);
            if (!unescape_name(rest, s.name)) return false;
            out.samplers.push_back(s);
        } else if (k == "constants") {
            if (std::sscanf(ln.c_str(), "constants %u", &a) != 1 || a > 256) return false;
            out.float_constants = a;
        } else if (k == "flags") {
            if (std::sscanf(ln.c_str(), "flags %u %u %u %u %u %u", &a, &b, &c, &d, &e, &f) != 6) return false;
            out.relative_addressing = a != 0;
            out.int_bool_constants = b != 0;
            out.bump_env = c != 0;
            out.writes_depth = d != 0;
            out.can_discard = e != 0;
            out.writes_point_size = f != 0;
        } else if (k == "colors") {
            if (std::sscanf(ln.c_str(), "colors %u", &a) != 1 || a > 4) return false;
            out.color_outputs = a;
        } else if (k == "push") {
            if (std::sscanf(ln.c_str(), "push %u", &a) != 1) return false;
            out.push_constant_bytes = a;
        } else if (k == "instanced") {
            if (std::sscanf(ln.c_str(), "instanced %u", &a) != 1 || a > 1) return false;
            out.instanced = a != 0;
        } else {
            return false;
        }
    }
    return have_profile;
}

std::string ShaderInterface::to_json() const {
    std::string s = "{\"profile\":\"" + profile + "\",\"stage\":\"" + (stage == ShaderType::Vertex ? "vs" : "ps") + "\"";
    s += ",\"inputs\":[";
    for (size_t i = 0; i < inputs.size(); ++i) {
        const VertexInputDesc& d = inputs[i];
        s += (i ? "," : "") + std::string("{\"location\":") + std::to_string(d.location) + ",\"usage\":\"" + usage_name(d.usage) +
             "\",\"index\":" + std::to_string(d.usage_index) + ",\"reg\":" + std::to_string(d.reg) +
             ",\"components\":" + std::to_string(d.components) + "}";
    }
    s += "],\"varyings\":[";
    for (size_t i = 0; i < varyings.size(); ++i) {
        const VaryingDesc& d = varyings[i];
        s += (i ? "," : "") + std::string("{\"location\":") + std::to_string(d.location) + ",\"semantic\":\"" + sem_name(d.semantic) +
             "\",\"index\":" + std::to_string(d.index) + ",\"mask\":" + std::to_string(d.mask) + "}";
    }
    s += "],\"samplers\":[";
    for (size_t i = 0; i < samplers.size(); ++i) {
        const SamplerDesc& d = samplers[i];
        std::string nm;
        for (char ch : d.name) nm += (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 32) ? '?' : ch;
        s += (i ? "," : "") + std::string("{\"slot\":") + std::to_string(d.slot) + ",\"dim\":\"" + dim_name(d.dim) + "\",\"name\":\"" +
             nm + "\"}";
    }
    s += "],\"float_constants\":" + std::to_string(float_constants);
    s += std::string(",\"relative_addressing\":") + (relative_addressing ? "true" : "false");
    s += std::string(",\"int_bool_constants\":") + (int_bool_constants ? "true" : "false");
    s += std::string(",\"bump_env\":") + (bump_env ? "true" : "false");
    s += std::string(",\"writes_depth\":") + (writes_depth ? "true" : "false");
    s += std::string(",\"can_discard\":") + (can_discard ? "true" : "false");
    s += std::string(",\"writes_point_size\":") + (writes_point_size ? "true" : "false");
    s += ",\"color_outputs\":" + std::to_string(color_outputs);
    s += ",\"push_constant_bytes\":" + std::to_string(push_constant_bytes) + "}";
    return s;
}

}  // namespace alamo::shaderxlat

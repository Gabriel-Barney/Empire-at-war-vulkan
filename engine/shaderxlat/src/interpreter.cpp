// CPU reference interpreter (see interpreter.hpp). Written against the semantics table at the top of
// translate.cpp, not against the emitter's code: every operation is re-implemented here in C++.
#include "alamo/shaderxlat/interpreter.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace alamo::shaderxlat {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

float sat(float x) { return std::fmin(std::fmax(x, 0.0f), 1.0f); }
float clampf(float x, float lo, float hi) { return std::fmin(std::fmax(x, lo), hi); }
float lmul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
F4 lmul4(const F4& a, const F4& b) { return {lmul(a[0], b[0]), lmul(a[1], b[1]), lmul(a[2], b[2]), lmul(a[3], b[3])}; }
float dp3(const F4& a, const F4& b) {
    const F4 m = lmul4(a, b);
    return m[0] + m[1] + m[2];
}
float dp4(const F4& a, const F4& b) {
    const F4 m = lmul4(a, b);
    return m[0] + m[1] + m[2] + m[3];
}
float pdot3(const F4& a, const F4& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float rcp(float x) { return x == 0.0f ? kInf : 1.0f / x; }
float rsq(float x) {
    const float a = std::fabs(x);
    return a == 0.0f ? kInf : 1.0f / std::sqrt(a);
}
float logx(float x) {
    const float a = std::fabs(x);
    return a == 0.0f ? -FLT_MAX : std::log2(a);
}
float mask_low8(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    b &= 0xFFFFFF00u;
    std::memcpy(&v, &b, 4);
    return v;
}
F4 splat(float v) { return {v, v, v, v}; }
int32_t to_addr(float v) {
    if (std::isnan(v)) return 0;
    return static_cast<int32_t>(clampf(v, -65536.0f, 65536.0f));
}

struct Pending {
    enum Kind : uint8_t { None, Value, Bools, Pad, Depth } kind = None;
    F4 value{};
    std::array<bool, 4> bools{};
    uint32_t index = 0;
    float scalar = 0.0f;
};

}  // namespace

struct Interpreter::Frame {
    const ConstantState* k = nullptr;
    const TextureSource* tex = nullptr;
    const TranslatedPush* push = nullptr;
    std::array<F4, 32> r{};
    std::array<F4, 16> v{};
    std::array<F4, 8> tc{};
    std::array<F4, 8> t{};
    std::array<float, 16> pad{};
    std::array<int32_t, 4> a0{};
    int32_t aL = 0;
    std::array<bool, 4> p0{};
    F4 oPos{}, oFog{}, oPts{};
    std::array<F4, 2> oD{};
    std::array<F4, 8> oT{};
    std::array<F4, 16> o{};          // vs_3_0 o0-o11
    F4 vPos{}, vFace{};              // ps_3_0
    std::array<F4, 4> oC{};
    F4 oDepth{};
    bool killed = false;
};

bool Interpreter::init(const Program& program, const TranslateOptions& options, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    prog_ = program;
    opt_ = options;
    cdef_.fill(false);
    idef_.fill(false);
    bdef_.fill(false);
    labels_.fill(-1);
    writes_depth_ = false;
    io_.fill(IoDecl{});
    const size_t n = program.instrs.size();
    match_.assign(n, -1);
    std::vector<int32_t> stack;
    bool in_sub = false;
    for (size_t i = 0; i < n; ++i) {
        const Instr& in = program.instrs[i];
        const bool ps = program.is_ps();
        const bool texop = (static_cast<uint16_t>(in.op) >= 64 && static_cast<uint16_t>(in.op) <= 87 &&
                            in.op != Op::ExpP && in.op != Op::LogP && in.op != Op::Cnd && in.op != Op::Def) ||
                           in.op == Op::Bem || in.op == Op::Dsx || in.op == Op::Dsy || in.op == Op::TexLdd;
        if (!ps && texop) return fail("texture op in a vertex shader");
        if (in.op == Op::Reserved0) return fail("unsupported op");
        if (in.op == Op::TexLdl && !ps) return fail("vertex texture fetch");
        if (program.major >= 3 && in.op == Op::Dcl) {
            const uint8_t t = in.dst.type;
            if (!ps && t == reg::kSampler) return fail("vertex texture fetch");
            if ((!ps && t == reg::kOutput) || (ps && t == reg::kInput)) {
                const Usage u = in.usage;
                const uint8_t k = in.usage_index;
                const bool ok = (u == Usage::Color && k < 2) || (u == Usage::Texcoord && k < 8) || (u == Usage::Fog && k == 0) ||
                                (!ps && ((u == Usage::Position || u == Usage::PSize) && k == 0));
                if (!ok || in.dst.index >= 16 || io_[in.dst.index].present) return fail("SM3 dcl usage without a varying slot");
                io_[in.dst.index] = {true, u, k};
            }
        }
        // Operand checks, so programs not produced by parse() (which enforces the same rules) cannot
        // index past a register file: ps_1_x stage indices must fit the 8-entry coordinate/texture
        // files, rep/loop counts, labels and def targets must name registers of the right file.
        const bool ps13 = program.ps1() && program.minor < 4;
        const bool is_call = in.op == Op::Call || in.op == Op::CallNz || in.op == Op::Label;
        if (program.ps1() && in.has_dst && in.dst.index >= 8) return fail("ps_1_x register index out of range");
        if (texop && !ps13 && in.op != Op::Tex && in.op != Op::TexKill && in.op != Op::TexLdd && in.op != Op::Dsx &&
            in.op != Op::Dsy && !((in.op == Op::TexCoord || in.op == Op::Bem || in.op == Op::TexDepth) && program.ps1()))
            return fail("ps_1_0..1_3 texture op outside ps_1_0..1_3");
        if (texop && ps13 && in.has_dst && in.dst.type != reg::kTexture) return fail("ps_1_0..1_3 texture op destination must be t#");
        if ((in.op == Op::Rep && (in.src[0].type != reg::kConstInt || in.src[0].index >= 16)) ||
            (in.op == Op::Loop && (in.src[1].type != reg::kConstInt || in.src[1].index >= 16)))
            return fail("rep/loop count must be an integer register");
        if (is_call && (in.src[0].type != reg::kLabel || in.src[0].index >= 2048)) return fail("call/label operand must be a label");
        if ((in.op == Op::Def && in.dst.index >= 256) || ((in.op == Op::Defi || in.op == Op::Defb) && in.dst.index >= 16))
            return fail("def register out of range");
        switch (in.op) {
        case Op::Def:
            cdef_[in.dst.index] = true;
            for (size_t c = 0; c < 4; ++c) std::memcpy(&cval_[in.dst.index][c], &in.literal[c], 4);
            break;
        case Op::Defi:
            idef_[in.dst.index] = true;
            for (size_t c = 0; c < 4; ++c) ival_[in.dst.index][c] = static_cast<int32_t>(in.literal[c]);
            break;
        case Op::Defb:
            bdef_[in.dst.index] = true;
            bval_[in.dst.index] = in.literal[0];
            break;
        case Op::If: case Op::Ifc: case Op::Rep: case Op::Loop:
            stack.push_back(static_cast<int32_t>(i));
            break;
        case Op::Else:
            if (stack.empty() || (program.instrs[static_cast<size_t>(stack.back())].op != Op::If &&
                                  program.instrs[static_cast<size_t>(stack.back())].op != Op::Ifc))
                return fail("else without if");
            if (match_[static_cast<size_t>(stack.back())] >= 0) return fail("double else");
            match_[static_cast<size_t>(stack.back())] = static_cast<int32_t>(i);
            stack.back() = static_cast<int32_t>(i);  // the else now waits for endif
            break;
        case Op::EndIf: {
            if (stack.empty()) return fail("endif without if");
            const Op o = program.instrs[static_cast<size_t>(stack.back())].op;
            if (o != Op::If && o != Op::Ifc && o != Op::Else) return fail("endif mismatch");
            const size_t s = static_cast<size_t>(stack.back());
            if (o == Op::Else) match_[s] = static_cast<int32_t>(i);
            else match_[s] = static_cast<int32_t>(i);
            match_[i] = static_cast<int32_t>(s);
            stack.pop_back();
            break;
        }
        case Op::EndRep: case Op::EndLoop: {
            const Op want = in.op == Op::EndRep ? Op::Rep : Op::Loop;
            if (stack.empty() || program.instrs[static_cast<size_t>(stack.back())].op != want) return fail("loop end mismatch");
            match_[static_cast<size_t>(stack.back())] = static_cast<int32_t>(i);
            match_[i] = stack.back();
            stack.pop_back();
            break;
        }
        case Op::Break: case Op::Breakc: case Op::Breakp: {
            bool inloop = false;
            for (int32_t s : stack) {
                const Op o = program.instrs[static_cast<size_t>(s)].op;
                inloop |= o == Op::Rep || o == Op::Loop;
            }
            if (!inloop) return fail("break outside a loop");
            break;
        }
        case Op::Label:
            if (!stack.empty()) return fail("label inside a block");
            if (labels_[in.src[0].index] >= 0) return fail("duplicate label");
            labels_[in.src[0].index] = static_cast<int32_t>(i);
            in_sub = true;
            break;
        case Op::TexDepth: case Op::TexM3x2Depth:
            writes_depth_ = true;
            break;
        default:
            if (in.has_dst && in.dst.type == reg::kDepthOut) writes_depth_ = true;
            break;
        }
    }
    (void)in_sub;
    if (!stack.empty()) return fail("unterminated block");
    for (const Instr& in : program.instrs)
        if ((in.op == Op::Call || in.op == Op::CallNz) && labels_[in.src[0].index] < 0) return fail("call to undefined label");
    for (uint32_t s = 0; s < 16; ++s) dims_[s] = sampler_dim(program, options, s);
    return true;
}

bool Interpreter::execute(Frame& f) const {
    const Program& p = prog_;
    const bool vs = p.is_vs();
    const bool ps1 = p.ps1();
    const bool ps14 = ps1 && p.minor >= 4;
    const bool sm3 = p.major >= 3;
    const float range = ps14 ? 8.0f : 1.0f;
    const auto& ins = p.instrs;
    const size_t n = ins.size();

    // ---------------------------------------------------- operand reads
    auto cread = [&](int64_t idx) -> F4 {
        const int64_t lim = vs ? 256 : 32;
        F4 v{};
        if (idx >= 0 && idx < lim) v = f.k->c[static_cast<size_t>(idx)];
        if (idx >= 0 && idx < 256 && cdef_[static_cast<size_t>(idx)]) v = cval_[static_cast<size_t>(idx)];
        if (ps1)
            for (float& x : v) x = clampf(x, -1.0f, 1.0f);
        return v;
    };
    // Bounds-checked register-file reads (init() and parse() already reject out-of-range operands;
    // these keep a hand-built Program from ever indexing past an array).
    auto ireg = [&](uint32_t i) -> std::array<int32_t, 4> {
        if (i >= 16) return {};
        return idef_[i] ? ival_[i] : f.k->i[i];
    };
    auto breg = [&](uint32_t i) -> bool { return i < 16 && (bdef_[i] ? bval_[i] != 0 : f.k->b[i] != 0); };
    auto tcv = [&](uint32_t i) -> F4 { return i < f.tc.size() ? f.tc[i] : F4{}; };
    auto base = [&](const Src& s, uint32_t off) -> F4 {
        switch (s.type) {
        case reg::kTemp: return f.r[(s.index + off) & 31u];
        case reg::kInput: return s.index < f.v.size() ? f.v[s.index] : F4{};
        case reg::kConst: {
            int64_t idx = static_cast<int64_t>(s.index) + off;
            if (s.relative) idx += s.rel_type == reg::kLoop ? f.aL : f.a0[s.rel_comp];
            return cread(idx);
        }
        case reg::kAddr:
            if (vs) return {static_cast<float>(f.a0[0]), static_cast<float>(f.a0[1]), static_cast<float>(f.a0[2]), static_cast<float>(f.a0[3])};
            if (ps1 && !ps14) return s.index < f.t.size() ? f.t[s.index] : F4{};
            return tcv(s.index);
        case reg::kConstInt: {
            const auto i = ireg(s.index);
            return {static_cast<float>(i[0]), static_cast<float>(i[1]), static_cast<float>(i[2]), static_cast<float>(i[3])};
        }
        case reg::kMiscType: return s.index == 0 ? f.vPos : f.vFace;
        default: return {};
        }
    };
    auto modify = [](const F4& b, SrcMod m) -> F4 {
        F4 r = b;
        for (float& x : r) {
            switch (m) {
            case SrcMod::Neg: x = -x; break;
            case SrcMod::Bias: x = x - 0.5f; break;
            case SrcMod::BiasNeg: x = -(x - 0.5f); break;
            case SrcMod::Sign: x = (x - 0.5f) * 2.0f; break;
            case SrcMod::SignNeg: x = -((x - 0.5f) * 2.0f); break;
            case SrcMod::Comp: x = 1.0f - x; break;
            case SrcMod::X2: x = x * 2.0f; break;
            case SrcMod::X2Neg: x = -(x * 2.0f); break;
            case SrcMod::Abs: x = std::fabs(x); break;
            case SrcMod::AbsNeg: x = -std::fabs(x); break;
            default: break;
            }
        }
        return r;
    };
    auto read = [&](const Src& s, uint32_t off = 0) -> F4 {
        const F4 b = base(s, off);
        const F4 w{b[s.swz[0]], b[s.swz[1]], b[s.swz[2]], b[s.swz[3]]};
        return modify(w, s.mod);
    };
    auto rs = [&](const Src& s) { return read(s)[3]; };
    // Texture-op source: _dz/_dw are returned unapplied.
    auto read_tex = [&](const Src& s) -> F4 {
        Src c = s;
        if (c.mod == SrcMod::Dz || c.mod == SrcMod::Dw) c.mod = SrcMod::None;
        return read(c);
    };
    auto sample = [&](uint32_t slot, const F4& coord) -> F4 {
        if (!f.tex) return {};
        return f.tex->sample(slot, dims_[slot & 15u], coord);
    };
    auto proj = [](const F4& c) -> F4 { return {c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]}; };

    // ---------------------------------------------------- one instruction -> pending write
    auto alu = [&](const Instr& in, Pending& out) -> bool {
        const Src* s = in.src.data();
        auto val = [&](const F4& v) {
            out.kind = Pending::Value;
            out.value = v;
            return true;
        };
        const uint32_t m = in.dst.index;
        F4 a{}, b{}, c{};
        switch (in.op) {
        case Op::Nop: case Op::Phase: return true;
        case Op::Mov: case Op::Mova: return val(read(s[0]));
        case Op::Add: a = read(s[0]); b = read(s[1]); return val({a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]});
        case Op::Sub: a = read(s[0]); b = read(s[1]); return val({a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]});
        case Op::Mul: return val(lmul4(read(s[0]), read(s[1])));
        case Op::Mad: {
            const F4 mm = lmul4(read(s[0]), read(s[1]));
            c = read(s[2]);
            return val({mm[0] + c[0], mm[1] + c[1], mm[2] + c[2], mm[3] + c[3]});
        }
        case Op::Dp3: return val(splat(dp3(read(s[0]), read(s[1]))));
        case Op::Dp4: return val(splat(dp4(read(s[0]), read(s[1]))));
        case Op::Min: case Op::Max: case Op::Slt: case Op::Sge: {
            a = read(s[0]);
            b = read(s[1]);
            F4 r{};
            for (size_t i = 0; i < 4; ++i) {
                if (in.op == Op::Min) r[i] = a[i] < b[i] ? a[i] : b[i];
                else if (in.op == Op::Max) r[i] = a[i] >= b[i] ? a[i] : b[i];
                else if (in.op == Op::Slt) r[i] = a[i] < b[i] ? 1.0f : 0.0f;
                else r[i] = a[i] >= b[i] ? 1.0f : 0.0f;
            }
            return val(r);
        }
        case Op::Rcp: return val(splat(rcp(rs(s[0]))));
        case Op::Rsq: return val(splat(rsq(rs(s[0]))));
        case Op::Exp: return val(splat(std::exp2(rs(s[0]))));
        case Op::Log: return val(splat(logx(rs(s[0]))));
        case Op::ExpP: {
            const float w = rs(s[0]);
            if (vs && p.major == 1) {
                const float fl = std::floor(w);
                return val({std::exp2(fl), w - fl, mask_low8(std::exp2(w)), 1.0f});
            }
            return val(splat(std::exp2(w)));
        }
        case Op::LogP: {
            const float w = rs(s[0]);
            if (vs && p.major == 1) {
                const float v = std::fabs(w);
                if (v == 0.0f) return val({-FLT_MAX, 1.0f, -FLT_MAX, 1.0f});
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                const float e = static_cast<float>(static_cast<int32_t>(bits >> 23) - 127);
                const uint32_t mb = (bits & 0x007FFFFFu) | 0x3F800000u;
                float mant;
                std::memcpy(&mant, &mb, 4);
                return val({e, mant, mask_low8(std::log2(v)), 1.0f});
            }
            return val(splat(logx(w)));
        }
        case Op::Frc: {
            a = read(s[0]);
            for (float& x : a) x = x - std::floor(x);
            return val(a);
        }
        case Op::Abs: {
            a = read(s[0]);
            for (float& x : a) x = std::fabs(x);
            return val(a);
        }
        case Op::Sgn: {
            a = read(s[0]);
            for (float& x : a) x = x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
            return val(a);
        }
        case Op::Nrm: {
            a = read(s[0]);
            const float r = rsq(dp3(a, a));
            return val(lmul4(a, splat(r)));
        }
        case Op::Lit: {
            a = read(s[0]);
            F4 r{1.0f, 0.0f, 0.0f, 1.0f};
            const float pw = clampf(a[3], -127.9961f, 127.9961f);
            if (a[0] > 0.0f) {
                r[1] = a[0];
                if (a[1] > 0.0f) r[2] = std::exp2(pw * std::log2(a[1]));
            }
            return val(r);
        }
        case Op::Dst: a = read(s[0]); b = read(s[1]); return val({1.0f, lmul(a[1], b[1]), a[2], b[3]});
        case Op::Crs:
            a = read(s[0]);
            b = read(s[1]);
            return val({lmul(a[1], b[2]) - lmul(a[2], b[1]), lmul(a[2], b[0]) - lmul(a[0], b[2]), lmul(a[0], b[1]) - lmul(a[1], b[0]), 0.0f});
        case Op::Lrp: {
            a = read(s[0]);
            b = read(s[1]);
            c = read(s[2]);
            const F4 d = lmul4(a, {b[0] - c[0], b[1] - c[1], b[2] - c[2], b[3] - c[3]});
            return val({d[0] + c[0], d[1] + c[1], d[2] + c[2], d[3] + c[3]});
        }
        case Op::Pow: return val(splat(std::exp2(lmul(rs(s[1]), std::log2(std::fabs(rs(s[0])))))));
        case Op::SinCos: {
            const float x = rs(s[0]);
            return val({std::cos(x), std::sin(x), 0.0f, 0.0f});
        }
        case Op::Dp2Add: a = read(s[0]); b = read(s[1]); return val(splat(lmul(a[0], b[0]) + lmul(a[1], b[1]) + rs(s[2])));
        case Op::Cmp: case Op::Cnd: {
            a = read(s[0]);
            b = read(s[1]);
            c = read(s[2]);
            F4 r{};
            for (size_t i = 0; i < 4; ++i) r[i] = (in.op == Op::Cmp ? a[i] >= 0.0f : a[i] > 0.5f) ? b[i] : c[i];
            return val(r);
        }
        case Op::M4x4: case Op::M4x3: case Op::M3x4: case Op::M3x3: case Op::M3x2: {
            const uint32_t rows = in.op == Op::M4x4 || in.op == Op::M3x4 ? 4u : in.op == Op::M3x2 ? 2u : 3u;
            const bool d4 = in.op == Op::M4x4 || in.op == Op::M4x3;
            a = read(s[0]);
            F4 r{};
            for (uint32_t k = 0; k < rows; ++k) {
                const F4 row = read(s[1], k);
                r[k] = d4 ? dp4(a, row) : dp3(a, row);
            }
            return val(r);
        }
        case Op::Dsx: case Op::Dsy: return val({});  // single-pixel evaluation: derivatives are 0
        case Op::Setp: {
            a = read(s[0]);
            b = read(s[1]);
            out.kind = Pending::Bools;
            for (size_t i = 0; i < 4; ++i) {
                switch (in.specific & 7u) {
                case 1: out.bools[i] = a[i] > b[i]; break;
                case 2: out.bools[i] = a[i] == b[i]; break;
                case 3: out.bools[i] = a[i] >= b[i]; break;
                case 4: out.bools[i] = a[i] < b[i]; break;
                case 5: out.bools[i] = a[i] != b[i]; break;
                case 6: out.bools[i] = a[i] <= b[i]; break;
                default: return false;
                }
            }
            return true;
        }
        case Op::Bem: {
            a = read(s[0]);
            b = read(s[1]);
            const F4 bm = f.k->c[(16 + m) & 31u];
            return val({a[0] + bm[0] * b[0] + bm[2] * b[1], a[1] + bm[1] * b[0] + bm[3] * b[1], 0.0f, 0.0f});
        }
        case Op::TexCoord: {
            if (!ps14) {
                const F4 t = tcv(m);
                return val({sat(t[0]), sat(t[1]), sat(t[2]), 1.0f});
            }
            a = read_tex(s[0]);
            if (s[0].mod == SrcMod::Dz) return val({a[0] / a[2], a[1] / a[2], 1.0f, 1.0f});
            if (s[0].mod == SrcMod::Dw) return val({a[0] / a[3], a[1] / a[3], 1.0f, 1.0f});
            return val({a[0], a[1], a[2], 1.0f});
        }
        case Op::Tex: {
            if (p.major >= 2) {
                const uint32_t slot = s[1].index;
                a = read_tex(s[0]);
                if (in.specific & kTexldProject) return val(sample(slot, proj(a)));
                return val(sample(slot, a));  // texldb: single-level textures, the bias has no effect
            }
            if (!ps14) {
                const F4 t = tcv(m);
                return val(sample(m, (opt_.projected_stages & (1u << m)) ? proj(t) : t));
            }
            a = read_tex(s[0]);
            if (s[0].mod == SrcMod::Dz) return val(sample(m, {a[0] / a[2], a[1] / a[2], a[2], a[3]}));
            if (s[0].mod == SrcMod::Dw) return val(sample(m, proj(a)));
            return val(sample(m, a));
        }
        case Op::TexLdd: return val(sample(s[1].index, read_tex(s[0])));
        case Op::TexLdl: return val(sample(s[1].index, read_tex(s[0])));  // single-level textures: the LOD has no effect
        case Op::TexKill: {
            F4 v{};
            int comps = 3;
            uint8_t mask = 7;
            if (ps1 && !ps14) {
                v = tcv(in.dst.index);
            } else {
                Src r;
                r.type = in.dst.type;
                r.index = in.dst.index;
                v = base(r, 0);
                if (!ps1) {
                    mask = in.dst.mask;
                    comps = 4;
                }
            }
            for (int i = 0; i < comps; ++i)
                if ((mask & (1u << i)) && v[static_cast<size_t>(i)] < 0.0f) f.killed = true;
            return true;
        }
        case Op::TexBem: case Op::TexBemL: {
            a = read_tex(s[0]);
            const F4 bm = f.k->c[(16 + m) & 31u];
            const F4 t = tcv(m);
            const F4 coord{t[0] + bm[0] * a[0] + bm[2] * a[1], t[1] + bm[1] * a[0] + bm[3] * a[1], t[2], t[3]};
            F4 r = sample(m, coord);
            if (in.op == Op::TexBemL) {
                const F4 l = f.k->c[(24 + m) & 31u];
                const float lum = sat(l[0] * a[2] + l[1]);
                r = {r[0] * lum, r[1] * lum, r[2] * lum, r[3]};
            }
            return val(r);
        }
        case Op::TexReg2Ar: a = read_tex(s[0]); return val(sample(m, {a[3], a[0], a[2], a[3]}));
        case Op::TexReg2Gb: a = read_tex(s[0]); return val(sample(m, {a[1], a[2], a[2], a[3]}));
        case Op::TexReg2Rgb: a = read_tex(s[0]); return val(sample(m, a));
        case Op::TexM3x2Pad: case Op::TexM3x3Pad:
            out.kind = Pending::Pad;
            out.index = m;
            out.scalar = pdot3(tcv(m), read_tex(s[0]));
            return true;
        case Op::TexM3x2Tex: {
            const float u = f.pad[(m - 1) & 15u], v = pdot3(tcv(m), read_tex(s[0]));
            return val(sample(m, {u, v, 0.0f, 1.0f}));
        }
        case Op::TexM3x2Depth: {
            const float w = pdot3(tcv(m), read_tex(s[0]));
            out.kind = Pending::Depth;
            out.scalar = w == 0.0f ? 1.0f : f.pad[(m - 1) & 15u] / w;
            return true;
        }
        case Op::TexM3x3: case Op::TexM3x3Tex: case Op::TexM3x3Spec: case Op::TexM3x3VSpec: {
            const F4 nv{f.pad[(m - 2) & 15u], f.pad[(m - 1) & 15u], pdot3(tcv(m), read_tex(s[0])), 1.0f};
            if (in.op == Op::TexM3x3) return val(nv);
            if (in.op == Op::TexM3x3Tex) return val(sample(m, nv));
            F4 e{};
            if (in.op == Op::TexM3x3Spec) {
                e = read(s[1]);
            } else {
                e = {tcv(m - 2)[3], tcv(m - 1)[3], tcv(m)[3], 0.0f};
            }
            const float nn = pdot3(nv, nv);
            F4 r{};
            if (nn != 0.0f) {
                const float q = pdot3(nv, e) / nn;
                for (size_t i = 0; i < 3; ++i) r[i] = 2.0f * nv[i] * q - e[i];
            } else {
                for (size_t i = 0; i < 3; ++i) r[i] = -e[i];
            }
            r[3] = 1.0f;
            return val(sample(m, r));
        }
        case Op::TexDp3Tex: case Op::TexDp3: {
            const float d = pdot3(tcv(m), read_tex(s[0]));
            if (in.op == Op::TexDp3) return val(splat(d));
            return val(sample(m, {d, 0.0f, 0.0f, 1.0f}));
        }
        case Op::TexDepth:
            out.kind = Pending::Depth;
            out.scalar = f.r[5][1] == 0.0f ? 1.0f : f.r[5][0] / f.r[5][1];
            return true;
        default:
            return false;
        }
    };

    auto commit = [&](const Instr& in, const Pending& pd) {
        if (pd.kind == Pending::None) return;
        if (pd.kind == Pending::Pad) {
            f.pad[pd.index & 15u] = pd.scalar;
            return;
        }
        if (pd.kind == Pending::Depth) {
            f.oDepth[0] = pd.scalar;
            return;
        }
        const Dst& d = in.dst;
        std::array<bool, 4> en{};
        for (size_t i = 0; i < 4; ++i) {
            en[i] = (d.mask >> i) & 1u;
            if (in.predicated) {
                bool pv = f.p0[in.pred.swz[i]];
                if (in.pred.mod == SrcMod::Not) pv = !pv;
                en[i] = en[i] && pv;
            }
        }
        if (pd.kind == Pending::Bools) {
            for (size_t i = 0; i < 4; ++i)
                if (en[i]) f.p0[i] = pd.bools[i];
            return;
        }
        F4 v = pd.value;
        static const float kScale[] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        for (float& x : v) {
            if (d.shift) x = x * kScale[d.shift + 3];
            if (d.saturate) x = sat(x);
            if (ps1 && (d.type == reg::kTemp || d.type == reg::kTexture)) x = clampf(x, -range, range);
        }
        if (vs && d.type == reg::kAddr) {
            for (size_t i = 0; i < 4; ++i)
                if (en[i]) f.a0[i] = to_addr(p.major >= 2 ? std::floor(v[i] + 0.5f) : std::floor(v[i]));
            return;
        }
        F4* t = nullptr;
        switch (d.type) {
        case reg::kTemp: t = &f.r[d.index & 31u]; break;
        case reg::kAddr: t = &f.t[d.index & 7u]; break;
        case reg::kRastOut: t = d.index == 0 ? &f.oPos : d.index == 1 ? &f.oFog : &f.oPts; break;
        case reg::kAttrOut: t = &f.oD[d.index & 1u]; break;
        case reg::kTexCrdOut: t = sm3 ? &f.o[d.index & 15u] : &f.oT[d.index & 7u]; break;
        case reg::kColorOut: t = &f.oC[d.index & 3u]; break;
        case reg::kDepthOut: t = &f.oDepth; break;
        default: return;
        }
        for (size_t i = 0; i < 4; ++i)
            if (en[i]) (*t)[i] = v[i];
    };

    auto cmp = [](uint8_t c, float a, float b) {
        switch (c & 7u) {
        case 1: return a > b;
        case 2: return a == b;
        case 3: return a >= b;
        case 4: return a < b;
        case 5: return a != b;
        case 6: return a <= b;
        default: return false;
        }
    };
    auto boolsrc = [&](const Src& s) {
        bool v = s.type == reg::kConstBool ? breg(s.index) : f.p0[s.swz[0]];
        return s.mod == SrcMod::Not ? !v : v;
    };

    struct LoopState {
        size_t start, end;
        int32_t count, iter;
        bool loop;
        int32_t saved_al, step;
    };
    std::vector<LoopState> loops;
    struct CallState {
        size_t ret;
        size_t loop_depth;
    };
    std::vector<CallState> calls;
    uint32_t budget = 1u << 20;
    size_t pc = 0;
    while (pc < n) {
        if (budget-- == 0) return false;
        if (f.killed) return true;
        const Instr& in = ins[pc];
        switch (in.op) {
        case Op::Dcl: case Op::Def: case Op::Defi: case Op::Defb:
            ++pc;
            continue;
        case Op::If: case Op::Ifc: {
            const bool c = in.op == Op::If ? boolsrc(in.src[0]) : cmp(in.specific, rs(in.src[0]), rs(in.src[1]));
            pc = c ? pc + 1 : static_cast<size_t>(match_[pc]) + 1;
            continue;
        }
        case Op::Else:
            pc = static_cast<size_t>(match_[pc]) + 1;
            continue;
        case Op::EndIf:
            ++pc;
            continue;
        case Op::Rep: case Op::Loop: {
            const bool lp = in.op == Op::Loop;
            const auto iv = ireg(in.src[lp ? 1 : 0].index);
            LoopState ls{pc + 1, static_cast<size_t>(match_[pc]), std::min(std::max(iv[0], 0), 255), 0, lp, f.aL, iv[2]};
            if (lp) f.aL = iv[1];
            if (ls.count == 0) {
                if (lp) f.aL = ls.saved_al;
                pc = ls.end + 1;
            } else {
                loops.push_back(ls);
                ++pc;
            }
            continue;
        }
        case Op::EndRep: case Op::EndLoop: {
            if (loops.empty()) return false;
            LoopState& ls = loops.back();
            ++ls.iter;
            if (ls.loop) f.aL += ls.step;
            if (ls.iter < ls.count) {
                pc = ls.start;
            } else {
                if (ls.loop) f.aL = ls.saved_al;
                loops.pop_back();
                ++pc;
            }
            continue;
        }
        case Op::Break: case Op::Breakc: case Op::Breakp: {
            bool take = true;
            if (in.op == Op::Breakc) take = cmp(in.specific, rs(in.src[0]), rs(in.src[1]));
            else if (in.op == Op::Breakp) take = boolsrc(in.src[0]);
            if (!take) {
                ++pc;
                continue;
            }
            if (loops.empty()) return false;
            const LoopState ls = loops.back();
            loops.pop_back();
            if (ls.loop) f.aL = ls.saved_al;
            pc = ls.end + 1;
            continue;
        }
        case Op::Call: case Op::CallNz: {
            const bool take = in.op == Op::Call || boolsrc(in.src[1]);
            if (!take) {
                ++pc;
                continue;
            }
            if (calls.size() >= 32) return false;
            calls.push_back({pc + 1, loops.size()});
            pc = static_cast<size_t>(labels_[in.src[0].index]) + 1;
            continue;
        }
        case Op::Ret:
            if (calls.empty()) return true;
            loops.resize(calls.back().loop_depth);
            pc = calls.back().ret;
            calls.pop_back();
            continue;
        case Op::Label:
            if (calls.empty()) return true;  // falling into a subroutine ends main
            ++pc;
            continue;
        default:
            break;
        }
        const bool pair = pc + 1 < n && ins[pc + 1].coissue;
        Pending a, b;
        if (!alu(in, a)) return false;
        if (pair && !alu(ins[pc + 1], b)) return false;
        if (f.killed) return true;
        commit(in, a);
        if (pair) commit(ins[pc + 1], b);
        pc += pair ? 2 : 1;
    }
    return true;
}

bool Interpreter::run_vs(const std::array<F4, 16>& inputs, const ConstantState& constants, const TranslatedPush& push,
                         VsResult& out) const {
    if (!prog_.is_vs()) return false;
    Frame f;
    f.k = &constants;
    f.push = &push;
    f.v = inputs;
    f.oFog = {1.0f, 0.0f, 0.0f, 0.0f};
    f.oPts = {1.0f, 0.0f, 0.0f, 0.0f};
    f.oD[0] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (F4& t : f.oT) t = {0.0f, 0.0f, 0.0f, 1.0f};
    const bool sm3 = prog_.major >= 3;
    // vs_3_0: each declared o# starts at its varying's default and is copied to it afterwards.
    auto target = [&](const IoDecl& d) -> F4* {
        switch (d.usage) {
        case Usage::Position: return &f.oPos;
        case Usage::Color: return &f.oD[d.index & 1u];
        case Usage::Texcoord: return &f.oT[d.index & 7u];
        case Usage::Fog: return &f.oFog;
        default: return &f.oPts;
        }
    };
    if (sm3)
        for (size_t r = 0; r < io_.size(); ++r)
            if (io_[r].present) f.o[r] = *target(io_[r]);
    if (!execute(f)) return false;
    if (sm3)
        for (size_t r = 0; r < io_.size(); ++r)
            if (io_[r].present) *target(io_[r]) = f.o[r];
    out.pos = f.oPos;
    out.pos[0] = f.oPos[0] + push.pixel_offset[0] * f.oPos[3];
    out.pos[1] = f.oPos[1] + push.pixel_offset[1] * f.oPos[3];
    for (size_t i = 0; i < 4; ++i) {
        out.color0[i] = sm3 ? f.oD[0][i] : sat(f.oD[0][i]);  // vs_3_0 outputs are not clamped
        out.color1[i] = sm3 ? f.oD[1][i] : sat(f.oD[1][i]);
    }
    out.texcoord = f.oT;
    out.fog = sm3 ? f.oFog[0] : sat(f.oFog[0]);
    out.point_size = f.oPts[0];
    return true;
}

bool Interpreter::run_ps(const PsInputs& inputs, const ConstantState& constants, const TranslatedPush& push,
                         const TextureSource* textures, PsResult& out) const {
    if (!prog_.is_ps()) return false;
    Frame f;
    f.k = &constants;
    f.tex = textures;
    f.push = &push;
    const bool sm3 = prog_.major >= 3;
    if (sm3) {
        // ps_3_0: v# by dcl usage, not saturated; vPos = pixel centre - 0.5 (D3D9 integer centres).
        for (size_t r = 0; r < io_.size() && r < f.v.size(); ++r) {
            const IoDecl& d = io_[r];
            if (!d.present) continue;
            if (d.usage == Usage::Color) f.v[r] = inputs.color[d.index & 1u];
            else if (d.usage == Usage::Texcoord) f.v[r] = inputs.texcoord[d.index & 7u];
            else f.v[r] = {inputs.fog, 0.0f, 0.0f, 0.0f};
        }
        f.vPos = {inputs.frag_pos[0] - 0.5f, inputs.frag_pos[1] - 0.5f, 0.0f, 0.0f};
        f.vFace = splat(inputs.front_face ? 1.0f : -1.0f);
    } else {
        for (size_t i = 0; i < 2; ++i)
            for (size_t c = 0; c < 4; ++c) f.v[i][c] = sat(inputs.color[i][c]);
    }
    f.tc = inputs.texcoord;
    out = PsResult{};
    if (!execute(f)) return false;
    if (f.killed) {
        out.killed = true;
        return true;
    }
    F4 c = prog_.ps1() ? f.r[0] : f.oC[0];
    // Epilogue: alpha test, then fog.
    if (push.alpha_func < 7u) {
        const uint32_t a = static_cast<uint32_t>(std::floor(sat(c[3]) * 255.0f + 0.5f));
        const uint32_t r = push.alpha_ref;
        bool pass = false;
        switch (push.alpha_func) {
        case 1: pass = a < r; break;
        case 2: pass = a == r; break;
        case 3: pass = a <= r; break;
        case 4: pass = a > r; break;
        case 5: pass = a != r; break;
        case 6: pass = a >= r; break;
        default: pass = false; break;
        }
        if (!pass) {
            out.killed = true;
            return true;
        }
    }
    if ((push.flags & kPushFog) && !sm3) {  // ps_3_0: no fixed-function fog
        const uint32_t mode = (push.flags >> kFogTableShift) & 3u;
        float fg = sat(inputs.fog);
        if (mode != 0) {
            const float d = inputs.frag_pos[2];
            const float* fp = push.fog_params;
            if (mode == 1) fg = std::exp(-(d * fp[2]));
            else if (mode == 2) fg = std::exp(-((d * fp[2]) * (d * fp[2])));
            else fg = (fp[1] - d) / (fp[1] - fp[0]);
            fg = sat(fg);
        }
        for (size_t i = 0; i < 3; ++i) c[i] = push.fog_color[i] + fg * (c[i] - push.fog_color[i]);
    }
    out.color[0] = c;
    for (size_t i = 1; i < 4; ++i) out.color[i] = f.oC[i];
    if (writes_depth_) {
        out.depth_written = true;
        out.depth = sat(f.oDepth[0]);
    }
    return true;
}

}  // namespace alamo::shaderxlat

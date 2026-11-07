// d3dbc parser + disassembler (see bytecode.hpp for the token format).
#include "alamo/shaderxlat/bytecode.hpp"

#include <cstdio>
#include <cstring>

#include "alamo/core/ascii.hpp"

namespace alamo::shaderxlat {

namespace {

struct Reader {
    const uint8_t* data = nullptr;
    size_t words = 0;
    uint32_t at(size_t i) const {
        uint32_t v = 0;
        std::memcpy(&v, data + i * 4, 4);
        return v;
    }
};

bool fail(std::string* err, const char* fmt, size_t token, unsigned a = 0, unsigned b = 0) {
    if (err) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), fmt, a, b);
        *err = std::string(buf) + " (token " + std::to_string(token) + ")";
    }
    return false;
}

uint8_t reg_type_of(uint32_t tok) { return static_cast<uint8_t>(((tok >> 28) & 7u) | ((tok >> 8) & 0x18u)); }

// Operand counts {dst, src}; dst/src < 0: unknown opcode.
struct Counts {
    int dst = -1, src = -1;
};
Counts operand_counts(Op op, const Program& p) {
    const bool ps14 = p.is_ps() && p.major == 1 && p.minor >= 4;
    switch (op) {
    case Op::Nop: case Op::Ret: case Op::EndLoop: case Op::EndRep: case Op::Else: case Op::EndIf: case Op::Break:
    case Op::Phase:
        return {0, 0};
    case Op::Mov: case Op::Rcp: case Op::Rsq: case Op::Exp: case Op::Log: case Op::Lit: case Op::Frc: case Op::Abs:
    case Op::Nrm: case Op::Mova: case Op::ExpP: case Op::LogP: case Op::Dsx: case Op::Dsy:
    case Op::TexBem: case Op::TexBemL: case Op::TexReg2Ar: case Op::TexReg2Gb: case Op::TexReg2Rgb:
    case Op::TexM3x2Pad: case Op::TexM3x2Tex: case Op::TexM3x3Pad: case Op::TexM3x3Tex: case Op::TexM3x3VSpec:
    case Op::TexDp3Tex: case Op::TexM3x2Depth: case Op::TexDp3: case Op::TexM3x3:
        return {1, 1};
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Dp3: case Op::Dp4: case Op::Min: case Op::Max: case Op::Slt:
    case Op::Sge: case Op::Dst: case Op::M4x4: case Op::M4x3: case Op::M3x4: case Op::M3x3: case Op::M3x2:
    case Op::Pow: case Op::Crs: case Op::TexM3x3Spec: case Op::Bem: case Op::Setp: case Op::TexLdl:
        return {1, 2};
    case Op::Mad: case Op::Lrp: case Op::Sgn: case Op::Cnd: case Op::Cmp: case Op::Dp2Add:
        return {1, 3};
    case Op::SinCos:
        return {1, p.major >= 3 ? 1 : 3};
    case Op::TexLdd:
        return {1, 4};
    case Op::Call: case Op::Label: case Op::Rep: case Op::If: case Op::Breakp:
        return {0, 1};
    case Op::CallNz: case Op::Loop: case Op::Ifc: case Op::Breakc:
        return {0, 2};
    case Op::TexCoord:
        return {1, ps14 ? 1 : 0};
    case Op::TexKill: case Op::TexDepth:
        return {1, 0};
    case Op::Tex:
        if (p.major >= 2) return {1, 2};
        return {1, ps14 ? 1 : 0};
    default:
        return {};
    }
}

// Register-file limits per shader model. Everything the emitter/interpreter indexes is checked here.
bool check_register(const Program& p, uint8_t type, uint32_t index, bool dst, std::string* err, size_t tok) {
    const bool vs = p.is_vs();
    const bool sm3 = p.major >= 3;
    uint32_t limit = 0;
    bool readable = true, writable = false;
    const bool ps1 = p.is_ps() && p.major == 1;
    switch (type) {
    // ps_1_0..1_3: r0-r1, ps_1_4: r0-r5; everything else gets the SM2/3 maximum of 32.
    case reg::kTemp: limit = ps1 ? (p.minor >= 4 ? 6u : 2u) : 32u; writable = true; break;
    case reg::kInput: limit = vs ? 16u : (sm3 ? 10u : 2u); break;
    // ps_1_x: c0-c7 (the bump-env constants c16+ are engine-supplied, never addressed by the bytecode);
    // ps_2_x: c0-c31; ps_3_0: c0-c223 (the translator binds c0-c31 and def literals, translate.cpp).
    case reg::kConst: limit = vs ? 256u : (ps1 ? 8u : (sm3 ? 224u : 32u)); break;
    case reg::kAddr:  // a0 (VS) / t# (PS; ps_3_0 has no t#: texture coordinates are v# with a dcl usage)
        if (vs) {
            limit = 1;
            writable = true;
        } else if (sm3) {
            return fail(err, "register type %u not valid here", tok, type);
        } else {
            limit = p.major >= 2 ? 8u : (p.minor >= 4 ? 6u : 4u);
            writable = p.major == 1 && p.minor < 4;  // ps_1_1..1_3 texture registers are writable
        }
        break;
    case reg::kRastOut: if (!vs || sm3) return fail(err, "register type %u not valid here", tok, type); limit = 3; readable = false; writable = true; break;
    case reg::kAttrOut: if (!vs || sm3) return fail(err, "register type %u not valid here", tok, type); limit = 2; readable = false; writable = true; break;
    case reg::kTexCrdOut: if (!vs) return fail(err, "register type %u not valid here", tok, type); limit = sm3 ? 12u : 8u; readable = false; writable = true; break;
    case reg::kConstInt: limit = 16; break;
    case reg::kColorOut: if (vs) return fail(err, "register type %u not valid here", tok, type); limit = 4; readable = false; writable = true; break;
    case reg::kDepthOut: if (vs) return fail(err, "register type %u not valid here", tok, type); limit = 1; readable = false; writable = true; break;
    case reg::kSampler: limit = vs ? 4u : 16u; break;
    case reg::kConstBool: limit = 16; break;
    case reg::kLoop: limit = 1; break;
    case reg::kMiscType: if (vs || !sm3) return fail(err, "register type %u not valid here", tok, type); limit = 2; break;  // vPos, vFace
    case reg::kLabel: limit = 2048; break;
    case reg::kPredicate: limit = 1; writable = true; break;
    default:
        return fail(err, "unsupported register type %u", tok, type);
    }
    if (index >= limit) return fail(err, "register index %u out of range for type %u", tok, index, type);
    if (dst && !writable) return fail(err, "register type %u is not writable", tok, type);
    if (!dst && !readable) return fail(err, "register type %u is not readable", tok, type);
    return true;
}

// ps_1_0..1_3 texture-stage ops: the destination t# names the stage (register, sampler, coordinate set).
bool is_ps13_tex_op(Op op) {
    switch (op) {
    case Op::TexBem: case Op::TexBemL: case Op::TexReg2Ar: case Op::TexReg2Gb: case Op::TexReg2Rgb:
    case Op::TexM3x2Pad: case Op::TexM3x2Tex: case Op::TexM3x2Depth: case Op::TexM3x3Pad: case Op::TexM3x3Tex:
    case Op::TexM3x3Spec: case Op::TexM3x3VSpec: case Op::TexM3x3: case Op::TexDp3Tex: case Op::TexDp3:
        return true;
    default:
        return false;
    }
}

// Operand register kinds per instruction and shader model (D3D9 asm reference). Instructions whose
// destination/source index selects a texture stage, coordinate set or integer/label register must use
// the right register file, or the emitter/interpreter would index the wrong (or a too small) array.
bool check_operand_kinds(const Program& p, const Instr& in, std::string* err) {
    const size_t tok = in.token;
    const bool ps = p.is_ps();
    const bool ps13 = ps && p.major == 1 && p.minor < 4;
    const bool ps14 = ps && p.major == 1 && p.minor >= 4;
    const uint8_t dt = in.dst.type;
    const Src* s = in.src.data();
    // ps_1_0..1_3-only addressing ops.
    if (is_ps13_tex_op(in.op)) {
        if (!ps13) return fail(err, "opcode %u is only valid in ps_1_0..ps_1_3", tok, static_cast<unsigned>(in.op));
        if (dt != reg::kTexture) return fail(err, "texture op destination must be t# (type %u)", tok, dt);
        if (s[0].type != reg::kTexture || s[0].index >= in.dst.index)
            return fail(err, "texture op source must be a lower t# (t%u -> t%u)", tok, s[0].index, in.dst.index);
        if (in.op == Op::TexM3x3Spec && s[1].type != reg::kConst) return fail(err, "texm3x3spec eye vector must be c#", tok);
        return true;
    }
    switch (in.op) {
    case Op::TexCoord:  // texcoord t# (1.0-1.3) / texcrd r#, t# (1.4)
        if (ps13 && dt == reg::kTexture) return true;
        if (ps14 && dt == reg::kTemp && s[0].type == reg::kTexture) return true;
        return fail(err, "texcoord/texcrd operands (destination type %u)", tok, dt);
    case Op::Tex:
        if (!ps) return fail(err, "texture op in a vertex shader", tok);
        if (ps13) return dt == reg::kTexture ? true : fail(err, "tex destination must be t# (type %u)", tok, dt);
        if (dt != reg::kTemp) return fail(err, "texld destination must be r# (type %u)", tok, dt);
        if (ps14 && s[0].type != reg::kTexture && s[0].type != reg::kTemp) return fail(err, "texld source type %u", tok, s[0].type);
        if (p.major >= 2 && s[1].type != reg::kSampler) return fail(err, "texld second source must be s#", tok);
        return true;
    case Op::TexLdd: case Op::TexLdl:
        if (dt != reg::kTemp || s[1].type != reg::kSampler) return fail(err, "texldd/texldl operands", tok);
        if (in.op == Op::TexLdd && !(ps && p.major >= 2)) return fail(err, "texldd outside ps_2_x+", tok);
        if (in.op == Op::TexLdl && p.major < 3) return fail(err, "texldl before SM3", tok);
        return true;
    case Op::TexKill:
        if (!ps) return fail(err, "texkill in a vertex shader", tok);
        if (ps13) return dt == reg::kTexture ? true : fail(err, "texkill operand must be t# (type %u)", tok, dt);
        return (dt == reg::kTemp || dt == reg::kTexture) ? true : fail(err, "texkill operand type %u", tok, dt);
    case Op::TexDepth:
        if (!ps14 || dt != reg::kTemp || in.dst.index != 5) return fail(err, "texdepth is ps_1_4 r5 only", tok);
        return true;
    case Op::Bem:
        if (!ps14 || dt != reg::kTemp) return fail(err, "bem is ps_1_4 with an r# destination", tok);
        return true;
    case Op::Dsx: case Op::Dsy:
        if (!(ps && p.major >= 2)) return fail(err, "dsx/dsy outside ps_2_x+", tok);
        return true;
    case Op::Reserved0:
        return fail(err, "reserved opcode", tok);
    case Op::Rep:
        if (s[0].type != reg::kConstInt) return fail(err, "rep operand must be i# (type %u)", tok, s[0].type);
        return true;
    case Op::Loop:
        if (s[0].type != reg::kLoop || s[1].type != reg::kConstInt) return fail(err, "loop operands must be aL, i#", tok);
        return true;
    case Op::Call: case Op::Label:
        if (s[0].type != reg::kLabel) return fail(err, "call/label operand must be l# (type %u)", tok, s[0].type);
        return true;
    case Op::CallNz:
        if (s[0].type != reg::kLabel || (s[1].type != reg::kConstBool && s[1].type != reg::kPredicate))
            return fail(err, "callnz operands must be l#, b#/p0", tok);
        return true;
    case Op::If:
        if (s[0].type != reg::kConstBool && s[0].type != reg::kPredicate) return fail(err, "if operand must be b#/p0", tok);
        return true;
    case Op::Breakp:
        if (s[0].type != reg::kPredicate) return fail(err, "breakp operand must be p0", tok);
        return true;
    default:
        return true;
    }
}

struct Parser {
    Reader r;
    Program& p;
    std::string* err;
    size_t pos = 0;

    bool need(size_t n) const { return pos + n <= r.words; }

    bool read_dst(Dst& d) {
        if (!need(1)) return fail(err, "truncated destination operand", pos);
        const uint32_t t = r.at(pos);
        if (!(t & 0x80000000u)) return fail(err, "destination operand token without bit 31", pos);
        d.type = reg_type_of(t);
        d.index = static_cast<uint16_t>(t & 0x7FFu);
        d.mask = static_cast<uint8_t>((t >> 16) & 0xFu);
        const uint32_t mods = (t >> 20) & 0xFu;
        if (mods & ~7u) return fail(err, "unknown result modifier 0x%x", pos, mods);
        d.saturate = (mods & 1u) != 0;
        d.partial_precision = (mods & 2u) != 0;
        d.centroid = (mods & 4u) != 0;
        const int shift = static_cast<int>((t >> 24) & 0xFu);
        d.shift = static_cast<int8_t>(shift >= 8 ? shift - 16 : shift);
        if (d.shift > 3 || d.shift < -3) return fail(err, "unsupported shift scale %u", pos, static_cast<unsigned>(shift));
        if (t & (1u << 13)) return fail(err, "relative addressing on a destination is not supported", pos);
        ++pos;
        return true;
    }

    bool read_src(Src& s) {
        if (!need(1)) return fail(err, "truncated source operand", pos);
        const uint32_t t = r.at(pos);
        if (!(t & 0x80000000u)) return fail(err, "source operand token without bit 31", pos);
        s.type = reg_type_of(t);
        s.index = static_cast<uint16_t>(t & 0x7FFu);
        const uint32_t sw = (t >> 16) & 0xFFu;
        for (int i = 0; i < 4; ++i) s.swz[static_cast<size_t>(i)] = static_cast<uint8_t>((sw >> (2 * i)) & 3u);
        const uint32_t mod = (t >> 24) & 0xFu;
        if (mod > static_cast<uint32_t>(SrcMod::Not)) return fail(err, "unknown source modifier %u", pos, mod);
        s.mod = static_cast<SrcMod>(mod);
        const size_t tok = pos;
        ++pos;
        if (t & (1u << 13)) {
            s.relative = true;
            if (p.is_ps() && p.major < 3) return fail(err, "relative addressing in a pixel shader before SM3", tok);
            if (p.major >= 2) {
                if (!need(1)) return fail(err, "truncated relative address token", pos);
                const uint32_t a = r.at(pos);
                if (!(a & 0x80000000u)) return fail(err, "relative address token without bit 31", pos);
                s.rel_type = reg_type_of(a);
                s.rel_comp = static_cast<uint8_t>((a >> 16) & 3u);
                if (s.rel_type != reg::kAddr && s.rel_type != reg::kLoop)
                    return fail(err, "relative address register type %u", pos, s.rel_type);
                if ((a & 0x7FFu) != 0) return fail(err, "relative address register index %u", pos, a & 0x7FFu);
                if (p.is_ps() && s.rel_type == reg::kAddr) return fail(err, "a0 relative addressing in a pixel shader", pos);
                ++pos;
            } else {
                if (!p.is_vs()) return fail(err, "relative addressing in a ps_1_x shader", tok);
                s.rel_type = reg::kAddr;
                s.rel_comp = 0;
            }
            // SM1-2: only constant float registers can be indexed.
            if (s.type != reg::kConst) return fail(err, "relative addressing on register type %u", tok, s.type);
        }
        return true;
    }

    bool parse_ctab(size_t payload, size_t len) {
        // payload points at the fourcc; the table follows it.
        if (len < 1 + 7) return true;  // too short to be a table: ignore
        const size_t base = (payload + 1) * 4;  // byte offset of the table
        const size_t bytes = (len - 1) * 4;
        auto u32 = [&](size_t off, uint32_t& v) {
            if (off + 4 > bytes) return false;
            std::memcpy(&v, r.data + base + off, 4);
            return true;
        };
        auto u16 = [&](size_t off, uint16_t& v) {
            if (off + 2 > bytes) return false;
            std::memcpy(&v, r.data + base + off, 2);
            return true;
        };
        auto str = [&](uint32_t off, std::string& s) {
            s.clear();
            for (size_t i = off; i < bytes && s.size() < 256; ++i) {
                const char c = static_cast<char>(r.data[base + i]);
                if (c == 0) return true;
                s.push_back(c);
            }
            return false;
        };
        uint32_t count = 0, info = 0;
        if (!u32(12, count) || !u32(16, info)) return true;
        if (count > 4096) return true;  // garbage: ignore the table (it is advisory)
        p.has_ctab = true;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t e = static_cast<size_t>(info) + static_cast<size_t>(i) * 20u;
            uint32_t name = 0, type_info = 0;
            CtabEntry c;
            if (!u32(e, name) || !u16(e + 4, c.register_set) || !u16(e + 6, c.register_index) ||
                !u16(e + 8, c.register_count) || !u32(e + 12, type_info))
                break;
            str(name, c.name);
            u16(type_info, c.cls);
            u16(type_info + 2, c.type);
            p.ctab.push_back(std::move(c));
        }
        return true;
    }

    bool run() {
        if (r.words < 2) return fail(err, "bytecode too short", 0);
        const uint32_t v = r.at(0);
        const uint32_t kind = v >> 16;
        if (kind == 0xFFFEu) p.type = ShaderType::Vertex;
        else if (kind == 0xFFFFu) p.type = ShaderType::Pixel;
        else return fail(err, "not a D3D9 shader version token (0x%08x)", 0, v);
        p.major = static_cast<uint8_t>((v >> 8) & 0xFFu);
        p.minor = static_cast<uint8_t>(v & 0xFFu);
        const bool ok_version = p.is_vs() ? ((p.major == 1 && p.minor <= 1) || (p.major == 2 && p.minor <= 1) ||
                                             (p.major == 3 && p.minor == 0))
                                          : ((p.major == 1 && p.minor <= 4) || (p.major == 2 && p.minor <= 1) ||
                                             (p.major == 3 && p.minor == 0));
        if (!ok_version) return fail(err, "unsupported shader version %u.%u", 0, p.major, p.minor);
        pos = 1;
        while (true) {
            if (pos >= r.words) return fail(err, "missing END token", pos);
            const uint32_t tok = r.at(pos);
            if (tok == 0x0000FFFFu) {
                p.token_count = static_cast<uint32_t>(pos + 1);
                return true;
            }
            const uint32_t opcode = tok & 0xFFFFu;
            if (opcode == 0xFFFEu) {
                if (tok & 0x80000000u) return fail(err, "malformed comment token", pos);
                const size_t len = (tok >> 16) & 0x7FFFu;
                if (pos + 1 + len > r.words) return fail(err, "comment runs past the end", pos);
                if (len >= 1) {
                    const uint32_t fourcc = r.at(pos + 1);
                    if (fourcc == 0x42415443u) parse_ctab(pos + 1, len);       // 'CTAB'
                    else if (fourcc == 0x53455250u) p.has_preshader = true;    // 'PRES'
                }
                pos += 1 + len;
                continue;
            }
            if (tok & 0x80000000u) return fail(err, "operand token where an instruction was expected", pos);
            if (p.instrs.size() >= kMaxInstructions) return fail(err, "too many instructions", pos);
            Instr in;
            in.token = static_cast<uint32_t>(pos);
            in.op = static_cast<Op>(opcode);
            in.specific = static_cast<uint8_t>((tok >> 16) & 0xFFu);
            in.coissue = (tok & (1u << 30)) != 0;
            in.predicated = (tok & (1u << 28)) != 0;
            const size_t length = (tok >> 24) & 0xFu;
            ++pos;
            const size_t start = pos;
            if (in.coissue && !(p.is_ps() && p.major == 1)) return fail(err, "co-issue outside ps_1_x", in.token);
            if (in.predicated && p.major < 2) return fail(err, "predication before SM2", in.token);
            if (in.op == Op::Phase) {
                if (!(p.is_ps() && p.major == 1 && p.minor == 4)) return fail(err, "phase outside ps_1_4", in.token);
            } else if (in.op == Op::Dcl) {
                if (!need(2)) return fail(err, "truncated dcl", in.token);
                const uint32_t u = r.at(pos);
                if (!(u & 0x80000000u)) return fail(err, "dcl usage token without bit 31", pos);
                ++pos;
                if (!read_dst(in.dst)) return false;
                in.has_dst = true;
                const uint32_t usage = u & 0x1Fu;
                const uint32_t tt = (u >> 27) & 0xFu;
                in.usage_index = static_cast<uint8_t>((u >> 16) & 0xFu);
                if (in.dst.type == reg::kSampler) {
                    if (tt == 1 || tt == 2) in.tex_dim = TextureDim::Tex2D;
                    else if (tt == 3) in.tex_dim = TextureDim::Cube;
                    else if (tt == 4) in.tex_dim = TextureDim::Volume;
                    else return fail(err, "unknown sampler texture type %u", in.token, tt);
                } else {
                    if (usage > static_cast<uint32_t>(Usage::Sample)) return fail(err, "unknown dcl usage %u", in.token, usage);
                    in.usage = static_cast<Usage>(usage);
                }
                const uint8_t t = in.dst.type;
                const bool okreg = p.is_vs() ? (t == reg::kInput || t == reg::kOutput || t == reg::kSampler)
                                             : (t == reg::kInput || t == reg::kTexture || t == reg::kSampler ||
                                                t == reg::kMiscType);
                if (!okreg) return fail(err, "dcl on register type %u", in.token, t);
                if (!check_register(p, t, in.dst.index, false, nullptr, 0) && t != reg::kOutput)
                    return fail(err, "dcl register index %u out of range", in.token, in.dst.index);
                if (t == reg::kOutput && in.dst.index >= 12) return fail(err, "dcl output index %u", in.token, in.dst.index);
            } else if (in.op == Op::Def || in.op == Op::Defi || in.op == Op::Defb) {
                const size_t nlit = in.op == Op::Defb ? 1 : 4;
                if (!need(1 + nlit)) return fail(err, "truncated def", in.token);
                if (!read_dst(in.dst)) return false;
                in.has_dst = true;
                const uint8_t want = in.op == Op::Def ? reg::kConst : in.op == Op::Defi ? reg::kConstInt : reg::kConstBool;
                if (in.dst.type != want) return fail(err, "def into register type %u", in.token, in.dst.type);
                if (!check_register(p, in.dst.type, in.dst.index, false, err, in.token)) return false;
                for (size_t i = 0; i < nlit; ++i) in.literal[i] = r.at(pos + i);
                pos += nlit;
            } else {
                const Counts c = operand_counts(in.op, p);
                if (c.dst < 0) return fail(err, "unknown opcode %u", in.token, opcode);
                if (c.dst > 0) {
                    if (!read_dst(in.dst)) return false;
                    in.has_dst = true;
                    if (!check_register(p, in.dst.type, in.dst.index, in.op != Op::TexKill, err, in.token)) return false;
                }
                if (in.predicated) {
                    if (!read_src(in.pred)) return false;
                    if (in.pred.type != reg::kPredicate) return fail(err, "predicate operand of type %u", in.token, in.pred.type);
                }
                in.nsrc = static_cast<uint8_t>(c.src);
                for (int i = 0; i < c.src; ++i) {
                    Src& s = in.src[static_cast<size_t>(i)];
                    if (!read_src(s)) return false;
                    if (!check_register(p, s.type, s.index, false, err, in.token)) return false;
                }
                if (!check_operand_kinds(p, in, err)) return false;
            }
            if (p.major >= 2 && length != 0 && pos - start != length)
                return fail(err, "instruction length %u does not match its operands (%u)", in.token,
                            static_cast<unsigned>(length), static_cast<unsigned>(pos - start));
            p.instrs.push_back(in);
        }
    }
};

const char kComp[4] = {'x', 'y', 'z', 'w'};

}  // namespace

const char* op_name(Op op) {
    switch (op) {
    case Op::Nop: return "nop";
    case Op::Mov: return "mov";
    case Op::Add: return "add";
    case Op::Sub: return "sub";
    case Op::Mad: return "mad";
    case Op::Mul: return "mul";
    case Op::Rcp: return "rcp";
    case Op::Rsq: return "rsq";
    case Op::Dp3: return "dp3";
    case Op::Dp4: return "dp4";
    case Op::Min: return "min";
    case Op::Max: return "max";
    case Op::Slt: return "slt";
    case Op::Sge: return "sge";
    case Op::Exp: return "exp";
    case Op::Log: return "log";
    case Op::Lit: return "lit";
    case Op::Dst: return "dst";
    case Op::Lrp: return "lrp";
    case Op::Frc: return "frc";
    case Op::M4x4: return "m4x4";
    case Op::M4x3: return "m4x3";
    case Op::M3x4: return "m3x4";
    case Op::M3x3: return "m3x3";
    case Op::M3x2: return "m3x2";
    case Op::Call: return "call";
    case Op::CallNz: return "callnz";
    case Op::Loop: return "loop";
    case Op::Ret: return "ret";
    case Op::EndLoop: return "endloop";
    case Op::Label: return "label";
    case Op::Dcl: return "dcl";
    case Op::Pow: return "pow";
    case Op::Crs: return "crs";
    case Op::Sgn: return "sgn";
    case Op::Abs: return "abs";
    case Op::Nrm: return "nrm";
    case Op::SinCos: return "sincos";
    case Op::Rep: return "rep";
    case Op::EndRep: return "endrep";
    case Op::If: return "if";
    case Op::Ifc: return "ifc";
    case Op::Else: return "else";
    case Op::EndIf: return "endif";
    case Op::Break: return "break";
    case Op::Breakc: return "breakc";
    case Op::Mova: return "mova";
    case Op::Defb: return "defb";
    case Op::Defi: return "defi";
    case Op::TexCoord: return "texcoord";
    case Op::TexKill: return "texkill";
    case Op::Tex: return "tex";
    case Op::TexBem: return "texbem";
    case Op::TexBemL: return "texbeml";
    case Op::TexReg2Ar: return "texreg2ar";
    case Op::TexReg2Gb: return "texreg2gb";
    case Op::TexM3x2Pad: return "texm3x2pad";
    case Op::TexM3x2Tex: return "texm3x2tex";
    case Op::TexM3x3Pad: return "texm3x3pad";
    case Op::TexM3x3Tex: return "texm3x3tex";
    case Op::Reserved0: return "reserved0";
    case Op::TexM3x3Spec: return "texm3x3spec";
    case Op::TexM3x3VSpec: return "texm3x3vspec";
    case Op::ExpP: return "expp";
    case Op::LogP: return "logp";
    case Op::Cnd: return "cnd";
    case Op::Def: return "def";
    case Op::TexReg2Rgb: return "texreg2rgb";
    case Op::TexDp3Tex: return "texdp3tex";
    case Op::TexM3x2Depth: return "texm3x2depth";
    case Op::TexDp3: return "texdp3";
    case Op::TexM3x3: return "texm3x3";
    case Op::TexDepth: return "texdepth";
    case Op::Cmp: return "cmp";
    case Op::Bem: return "bem";
    case Op::Dp2Add: return "dp2add";
    case Op::Dsx: return "dsx";
    case Op::Dsy: return "dsy";
    case Op::TexLdd: return "texldd";
    case Op::Setp: return "setp";
    case Op::TexLdl: return "texldl";
    case Op::Breakp: return "breakp";
    case Op::Phase: return "phase";
    case Op::Comment: return "comment";
    case Op::End: return "end";
    }
    return "?";
}

const char* usage_name(Usage u) {
    static const char* const k[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                                    "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
    const auto i = static_cast<size_t>(u);
    return i < sizeof(k) / sizeof(k[0]) ? k[i] : "UNKNOWN";
}

uint32_t Program::version_token() const {
    return (is_vs() ? 0xFFFE0000u : 0xFFFF0000u) | (static_cast<uint32_t>(major) << 8) | minor;
}

std::string Program::profile() const {
    std::string s = is_vs() ? "vs_" : "ps_";
    s += static_cast<char>('0' + major);
    s += '_';
    if (major == 2 && minor == 1) s += 'x';
    else s += static_cast<char>('0' + minor);
    return s;
}

bool parse(std::span<const uint8_t> bytes, Program& out, std::string* err) {
    out = Program{};
    if (bytes.size() > kMaxBytecodeBytes) return fail(err, "bytecode larger than %u bytes", 0, static_cast<unsigned>(kMaxBytecodeBytes));
    Parser ps{{bytes.data(), bytes.size() / 4}, out, err};
    if (!ps.run()) {
        out.instrs.clear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- disassembler

namespace {

std::string reg_name(const Program& p, uint8_t type, uint32_t index) {
    const std::string n = std::to_string(index);
    switch (type) {
    case reg::kTemp: return "r" + n;
    case reg::kInput: return "v" + n;
    case reg::kConst: return "c" + n;
    case reg::kAddr: return p.is_vs() ? "a" + n : "t" + n;
    case reg::kRastOut: return index == 0 ? "oPos" : index == 1 ? "oFog" : "oPts";
    case reg::kAttrOut: return "oD" + n;
    case reg::kTexCrdOut: return p.major >= 3 ? "o" + n : "oT" + n;
    case reg::kConstInt: return "i" + n;
    case reg::kColorOut: return "oC" + n;
    case reg::kDepthOut: return "oDepth";
    case reg::kSampler: return "s" + n;
    case reg::kConstBool: return "b" + n;
    case reg::kLoop: return "aL";
    case reg::kMiscType: return index == 0 ? "vPos" : "vFace";
    case reg::kLabel: return "l" + n;
    case reg::kPredicate: return "p" + n;
    default: return "?" + n;
    }
}

std::string mask_str(uint8_t m) {
    if (m == 0xF) return {};
    std::string s = ".";
    for (int i = 0; i < 4; ++i)
        if (m & (1u << i)) s += kComp[i];
    return s;
}

std::string swz_str(const std::array<uint8_t, 4>& w) {
    if (w[0] == 0 && w[1] == 1 && w[2] == 2 && w[3] == 3) return {};
    std::string s = ".";
    if (w[0] == w[1] && w[1] == w[2] && w[2] == w[3]) return s + kComp[w[0]];
    for (int i = 0; i < 4; ++i) s += kComp[w[static_cast<size_t>(i)]];
    return s;
}

std::string src_str(const Program& p, const Src& s) {
    std::string r = reg_name(p, s.type, s.index);
    if (s.relative) {
        const std::string a = s.rel_type == reg::kLoop ? "aL" : std::string("a0.") + kComp[s.rel_comp];
        r = reg_name(p, s.type, 0).substr(0, 1) + "[" + a + " + " + std::to_string(s.index) + "]";
    }
    r += swz_str(s.swz);
    switch (s.mod) {
    case SrcMod::None: return r;
    case SrcMod::Neg: return "-" + r;
    case SrcMod::Bias: return r + "_bias";
    case SrcMod::BiasNeg: return "-" + r + "_bias";
    case SrcMod::Sign: return r + "_bx2";
    case SrcMod::SignNeg: return "-" + r + "_bx2";
    case SrcMod::Comp: return "1-" + r;
    case SrcMod::X2: return r + "_x2";
    case SrcMod::X2Neg: return "-" + r + "_x2";
    case SrcMod::Dz: return r + "_dz";
    case SrcMod::Dw: return r + "_dw";
    case SrcMod::Abs: return r + "_abs";
    case SrcMod::AbsNeg: return "-" + r + "_abs";
    case SrcMod::Not: return "!" + r;
    }
    return r;
}

std::string float_str(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    return buf;
}

}  // namespace

std::string disassemble(const Program& p, const Instr& in) {
    std::string s = in.coissue ? "+" : "";
    if (in.predicated) s += "(" + src_str(p, in.pred) + ") ";
    std::string name = op_name(in.op);
    if (in.op == Op::Tex) {
        if (p.major >= 2) name = (in.specific & kTexldProject) ? "texldp" : (in.specific & kTexldBias) ? "texldb" : "texld";
        else if (p.minor >= 4) name = "texld";
    } else if (in.op == Op::TexCoord && p.is_ps() && p.major == 1 && p.minor >= 4) {
        name = "texcrd";
    } else if (in.op == Op::Ifc || in.op == Op::Breakc || in.op == Op::Setp) {
        static const char* const k[] = {"", "_gt", "_eq", "_ge", "_lt", "_ne", "_le", ""};
        name += k[in.specific & 7u];
    } else if (in.op == Op::Dcl) {
        if (in.dst.type == reg::kSampler) {
            name = in.tex_dim == TextureDim::Cube ? "dcl_cube" : in.tex_dim == TextureDim::Volume ? "dcl_volume" : "dcl_2d";
        } else if (p.is_vs() || p.major >= 3) {
            const std::string u = ascii_lower(usage_name(in.usage));
            name = "dcl_" + u + (in.usage_index ? std::to_string(in.usage_index) : std::string());
        }
    }
    if (in.has_dst) {
        if (in.dst.saturate) name += "_sat";
        if (in.dst.partial_precision) name += "_pp";
        if (in.dst.centroid) name += "_centroid";
        static const char* const sh[] = {"_d8", "_d4", "_d2", "", "_x2", "_x4", "_x8"};
        name += sh[in.dst.shift + 3];
    }
    s += name;
    bool first = true;
    auto sep = [&] {
        s += first ? " " : ", ";
        first = false;
    };
    if (in.has_dst) {
        sep();
        s += reg_name(p, in.dst.type, in.dst.index) + mask_str(in.dst.mask);
    }
    if (in.op == Op::Def) {
        for (int i = 0; i < 4; ++i) {
            sep();
            s += float_str(in.literal[static_cast<size_t>(i)]);
        }
    } else if (in.op == Op::Defi) {
        for (int i = 0; i < 4; ++i) {
            sep();
            s += std::to_string(static_cast<int32_t>(in.literal[static_cast<size_t>(i)]));
        }
    } else if (in.op == Op::Defb) {
        sep();
        s += in.literal[0] ? "true" : "false";
    }
    for (int i = 0; i < in.nsrc; ++i) {
        sep();
        s += src_str(p, in.src[static_cast<size_t>(i)]);
    }
    return s;
}

std::string disassemble(const Program& p) {
    std::string s = p.profile() + "\n";
    for (const Instr& in : p.instrs) s += "    " + disassemble(p, in) + "\n";
    return s;
}

}  // namespace alamo::shaderxlat

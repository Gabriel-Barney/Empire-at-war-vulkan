#include "mini_asm.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "alamo/shaderxlat/bytecode.hpp"

namespace alamo::shaderxlat::test {

namespace {

struct Ctx {
    bool ps = false;
    int major = 0, minor = 0;
    std::string* err = nullptr;
    int line = 0;
    bool fail(const std::string& m) {
        if (err) *err = "line " + std::to_string(line) + ": " + m;
        return false;
    }
};

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '[') ++depth;
        if (c == ']') --depth;
        if (c == sep && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

int comp_index(char c) {
    switch (c) {
    case 'x': case 'r': return 0;
    case 'y': case 'g': return 1;
    case 'z': case 'b': return 2;
    case 'w': case 'a': return 3;
    default: return -1;
    }
}

uint32_t reg_bits(uint8_t type, uint32_t idx) {
    return 0x80000000u | ((static_cast<uint32_t>(type) & 7u) << 28) | ((static_cast<uint32_t>(type) & 0x18u) << 8) | (idx & 0x7FFu);
}

struct Reg {
    uint8_t type = 0;
    uint32_t index = 0;
    bool relative = false;
    uint8_t rel_type = reg::kAddr;
    uint8_t rel_comp = 0;
};

bool parse_reg(Ctx& c, const std::string& s, Reg& r) {
    auto num = [&](size_t from, uint32_t& v) {
        if (from >= s.size()) return false;
        char* end = nullptr;
        const unsigned long x = std::strtoul(s.c_str() + from, &end, 10);
        if (end != s.c_str() + s.size()) return false;
        v = static_cast<uint32_t>(x);
        return true;
    };
    if (s.size() >= 2 && s[0] == 'c' && s[1] == '[') {
        // c[a0.x + 4] / c[aL + 4]
        const size_t close = s.find(']');
        if (close == std::string::npos) return c.fail("bad relative register " + s);
        const std::string inner = s.substr(2, close - 2);
        const size_t plus = inner.find('+');
        const std::string a = trim(inner.substr(0, plus));
        const std::string off = plus == std::string::npos ? "0" : trim(inner.substr(plus + 1));
        r.type = reg::kConst;
        r.relative = true;
        r.index = static_cast<uint32_t>(std::strtoul(off.c_str(), nullptr, 10));
        if (a == "aL") {
            r.rel_type = reg::kLoop;
        } else if (a.size() == 4 && a.substr(0, 3) == "a0.") {
            r.rel_type = reg::kAddr;
            const int ci = comp_index(a[3]);
            if (ci < 0) return c.fail("bad address component");
            r.rel_comp = static_cast<uint8_t>(ci);
        } else {
            return c.fail("bad address register " + a);
        }
        return true;
    }
    struct Named {
        const char* name;
        uint8_t type;
        uint32_t index;
    };
    static const Named named[] = {{"oPos", reg::kRastOut, 0}, {"oFog", reg::kRastOut, 1}, {"oPts", reg::kRastOut, 2},
                                  {"oDepth", reg::kDepthOut, 0}, {"aL", reg::kLoop, 0}, {"vPos", reg::kMiscType, 0},
                                  {"vFace", reg::kMiscType, 1}};
    for (const Named& n : named)
        if (s == n.name) {
            r.type = n.type;
            r.index = n.index;
            return true;
        }
    struct Prefix {
        const char* p;
        uint8_t type;
    };
    const Prefix prefixes[] = {{"oD", reg::kAttrOut}, {"oT", reg::kTexCrdOut}, {"oC", reg::kColorOut}, {"o", reg::kOutput},
                               {"r", reg::kTemp}, {"v", reg::kInput}, {"c", reg::kConst}, {"a", reg::kAddr},
                               {"t", reg::kTexture}, {"s", reg::kSampler}, {"i", reg::kConstInt}, {"b", reg::kConstBool},
                               {"p", reg::kPredicate}, {"l", reg::kLabel}};
    for (const Prefix& p : prefixes) {
        const size_t n = std::strlen(p.p);
        if (s.size() > n && s.compare(0, n, p.p) == 0 && std::isdigit(static_cast<unsigned char>(s[n]))) {
            r.type = p.type;
            if (!num(n, r.index)) return c.fail("bad register " + s);
            return true;
        }
    }
    return c.fail("unknown register " + s);
}

bool parse_mask(Ctx& c, const std::string& s, uint8_t& mask) {
    mask = 0;
    for (char ch : s) {
        const int i = comp_index(ch);
        if (i < 0) return c.fail("bad mask ." + s);
        mask = static_cast<uint8_t>(mask | (1u << i));
    }
    return true;
}

bool parse_swz(Ctx& c, const std::string& s, uint32_t& swz) {
    int sel[4] = {0, 1, 2, 3};
    if (!s.empty()) {
        for (size_t i = 0; i < 4; ++i) {
            const char ch = i < s.size() ? s[i] : s.back();
            const int k = comp_index(ch);
            if (k < 0 || s.size() > 4) return c.fail("bad swizzle ." + s);
            sel[i] = k;
        }
    }
    swz = static_cast<uint32_t>(sel[0] | (sel[1] << 2) | (sel[2] << 4) | (sel[3] << 6));
    return true;
}

bool dst_token(Ctx& c, const std::string& s, uint32_t mods, int shift, uint32_t& tok) {
    const size_t dot = s.find('.');
    Reg r;
    if (!parse_reg(c, s.substr(0, dot), r)) return false;
    uint8_t mask = 0xF;
    if (dot != std::string::npos && !parse_mask(c, s.substr(dot + 1), mask)) return false;
    tok = reg_bits(r.type, r.index) | (static_cast<uint32_t>(mask) << 16) | (mods << 20) | ((static_cast<uint32_t>(shift) & 0xFu) << 24);
    return true;
}

bool src_tokens(Ctx& c, std::string s, std::vector<uint32_t>& out) {
    bool neg = false, comp = false, nott = false;
    if (!s.empty() && s[0] == '!') {
        nott = true;
        s = s.substr(1);
    }
    if (!s.empty() && s[0] == '-') {
        neg = true;
        s = s.substr(1);
    }
    if (s.size() > 2 && s[0] == '1' && s[1] == '-') {
        comp = true;
        s = s.substr(2);
    }
    // suffix modifier
    std::string suffix;
    const size_t us = s.find('_', s.find(']') == std::string::npos ? 0 : s.find(']'));
    if (us != std::string::npos) {
        suffix = s.substr(us + 1);
        s = s.substr(0, us);
    }
    std::string swz;
    const size_t close = s.find(']');
    const size_t dot = s.find('.', close == std::string::npos ? 0 : close);
    if (dot != std::string::npos) {
        swz = s.substr(dot + 1);
        s = s.substr(0, dot);
    }
    // "c2_bx2.z" (D3D order) as well as "c2.z_bx2" (disassemble() order).
    const size_t sdot = suffix.find('.');
    if (sdot != std::string::npos) {
        swz = suffix.substr(sdot + 1);
        suffix = suffix.substr(0, sdot);
    }
    Reg r;
    if (!parse_reg(c, s, r)) return false;
    uint32_t sw = 0;
    if (!parse_swz(c, swz, sw)) return false;
    SrcMod mod = SrcMod::None;
    if (suffix.empty()) mod = neg ? SrcMod::Neg : SrcMod::None;
    else if (suffix == "bias") mod = neg ? SrcMod::BiasNeg : SrcMod::Bias;
    else if (suffix == "bx2") mod = neg ? SrcMod::SignNeg : SrcMod::Sign;
    else if (suffix == "x2") mod = neg ? SrcMod::X2Neg : SrcMod::X2;
    else if (suffix == "abs") mod = neg ? SrcMod::AbsNeg : SrcMod::Abs;
    else if (suffix == "dz") mod = SrcMod::Dz;
    else if (suffix == "dw") mod = SrcMod::Dw;
    else return c.fail("bad source modifier _" + suffix);
    if (comp) mod = SrcMod::Comp;
    if (nott) mod = SrcMod::Not;
    uint32_t t = reg_bits(r.type, r.index) | (sw << 16) | (static_cast<uint32_t>(mod) << 24);
    if (r.relative) t |= 1u << 13;
    out.push_back(t);
    if (r.relative && c.major >= 2) {
        out.push_back(reg_bits(r.rel_type, 0) | (static_cast<uint32_t>(r.rel_comp) * 0x55u << 16));
    }
    return true;
}

struct OpName {
    const char* name;
    Op op;
};
const OpName kOps[] = {
    {"nop", Op::Nop}, {"mov", Op::Mov}, {"add", Op::Add}, {"sub", Op::Sub}, {"mad", Op::Mad}, {"mul", Op::Mul},
    {"rcp", Op::Rcp}, {"rsq", Op::Rsq}, {"dp3", Op::Dp3}, {"dp4", Op::Dp4}, {"min", Op::Min}, {"max", Op::Max},
    {"slt", Op::Slt}, {"sge", Op::Sge}, {"exp", Op::Exp}, {"log", Op::Log}, {"lit", Op::Lit}, {"dst", Op::Dst},
    {"lrp", Op::Lrp}, {"frc", Op::Frc}, {"m4x4", Op::M4x4}, {"m4x3", Op::M4x3}, {"m3x4", Op::M3x4}, {"m3x3", Op::M3x3},
    {"m3x2", Op::M3x2}, {"call", Op::Call}, {"callnz", Op::CallNz}, {"loop", Op::Loop}, {"ret", Op::Ret},
    {"endloop", Op::EndLoop}, {"label", Op::Label}, {"pow", Op::Pow}, {"crs", Op::Crs}, {"sgn", Op::Sgn},
    {"abs", Op::Abs}, {"nrm", Op::Nrm}, {"sincos", Op::SinCos}, {"rep", Op::Rep}, {"endrep", Op::EndRep},
    {"if", Op::If}, {"ifc", Op::Ifc}, {"else", Op::Else}, {"endif", Op::EndIf}, {"break", Op::Break},
    {"breakc", Op::Breakc}, {"mova", Op::Mova}, {"texcoord", Op::TexCoord}, {"texcrd", Op::TexCoord},
    {"texkill", Op::TexKill}, {"tex", Op::Tex}, {"texld", Op::Tex}, {"texldp", Op::Tex}, {"texldb", Op::Tex},
    {"texbem", Op::TexBem}, {"texbeml", Op::TexBemL}, {"texreg2ar", Op::TexReg2Ar}, {"texreg2gb", Op::TexReg2Gb},
    {"texm3x2pad", Op::TexM3x2Pad}, {"texm3x2tex", Op::TexM3x2Tex}, {"texm3x3pad", Op::TexM3x3Pad},
    {"texm3x3tex", Op::TexM3x3Tex}, {"texm3x3spec", Op::TexM3x3Spec}, {"texm3x3vspec", Op::TexM3x3VSpec},
    {"expp", Op::ExpP}, {"logp", Op::LogP}, {"cnd", Op::Cnd}, {"texreg2rgb", Op::TexReg2Rgb},
    {"texdp3tex", Op::TexDp3Tex}, {"texm3x2depth", Op::TexM3x2Depth}, {"texdp3", Op::TexDp3}, {"texm3x3", Op::TexM3x3},
    {"texdepth", Op::TexDepth}, {"cmp", Op::Cmp}, {"bem", Op::Bem}, {"dp2add", Op::Dp2Add}, {"dsx", Op::Dsx},
    {"dsy", Op::Dsy}, {"texldd", Op::TexLdd}, {"setp", Op::Setp}, {"breakp", Op::Breakp}, {"phase", Op::Phase},
};

uint32_t usage_from(const std::string& s, bool& ok) {
    static const char* const k[] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent",
                                    "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"};
    for (uint32_t i = 0; i < 14; ++i)
        if (s == k[i]) {
            ok = true;
            return i;
        }
    ok = false;
    return 0;
}

}  // namespace

bool assemble(std::string_view text, std::vector<uint32_t>& tokens, std::string* err) {
    tokens.clear();
    Ctx c;
    c.err = err;
    bool have_version = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string ln(text.substr(pos, end - pos));
        pos = end + 1;
        ++c.line;
        const size_t cm = ln.find("//");
        if (cm != std::string::npos) ln = ln.substr(0, cm);
        ln = trim(ln);
        if (ln.empty()) {
            if (end == text.size()) break;
            continue;
        }
        if (!have_version) {
            if (ln.size() != 6 || (ln.compare(0, 3, "vs_") != 0 && ln.compare(0, 3, "ps_") != 0) || ln[4] != '_')
                return c.fail("expected a profile, got " + ln);
            c.ps = ln[0] == 'p';
            c.major = ln[3] - '0';
            c.minor = ln[5] == 'x' ? 1 : ln[5] - '0';
            tokens.push_back((c.ps ? 0xFFFF0000u : 0xFFFE0000u) | (static_cast<uint32_t>(c.major) << 8) | static_cast<uint32_t>(c.minor));
            have_version = true;
            continue;
        }
        bool coissue = false;
        if (ln[0] == '+') {
            coissue = true;
            ln = trim(ln.substr(1));
        }
        std::vector<uint32_t> pred;
        if (ln[0] == '(') {
            const size_t close = ln.find(')');
            if (close == std::string::npos) return c.fail("bad predicate");
            if (!src_tokens(c, ln.substr(1, close - 1), pred)) return false;
            ln = trim(ln.substr(close + 1));
        }
        const size_t sp = ln.find(' ');
        const std::string mnem = ln.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : trim(ln.substr(sp + 1));
        std::vector<std::string> ops = rest.empty() ? std::vector<std::string>() : split(rest, ',');
        std::vector<std::string> parts = split(mnem, '_');
        if (parts.empty()) return c.fail("empty mnemonic");
        // Instruction token fields.
        Op op = Op::Nop;
        uint32_t specific = 0, mods = 0;
        int shift = 0;
        std::vector<uint32_t> body;
        size_t first_mod = 1;
        const std::string& base = parts[0];
        if (base == "dcl") {
            uint32_t usage_tok = 0x80000000u;
            size_t k = 1;
            if (parts.size() > 1) {
                const std::string& u = parts[1];
                if (u == "2d") { usage_tok |= 2u << 27; k = 2; }
                else if (u == "cube") { usage_tok |= 3u << 27; k = 2; }
                else if (u == "volume") { usage_tok |= 4u << 27; k = 2; }
                else if (u != "pp" && u != "centroid" && u != "sat") {
                    std::string name = u;
                    uint32_t idx = 0;
                    while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back()))) name.pop_back();
                    if (name.size() < u.size()) idx = static_cast<uint32_t>(std::strtoul(u.c_str() + name.size(), nullptr, 10));
                    bool ok = false;
                    usage_tok |= usage_from(name, ok) | (idx << 16);
                    if (!ok) return c.fail("bad dcl usage " + u);
                    k = 2;
                }
            }
            for (; k < parts.size(); ++k) {
                if (parts[k] == "pp") mods |= 2;
                else if (parts[k] == "centroid") mods |= 4;
                else if (parts[k] == "sat") mods |= 1;
                else return c.fail("bad dcl modifier " + parts[k]);
            }
            if (ops.size() != 1) return c.fail("dcl takes one operand");
            uint32_t d = 0;
            if (!dst_token(c, ops[0], mods, 0, d)) return false;
            op = Op::Dcl;
            body = {usage_tok, d};
        } else if (base == "def" || base == "defi" || base == "defb") {
            op = base == "def" ? Op::Def : base == "defi" ? Op::Defi : Op::Defb;
            const size_t want = op == Op::Defb ? 2 : 5;
            if (ops.size() != want) return c.fail("def operand count");
            uint32_t d = 0;
            if (!dst_token(c, ops[0], 0, 0, d)) return false;
            body.push_back(d);
            for (size_t k = 1; k < ops.size(); ++k) {
                if (op == Op::Def) {
                    const float f = std::strtof(ops[k].c_str(), nullptr);
                    uint32_t b;
                    std::memcpy(&b, &f, 4);
                    body.push_back(b);
                } else if (op == Op::Defi) {
                    body.push_back(static_cast<uint32_t>(std::strtol(ops[k].c_str(), nullptr, 10)));
                } else {
                    body.push_back(ops[k] == "true" ? 1u : 0u);
                }
            }
        } else {
            bool found = false;
            for (const OpName& o : kOps)
                if (base == o.name) {
                    op = o.op;
                    found = true;
                }
            if (!found) return c.fail("unknown instruction " + base);
            if (base == "texldp") specific = kTexldProject;
            if (base == "texldb") specific = kTexldBias;
            if ((op == Op::Ifc || op == Op::Breakc || op == Op::Setp) && parts.size() > 1) {
                static const char* const cmpn[] = {"", "gt", "eq", "ge", "lt", "ne", "le"};
                for (uint32_t k = 1; k < 7; ++k)
                    if (parts[1] == cmpn[k]) specific = k;
                if (!specific) return c.fail("bad comparison " + parts[1]);
                first_mod = 2;
            }
            for (size_t k = first_mod; k < parts.size(); ++k) {
                const std::string& m = parts[k];
                if (m == "sat") mods |= 1;
                else if (m == "pp") mods |= 2;
                else if (m == "centroid") mods |= 4;
                else if (m == "x2") shift = 1;
                else if (m == "x4") shift = 2;
                else if (m == "x8") shift = 3;
                else if (m == "d2") shift = -1;
                else if (m == "d4") shift = -2;
                else if (m == "d8") shift = -3;
                else return c.fail("bad modifier _" + m);
            }
            // Which operand is a destination?
            bool has_dst = true;
            switch (op) {
            case Op::Nop: case Op::Ret: case Op::EndLoop: case Op::EndRep: case Op::Else: case Op::EndIf:
            case Op::Break: case Op::Phase: case Op::Call: case Op::CallNz: case Op::Loop: case Op::Label: case Op::Rep:
            case Op::If: case Op::Ifc: case Op::Breakc: case Op::Breakp:
                has_dst = false;
                break;
            default:
                break;
            }
            for (size_t k = 0; k < ops.size(); ++k) {
                if (k == 0 && has_dst) {
                    uint32_t d = 0;
                    if (!dst_token(c, ops[0], mods, shift, d)) return false;
                    body.push_back(d);
                    body.insert(body.end(), pred.begin(), pred.end());
                } else {
                    if (k == 0) body.insert(body.end(), pred.begin(), pred.end());
                    if (!src_tokens(c, ops[k], body)) return false;
                }
            }
            if (ops.empty()) body.insert(body.end(), pred.begin(), pred.end());
        }
        uint32_t tok = static_cast<uint32_t>(op) | (specific << 16);
        if (c.major >= 2 && op != Op::Phase) tok |= static_cast<uint32_t>(body.size() & 0xF) << 24;
        if (!pred.empty()) tok |= 1u << 28;
        if (coissue) tok |= 1u << 30;
        tokens.push_back(tok);
        tokens.insert(tokens.end(), body.begin(), body.end());
        if (end == text.size()) break;
    }
    if (!have_version) return c.fail("empty program");
    tokens.push_back(0x0000FFFFu);
    return true;
}

std::vector<uint8_t> to_bytes(const std::vector<uint32_t>& tokens) {
    std::vector<uint8_t> b(tokens.size() * 4);
    std::memcpy(b.data(), tokens.data(), b.size());
    return b;
}

}  // namespace alamo::shaderxlat::test

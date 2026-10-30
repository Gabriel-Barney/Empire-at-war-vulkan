// FXLC parsing and evaluation. See fxlc.hpp.
#include "alamo/materials/fxlc.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "fx2_reader.hpp"

namespace alamo::materials {
namespace {

constexpr uint32_t kTabLiteral = 1, kTabInput = 2, kTabOutput = 4, kTabTemp = 7;

uint32_t word(const uint8_t* d, size_t n, size_t i) {
    if ((i + 1) * 4 > n) return 0;
    uint32_t v;
    std::memcpy(&v, d + 4 * i, 4);
    return v;
}

enum Op : uint32_t {
    kMov = 0x100, kNeg = 0x101, kRcp = 0x103, kFrc = 0x104, kExp = 0x105, kLog = 0x106, kRsq = 0x107,
    kSin = 0x108, kCos = 0x109, kSqrt = 0x112, kFloor = 0x139, kCeil = 0x13A, kMin = 0x200, kMax = 0x201,
    kLt = 0x202, kGe = 0x203, kAdd = 0x204, kMul = 0x205, kDiv = 0x208, kCmp = 0x300, kMovc = 0x301,
    kDot = 0x500, kNop = 0x000
};

bool supported_op(uint32_t op) {
    switch (op) {
    case kMov: case kNeg: case kRcp: case kFrc: case kExp: case kLog: case kRsq: case kSin: case kCos:
    case kSqrt: case kFloor: case kCeil: case kMin: case kMax: case kLt: case kGe: case kAdd: case kMul:
    case kDiv: case kCmp: case kMovc: case kDot: case kNop:
        return true;
    default:
        return false;
    }
}

// One component of a non-dot instruction (shared by run_expression and PreparedExpression::run).
float apply_op(uint32_t op, float a, float b, float d) {
    switch (op) {
    case kNop: return 0.0f;
    case kMov: return a;
    case kNeg: return -a;
    case kRcp: return a == 0.0f ? std::numeric_limits<float>::infinity() : 1.0f / a;
    case kFrc: return a - std::floor(a);
    case kExp: return std::exp2(a);
    case kLog: return std::log2(std::fabs(a));
    case kRsq: return 1.0f / std::sqrt(std::fabs(a));
    case kSin: return std::sin(a);
    case kCos: return std::cos(a);
    case kSqrt: return std::sqrt(a);
    case kFloor: return std::floor(a);
    case kCeil: return std::ceil(a);
    case kMin: return std::min(a, b);
    case kMax: return std::max(a, b);
    case kLt: return a < b ? 1.0f : 0.0f;
    case kGe: return a >= b ? 1.0f : 0.0f;
    case kAdd: return a + b;
    case kMul: return a * b;
    case kDiv: return a / b;
    case kCmp: return a >= 0.0f ? b : d;
    case kMovc: return a != 0.0f ? b : d;
    default: return 0.0f;
    }
}

bool parse_code(const uint8_t* d, size_t n, Expression& e, std::string* err) {
    const size_t nw = n / 4;
    size_t p = 0;
    const uint32_t count = word(d, n, p++);
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 2 > nw) {
            if (err) *err = "FXLC truncated";
            return false;
        }
        FxlcInstruction ins;
        const uint32_t code = word(d, n, p), nargs = word(d, n, p + 1);
        p += 2;
        ins.op = (code >> 20) & 0x7FFu;
        ins.comps = code & 0xFFFFu;
        ins.scalar = (code & 0x80000000u) != 0;
        if (nargs > 8 || ins.comps > 4) {
            if (err) *err = "FXLC bad instruction";
            return false;
        }
        for (uint32_t a = 0; a <= nargs; ++a) {
            FxlcOperand o;
            if (p >= nw) {
                if (err) *err = "FXLC operand truncated";
                return false;
            }
            const uint32_t has_index = word(d, n, p);
            if (has_index) {
                if (p + 5 > nw) return false;
                o.indexed = true;
                o.index_table = word(d, n, p + 1);
                o.index_offset = word(d, n, p + 2);
                o.table = word(d, n, p + 3);
                o.offset = word(d, n, p + 4);
                p += 5;
            } else {
                if (p + 3 > nw) return false;
                o.table = word(d, n, p + 1);
                o.offset = word(d, n, p + 2);
                p += 3;
            }
            if (a < nargs) ins.inputs.push_back(o);
            else ins.output = o;
        }
        // Register-file bounds: offsets are untrusted u32 component offsets. Every register an
        // operand can touch must lie inside kMaxFxlcRegisters (64-bit arithmetic, no wrap).
        const uint64_t ncomp = std::max<uint32_t>(1, ins.comps);
        auto reg_end = [&](const FxlcOperand& o, uint64_t comps) -> uint64_t {
            return (uint64_t(o.offset) + comps - 1) / 4 + 1;
        };
        auto bad = [&](const FxlcOperand& o, uint64_t comps) {
            if (o.indexed) {
                if (o.index_table != kTabLiteral && o.index_table != kTabInput && o.index_table != kTabTemp &&
                    o.index_table != kTabOutput)
                    return true;
                if (uint64_t(o.index_offset) / 4 >= kMaxFxlcRegisters) return true;
            }
            if (o.table == kTabLiteral) return uint64_t(o.offset) + comps > (uint64_t(kMaxFxlcRegisters) * 4);
            if (o.table == kTabInput || o.table == kTabTemp || o.table == kTabOutput)
                return reg_end(o, comps) > kMaxFxlcRegisters;
            return true;  // unknown table
        };
        const uint64_t nout = ins.op == kDot ? 1u : ncomp;
        if (bad(ins.output, nout) || ins.output.table == kTabLiteral || ins.output.table == kTabInput) {
            if (err) *err = "FXLC output operand out of range";
            return false;
        }
        for (size_t a = 0; a < ins.inputs.size(); ++a) {
            const uint64_t k = (ins.scalar && a == 0) ? 1u : ncomp;
            if (bad(ins.inputs[a], k)) {
                if (err) *err = "FXLC input operand out of range";
                return false;
            }
        }
        const uint32_t last = static_cast<uint32_t>(reg_end(ins.output, nout));
        if (ins.output.table == kTabOutput) e.output_registers = std::max(e.output_registers, last);
        if (ins.output.table == kTabTemp) e.temp_registers = std::max(e.temp_registers, last);
        for (size_t a = 0; a < ins.inputs.size(); ++a) {
            const auto& in = ins.inputs[a];
            const uint64_t k = (ins.scalar && a == 0) ? 1u : ncomp;
            if (in.table == kTabInput)
                e.input_registers = std::max(e.input_registers, static_cast<uint32_t>(reg_end(in, k)));
            if (in.table == kTabTemp)
                e.temp_registers = std::max(e.temp_registers, static_cast<uint32_t>(reg_end(in, k)));
        }
        e.code.push_back(std::move(ins));
    }
    return true;
}

}  // namespace

static bool parse_expression_impl(const uint8_t* d, size_t n, Expression& e, std::string* err) {
    e = Expression{};
    const size_t nw = n / 4;
    if (nw < 1 || (word(d, n, 0) >> 16) != 0x4658u) {
        if (err) *err = "not an FXLC stream";
        return false;
    }
    e.version = word(d, n, 0);
    // CTAB through the shared reader (it accepts FXLC streams).
    try {
        fx::ShaderInfo si = fx::inspectShader(d, n);
        for (const auto& c : si.constants) {
            if (c.registerSet != 2) continue;
            if (uint32_t(c.registerIndex) + c.registerCount > kMaxFxlcRegisters) {
                if (err) *err = "FXLC input constant out of range";
                return false;
            }
            e.inputs.push_back({c.name, c.registerIndex, c.registerCount, c.cls, c.rows, c.columns, c.elements});
            e.input_registers = std::max<uint32_t>(e.input_registers, uint32_t(c.registerIndex) + c.registerCount);
        }
    } catch (const std::exception& ex) {
        if (err) *err = ex.what();
        return false;
    }
    size_t p = 1;
    bool have_code = false;
    while (p < nw) {
        const uint32_t t = word(d, n, p);
        if (t == 0x0000FFFFu) break;
        if ((t & 0xFFFFu) != 0xFFFEu) {
            if (err) *err = "unexpected token in FXLC stream";
            return false;
        }
        const size_t len = (t >> 16) & 0x7FFFu;
        if (p + 1 + len > nw) {
            if (err) *err = "FXLC comment past end";
            return false;
        }
        const uint8_t* body = d + 4 * (p + 1);
        const size_t blen = 4 * len;
        const uint32_t fourcc = len ? word(body, blen, 0) : 0;
        if (fourcc == 0x54494C43u /* CLIT */ && len >= 2) {
            const uint32_t cnt = word(body, blen, 1);
            if (8 + size_t(cnt) * 8 > blen) {
                if (err) *err = "CLIT truncated";
                return false;
            }
            for (uint32_t i = 0; i < cnt; ++i) {
                double v;
                std::memcpy(&v, body + 8 + 8 * size_t(i), 8);
                e.literals.push_back(v);
            }
        } else if (fourcc == 0x434C5846u /* FXLC */) {
            if (!parse_code(body + 4, blen - 4, e, err)) return false;
            have_code = true;
        }
        p += 1 + len;
    }
    if (!have_code) {
        if (err) *err = "no FXLC block";
        return false;
    }
    return true;
}

bool parse_expression(const uint8_t* d, size_t n, Expression& e, std::string* err) {
    try {
        if (parse_expression_impl(d, n, e, err)) return true;
    } catch (const std::exception& ex) {
        if (err) *err = std::string("FXLC: ") + ex.what();
    }
    e = Expression{};
    return false;
}

bool expression_supported(const Expression& e, std::string* first) {
    for (const auto& i : e.code)
        if (!supported_op(i.op)) {
            if (first) *first = "op 0x" + std::to_string(i.op);
            return false;
        }
    return true;
}

uint32_t pack_registers(std::span<const float> v, uint16_t cls, uint16_t rows, uint16_t columns,
                        uint16_t elements, uint32_t max_regs, std::span<float4> out) {
    const uint32_t R = std::max<uint16_t>(1, rows), C = std::max<uint16_t>(1, columns);
    const uint32_t E = std::max<uint16_t>(1, elements);
    const uint32_t per_elem_floats = R * C;
    uint32_t written = 0;
    auto at = [&](size_t i) { return i < v.size() ? v[i] : 0.0f; };
    for (uint32_t e = 0; e < E; ++e) {
        const size_t base = size_t(e) * per_elem_floats;
        const uint32_t nregs = cls == 3 ? C : (cls == 2 ? R : 1);
        for (uint32_t r = 0; r < nregs; ++r) {
            if (written >= max_regs || written >= out.size()) return written;
            float t[4] = {0, 0, 0, 0};
            if (cls == 3) {          // column r: (m[0][r], m[1][r], ...)
                for (uint32_t k = 0; k < R && k < 4; ++k) t[k] = at(base + size_t(k) * C + r);
            } else if (cls == 2) {   // row r
                for (uint32_t k = 0; k < C && k < 4; ++k) t[k] = at(base + size_t(r) * C + k);
            } else {                 // scalar / vector element
                for (uint32_t k = 0; k < per_elem_floats && k < 4; ++k) t[k] = at(base + k);
            }
            out[written++] = float4{t[0], t[1], t[2], t[3]};
        }
    }
    return written;
}

// Runs the code on caller-provided register files (no allocation). in / tmp / res hold at
// least input_registers / temp_registers / output_registers float4s.
bool run_expression(const Expression& e, std::span<float> in, std::span<float> tmp, std::span<float> res,
                    std::string* err) {
    auto table = [&](uint32_t t) -> std::span<float>* {
        switch (t) {
        case kTabInput: return &in;
        case kTabTemp: return &tmp;
        case kTabOutput: return &res;
        default: return nullptr;
        }
    };
    auto read = [&](const FxlcOperand& o, uint32_t comp) -> float {
        size_t idx = o.offset + comp;
        if (o.indexed) {
            float iv = 0.0f;
            if (o.index_table == kTabLiteral) {
                if (o.index_offset < e.literals.size()) iv = static_cast<float>(e.literals[o.index_offset]);
            } else if (auto* it = table(o.index_table); it && o.index_offset < it->size()) {
                iv = (*it)[o.index_offset];
            }
            // NaN / huge / negative indices read 0 instead of converting out of range (UB).
            if (!(iv >= 0.0f) || iv >= static_cast<float>(kMaxFxlcRegisters)) return 0.0f;
            idx = (size_t(o.offset / 4) + static_cast<size_t>(iv)) * 4 + (o.offset % 4) + comp;
        }
        if (o.table == kTabLiteral) return idx < e.literals.size() ? static_cast<float>(e.literals[idx]) : 0.0f;
        auto* t = table(o.table);
        return (t && idx < t->size()) ? (*t)[idx] : 0.0f;
    };
    for (const auto& ins : e.code) {
        if (!supported_op(ins.op)) {
            if (err) *err = "unsupported FXLC op " + std::to_string(ins.op);
            return false;
        }
        const uint32_t n = std::max<uint32_t>(1, ins.comps);
        auto arg = [&](size_t a, uint32_t c) {
            if (a >= ins.inputs.size()) return 0.0f;
            const bool bcast = ins.scalar && a == 0;
            return read(ins.inputs[a], bcast ? 0 : c);
        };
        auto* dst = table(ins.output.table);
        if (!dst) continue;
        if (ins.op == kDot) {
            float s = 0.0f;
            for (uint32_t c = 0; c < n; ++c) s += arg(0, c) * arg(1, c);
            if (ins.output.offset < dst->size()) (*dst)[ins.output.offset] = s;
            continue;
        }
        float r[4] = {0, 0, 0, 0};
        for (uint32_t c = 0; c < n && c < 4; ++c) r[c] = apply_op(ins.op, arg(0, c), arg(1, c), arg(2, c));
        for (uint32_t c = 0; c < n && c < 4; ++c)
            if (ins.output.offset + c < dst->size()) (*dst)[ins.output.offset + c] = r[c];
    }
    return true;
}

bool PreparedExpression::prepare(const Expression& e, size_t in_floats, size_t tmp_floats, size_t res_floats) {
    code_.clear();
    lits_.clear();
    ok_ = false;
    nin_ = in_floats;
    ntmp_ = tmp_floats;
    nres_ = res_floats;
    if (nin_ > kFile || ntmp_ > kFile || nres_ > kFile) return false;
    for (double v : e.literals) lits_.push_back(static_cast<float>(v));
    const int32_t zero = -static_cast<int32_t>(lits_.size()) - 1;  // a 0 literal past the real ones
    lits_.push_back(0.0f);
    // Operand component -> index (>= 0: file [in | tmp | res], < 0: literal -(i + 1)), as run_expression reads it.
    auto src = [&](const FxlcOperand& o, uint32_t comp, int32_t& idx) {
        if (o.indexed) return false;
        const size_t i = size_t(o.offset) + comp;
        switch (o.table) {
        case kTabLiteral: idx = i < e.literals.size() ? -static_cast<int32_t>(i) - 1 : zero; return true;
        case kTabInput: idx = i < nin_ ? static_cast<int32_t>(i) : zero; return true;
        case kTabTemp: idx = i < ntmp_ ? static_cast<int32_t>(kFile + i) : zero; return true;
        case kTabOutput: idx = i < nres_ ? static_cast<int32_t>(2 * kFile + i) : zero; return true;
        default: idx = zero; return true;
        }
    };
    for (const FxlcInstruction& ins : e.code) {
        if (!supported_op(ins.op)) return false;
        Ins p;
        p.op = ins.op;
        p.n = std::max<uint32_t>(1, ins.comps);
        size_t dst_size = 0, base = 0;
        switch (ins.output.table) {
        case kTabInput: dst_size = nin_; base = 0; break;
        case kTabTemp: dst_size = ntmp_; base = kFile; break;
        case kTabOutput: dst_size = nres_; base = 2 * kFile; break;
        default: p.skip = true; break;
        }
        if (ins.output.indexed) return false;
        for (uint32_t a = 0; a < 3; ++a)
            for (uint32_t c = 0; c < 4; ++c) {
                p.src[a][c] = zero;
                if (a >= ins.inputs.size()) continue;
                const bool bcast = ins.scalar && a == 0;
                if (!src(ins.inputs[a], bcast ? 0 : c, p.src[a][c])) return false;
            }
        for (uint32_t c = 0; c < 4; ++c) {
            const size_t o = size_t(ins.output.offset) + c;
            p.dst[c] = !p.skip && o < dst_size ? static_cast<int32_t>(base + o) : -1;
        }
        code_.push_back(p);
    }
    ok_ = true;
    return true;
}

void PreparedExpression::run(std::span<const float> in, std::span<float> res) const {
    float file[3 * kFile];
    std::copy_n(in.data(), std::min(in.size(), nin_), file);
    std::fill(file + kFile, file + kFile + ntmp_, 0.0f);
    std::fill(file + 2 * kFile, file + 2 * kFile + nres_, 0.0f);
    const float* lits = lits_.data();
    auto rd = [&](int32_t i) { return i >= 0 ? file[i] : lits[-i - 1]; };
    for (const Ins& p : code_) {
        if (p.skip) continue;
        if (p.op == kDot) {
            float s = 0.0f;
            for (uint32_t c = 0; c < p.n; ++c) s += rd(p.src[0][c < 4 ? c : 3]) * rd(p.src[1][c < 4 ? c : 3]);
            if (p.dst[0] >= 0) file[p.dst[0]] = s;
            continue;
        }
        float r[4] = {0, 0, 0, 0};
        for (uint32_t c = 0; c < p.n && c < 4; ++c) r[c] = apply_op(p.op, rd(p.src[0][c]), rd(p.src[1][c]), rd(p.src[2][c]));
        for (uint32_t c = 0; c < p.n && c < 4; ++c)
            if (p.dst[c] >= 0) file[p.dst[c]] = r[c];
    }
    std::copy_n(file + 2 * kFile, std::min(res.size(), nres_), res.data());
}

bool evaluate(const Expression& e, const fx::Effect& effect, const ParamLookup& lookup, std::span<float4> out,
              std::string* err) {
    std::vector<float> in(size_t(std::max<uint32_t>(e.input_registers, 1)) * 4, 0.0f);
    std::vector<float> tmp(size_t(std::max<uint32_t>(e.temp_registers, 1)) * 4, 0.0f);
    std::vector<float> res(size_t(std::max<uint32_t>(e.output_registers, 1)) * 4, 0.0f);
    for (const auto& i : e.inputs) {
        const fx::Parameter* p = effect.findParameter(i.name);
        if (!p) continue;
        std::vector<float> v = param_value(*p, lookup);
        std::vector<float4> regs(i.count);
        const uint32_t n = pack_registers(v, i.cls, i.rows, i.columns, i.elements, i.count, regs);
        for (uint32_t r = 0; r < n; ++r) {
            const size_t b = (size_t(i.reg) + r) * 4;
            if (b + 3 < in.size()) std::memcpy(&in[b], &regs[r], 16);
        }
    }
    if (!run_expression(e, in, tmp, res, err)) return false;
    for (size_t r = 0; r < out.size(); ++r) {
        float4 v{};
        if (r * 4 + 3 < res.size()) std::memcpy(static_cast<void*>(&v), &res[r * 4], 16);  // float4 is trivially copyable
        out[r] = v;
    }
    return true;
}

namespace {
std::string upper_ascii(std::string_view s) {
    std::string o(s);
    for (char& ch : o)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return o;
}
}  // namespace

bool CompiledExpression::compile(const Expression& e, const fx::Effect& effect, const ParamLookup& material_lookup,
                                 std::string* err) {
    expr_ = e;
    bindings_.clear();
    baked_.assign(size_t(std::max<uint32_t>(e.input_registers, 1)) * 4, 0.0f);
    written_.assign(size_t(std::max<uint32_t>(e.output_registers, 1)) * 4, 0);
    std::string unsupported;
    if (!expression_supported(e, &unsupported)) {
        if (err) *err = "unsupported FXLC " + unsupported;
        return false;
    }
    for (const auto& ins : e.code) {
        if (ins.output.table != kTabOutput) continue;
        const uint32_t n = ins.op == kDot ? 1u : std::max<uint32_t>(1, ins.comps);
        for (uint32_t c = 0; c < n; ++c)
            if (ins.output.offset + c < written_.size()) written_[ins.output.offset + c] = 1;
    }
    for (const auto& i : e.inputs) {
        const fx::Parameter* p = effect.findParameter(i.name);
        if (!p) continue;
        std::vector<float> v = param_value(*p, material_lookup);
        std::vector<float4> regs(i.count);
        const uint32_t n = pack_registers(v, i.cls, i.rows, i.columns, i.elements, i.count, regs);
        for (uint32_t r = 0; r < n; ++r) {
            const size_t b = (size_t(i.reg) + r) * 4;
            if (b + 3 < baked_.size()) std::memcpy(&baked_[b], &regs[r], 16);
        }
        const std::string sem = upper_ascii(p->type.semantic);
        Binding bd;
        bool dyn = true;
        if (sem == "LIGHT_SCALE") bd.source = Source::LightScale;
        else if (sem == "TIME") bd.source = Source::Time;
        else if (sem == "VIEWINVERSE") bd.source = Source::ViewInv;
        else if (sem == "FOW_TEX_U") bd.source = Source::FowTexU;
        else if (sem == "FOW_TEX_V") bd.source = Source::FowTexV;
        else if (sem == "CLOUD_TEX_U") bd.source = Source::CloudTexU;
        else if (sem == "CLOUD_TEX_V") bd.source = Source::CloudTexV;
        else if (upper_ascii(p->type.name) == "COLORIZATION") bd.source = Source::Colorization;
        else dyn = false;
        if (!dyn) continue;
        bd.reg = i.reg;
        bd.count = i.count;
        bd.cls = i.cls;
        bd.rows = i.rows;
        bd.columns = i.columns;
        bd.elements = i.elements;
        std::copy_n(v.begin(), std::min<size_t>(16, v.size()), bd.fallback.begin());
        bindings_.push_back(bd);
    }
    return true;
}

bool CompiledExpression::depends_on(Source s) const {
    for (const auto& b : bindings_)
        if (b.source == s) return true;
    return false;
}

void CompiledExpression::run(const ExprInputs& in, std::span<float4> out) const {
    std::array<float, kMaxFxlcRegisters * 4> ir;
    std::array<float, kMaxFxlcRegisters * 4> tr;
    std::array<float, kMaxFxlcRegisters * 4> rr;
    const size_t ni = std::min(baked_.size(), ir.size());
    std::copy_n(baked_.begin(), ni, ir.begin());
    const size_t nt = size_t(std::max<uint32_t>(expr_.temp_registers, 1)) * 4;
    std::fill(tr.begin(), tr.begin() + static_cast<std::ptrdiff_t>(nt), 0.0f);
    const size_t nr = size_t(std::max<uint32_t>(expr_.output_registers, 1)) * 4;
    std::fill(rr.begin(), rr.begin() + static_cast<std::ptrdiff_t>(nr), 0.0f);
    for (const auto& b : bindings_) {
        std::array<float, 16> v = b.fallback;
        switch (b.source) {
        case Source::LightScale: v = {in.light_scale.x, in.light_scale.y, in.light_scale.z, in.light_scale.w}; break;
        case Source::Time: v[0] = in.time; break;
        case Source::Colorization:
            if (in.colorization) v = {in.colorization->x, in.colorization->y, in.colorization->z, in.colorization->w};
            break;
        case Source::ViewInv:
            if (in.view_inv) std::copy_n(in.view_inv, 16, v.begin());
            break;
        case Source::FowTexU:
            if (in.fow_tex_u) v = {in.fow_tex_u->x, in.fow_tex_u->y, in.fow_tex_u->z, in.fow_tex_u->w};
            break;
        case Source::FowTexV:
            if (in.fow_tex_v) v = {in.fow_tex_v->x, in.fow_tex_v->y, in.fow_tex_v->z, in.fow_tex_v->w};
            break;
        case Source::CloudTexU:
            if (in.cloud_tex_u) v = {in.cloud_tex_u->x, in.cloud_tex_u->y, in.cloud_tex_u->z, in.cloud_tex_u->w};
            break;
        case Source::CloudTexV:
            if (in.cloud_tex_v) v = {in.cloud_tex_v->x, in.cloud_tex_v->y, in.cloud_tex_v->z, in.cloud_tex_v->w};
            break;
        }
        std::array<float4, 4> regs{};
        const uint32_t n = pack_registers(v, b.cls, b.rows, b.columns, b.elements, std::min<uint32_t>(b.count, 4u), regs);
        for (uint32_t r = 0; r < n; ++r) {
            const size_t o = (size_t(b.reg) + r) * 4;
            if (o + 3 < ni) std::memcpy(&ir[o], &regs[r], 16);
        }
    }
    if (!run_expression(expr_, std::span<float>(ir.data(), ni), std::span<float>(tr.data(), nt),
                        std::span<float>(rr.data(), nr)))
        return;
    for (size_t r = 0; r < out.size() && r * 4 < nr; ++r) {
        float* dst = &out[r].x;
        for (size_t c = 0; c < 4; ++c)
            if (written_[r * 4 + c]) dst[c] = rr[r * 4 + c];
    }
}

}  // namespace alamo::materials

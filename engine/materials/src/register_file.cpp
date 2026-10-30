// Register-file plans. See register_file.hpp.
#include "alamo/materials/register_file.hpp"

#include <algorithm>
#include <cstring>

#include "fx2_reader.hpp"

namespace alamo::materials {

std::span<const uint8_t> shader_bytecode(const fx::Effect& effect, const fx::State& s) {
    if (s.kind == fx::State::Kind::Shader && !s.blob.empty()) return {s.blob.data(), s.blob.size()};
    uint32_t id = 0;
    bool have = false;
    if (s.kind == fx::State::Kind::ParameterRef) {
        const fx::Parameter* p = effect.findParameter(s.refName);
        if (p && !p->value.objects.empty()) {
            id = p->value.objects[0];
            have = true;
        }
    } else if (!s.value.objects.empty()) {
        id = s.value.objects[0];
        have = true;
    }
    if (!have) return {};
    auto it = effect.objects.find(id);
    if (it == effect.objects.end() || it->second.data.size() < 8) return {};
    return {it->second.data.data(), it->second.data.size()};
}

namespace {
// Finds the PRES comment payload (an FXLC stream) in shader bytecode.
std::span<const uint8_t> find_pres(std::span<const uint8_t> b) {
    const size_t n = b.size() / 4;
    auto w = [&](size_t i) {
        uint32_t v;
        std::memcpy(&v, b.data() + 4 * i, 4);
        return v;
    };
    for (size_t p = 1; p < n;) {
        const uint32_t tok = w(p);
        if ((tok & 0xFFFFu) == 0xFFFEu) {
            const size_t len = (tok >> 16) & 0x7FFFu;
            if (p + 1 + len > n) break;
            if (len >= 2 && w(p + 1) == 0x53455250u /* PRES */)
                return b.subspan(4 * (p + 2), 4 * (len - 1));
            p += 1 + len;
            continue;
        }
        if (tok == 0x0000FFFFu) break;
        ++p;  // instruction tokens: comments only appear before the first instruction in practice
    }
    return {};
}
}  // namespace

bool build_register_plan(const fx::Effect& effect, std::span<const uint8_t> bc, RegisterFilePlan& plan,
                         std::string* err) {
    plan = RegisterFilePlan{};
    fx::ShaderInfo si;
    try {
        si = fx::inspectShader(bc.data(), bc.size());
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    plan.pixel_shader = si.isPixelShader;
    plan.major = si.major;
    plan.minor = si.minor;
    for (const auto& c : si.constants) {
        if (c.registerSet == 3) {
            plan.samplers.push_back({c.name, c.registerIndex, std::max<uint16_t>(1, c.registerCount)});
            continue;
        }
        if (c.registerSet != 2) {
            ++plan.bool_int_constants;
            RegisterBinding ib;
            ib.name = c.name;
            ib.reg = c.registerIndex;
            ib.count = c.registerCount;
            ib.cls = c.cls;
            ib.rows = c.rows;
            ib.columns = c.columns;
            ib.elements = c.elements;
            for (size_t i = 0; i < effect.parameters.size(); ++i)
                if (effect.parameters[i].type.name == c.name) ib.parameter = static_cast<int>(i);
            (c.registerSet == 1 ? plan.ints : plan.bools).push_back(std::move(ib));
            continue;
        }
        RegisterBinding rb;
        rb.name = c.name;
        rb.reg = c.registerIndex;
        rb.count = c.registerCount;
        rb.cls = c.cls;
        rb.rows = c.rows;
        rb.columns = c.columns;
        rb.elements = c.elements;
        for (size_t i = 0; i < effect.parameters.size(); ++i)
            if (effect.parameters[i].type.name == c.name) {
                rb.parameter = static_cast<int>(i);
                break;
            }
        if (rb.parameter < 0) plan.unbound.push_back(c.name);
        plan.used_registers = std::max<uint32_t>(plan.used_registers, uint32_t(rb.reg) + rb.count);
        plan.floats.push_back(std::move(rb));
    }
    if (si.hasPreshader) {
        std::span<const uint8_t> pres = find_pres(bc);
        Expression ex;
        std::string perr;
        if (!pres.empty() && parse_expression(pres.data(), pres.size(), ex, &perr)) {
            plan.used_registers = std::max(plan.used_registers, ex.output_registers);
            plan.preshader = std::move(ex);
        } else if (err) {
            *err = "preshader: " + perr;
        }
    }
    return true;
}

void fill_register_binding(const fx::Effect& effect, const RegisterBinding& b, const ParamLookup& lookup,
                           std::span<float4> regs) {
    if (b.parameter < 0 || b.reg >= regs.size()) return;
    const fx::Parameter& p = effect.parameters[static_cast<size_t>(b.parameter)];
    const size_t n = param_float_count(p);
    constexpr size_t kStack = 1024;  // a full 256-register file; larger values take the heap
    if (n <= kStack) {
        float v[kStack];
        param_value(p, lookup, std::span<float>(v, n));
        pack_registers(std::span<const float>(v, n), b.cls, b.rows, b.columns, b.elements, b.count, regs.subspan(b.reg));
        return;
    }
    std::vector<float> v(n);
    param_value(p, lookup, v);
    pack_registers(v, b.cls, b.rows, b.columns, b.elements, b.count, regs.subspan(b.reg));
}

void fill_register_file(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                        std::span<float4> regs) {
    for (const auto& b : plan.floats) fill_register_binding(effect, b, lookup, regs);
    fill_register_preshader(effect, plan, lookup, regs);
}

void fill_register_preshader(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                             std::span<float4> regs) {
    if (plan.preshader) {
        std::vector<float4> out(plan.preshader->output_registers);
        if (evaluate(*plan.preshader, effect, lookup, out)) {
            // Outputs are written to c# directly; only registers the code writes are stored.
            std::vector<uint8_t> written(out.size() * 4, 0);
            for (const auto& ins : plan.preshader->code) {
                if (ins.output.table != 4) continue;
                const uint32_t n = ins.op == 0x500 ? 1u : std::max<uint32_t>(1, ins.comps);
                for (uint32_t c = 0; c < n; ++c)
                    if (ins.output.offset + c < written.size()) written[ins.output.offset + c] = 1;
            }
            for (size_t r = 0; r < out.size() && r < regs.size(); ++r) {
                float* dst = &regs[r].x;
                const float* src = &out[r].x;
                for (int c = 0; c < 4; ++c)
                    if (written[r * 4 + static_cast<size_t>(c)]) dst[c] = src[c];
            }
        }
    }
}

namespace {
// One preshader input packed into the input register file, as evaluate() does.
void pack_preshader_input(const FxlcInput& i, const fx::Parameter& p, const ParamLookup& lookup, std::span<float> in) {
    const size_t n = param_float_count(p);
    float4 regs[kMaxFxlcRegisters];
    const uint32_t count = std::min<uint32_t>(i.count, kMaxFxlcRegisters);
    uint32_t packed = 0;
    if (n <= 1024) {
        float v[1024];
        param_value(p, lookup, std::span<float>(v, n));
        packed = pack_registers(std::span<const float>(v, n), i.cls, i.rows, i.columns, i.elements, i.count,
                                std::span<float4>(regs, count));
    } else {
        std::vector<float> v(n);
        param_value(p, lookup, v);
        packed = pack_registers(v, i.cls, i.rows, i.columns, i.elements, i.count, std::span<float4>(regs, count));
    }
    for (uint32_t r = 0; r < packed; ++r) {
        const size_t b = (size_t(i.reg) + r) * 4;
        if (b + 3 < in.size()) std::memcpy(&in[b], &regs[r], 16);
    }
}
}  // namespace

bool bake_preshader(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                    const std::function<bool(const fx::Parameter&)>& per_draw, PreshaderBake& out) {
    out.usable = false;  // vectors cleared, capacity kept (re-baked per frame without allocating)
    out.dynamic.clear();
    out.params.clear();
    out.written.clear();
    if (!plan.preshader) return false;
    const Expression& e = *plan.preshader;
    if (e.input_registers > kMaxFxlcRegisters || e.temp_registers > kMaxFxlcRegisters || e.output_registers > kMaxFxlcRegisters)
        return false;
    out.inputs.assign(size_t(std::max<uint32_t>(e.input_registers, 1)) * 4, 0.0f);
    out.params.assign(e.inputs.size(), nullptr);
    for (size_t k = 0; k < e.inputs.size(); ++k) {
        const fx::Parameter* p = effect.findParameter(e.inputs[k].name);
        out.params[k] = p;
        if (p && per_draw(*p)) out.dynamic.push_back(static_cast<uint16_t>(k));
    }
    // A per-draw input sharing registers with another input would change the packing order: not split.
    for (uint16_t d : out.dynamic)
        for (size_t k = 0; k < e.inputs.size(); ++k) {
            if (k == d || !out.params[k]) continue;
            const FxlcInput& a = e.inputs[d];
            const FxlcInput& b = e.inputs[k];
            if (a.reg < b.reg + b.count && b.reg < a.reg + a.count) return false;
        }
    for (size_t k = 0; k < e.inputs.size(); ++k)  // static inputs, in input order
        if (out.params[k] && std::find(out.dynamic.begin(), out.dynamic.end(), static_cast<uint16_t>(k)) == out.dynamic.end())
            pack_preshader_input(e.inputs[k], *out.params[k], lookup, out.inputs);
    out.written.assign(size_t(e.output_registers) * 4, 0);
    for (const auto& ins : e.code) {
        if (ins.output.table != 4) continue;
        const uint32_t n = ins.op == 0x500 ? 1u : std::max<uint32_t>(1, ins.comps);
        for (uint32_t c = 0; c < n; ++c)
            if (ins.output.offset + c < out.written.size()) out.written[ins.output.offset + c] = 1;
    }
    out.prepared.prepare(e, out.inputs.size(), size_t(std::max<uint32_t>(e.temp_registers, 1)) * 4,
                         size_t(std::max<uint32_t>(e.output_registers, 1)) * 4);
    out.usable = true;
    return true;
}

void run_preshader(const RegisterFilePlan& plan, const PreshaderBake& bake, const ParamLookup& lookup, std::span<float4> regs) {
    if (!plan.preshader || !bake.usable) return;
    const Expression& e = *plan.preshader;
    float in[kMaxFxlcRegisters * 4], tmp[kMaxFxlcRegisters * 4], res[kMaxFxlcRegisters * 4];
    const size_t nin = bake.inputs.size();
    const size_t ntmp = size_t(std::max<uint32_t>(e.temp_registers, 1)) * 4;
    const size_t nres = size_t(std::max<uint32_t>(e.output_registers, 1)) * 4;
    std::memcpy(in, bake.inputs.data(), nin * sizeof(float));
    for (uint16_t d : bake.dynamic) pack_preshader_input(e.inputs[d], *bake.params[d], lookup, std::span<float>(in, nin));
    if (bake.prepared.ok()) {
        bake.prepared.run(std::span<const float>(in, nin), std::span<float>(res, nres));
    } else {
        std::fill(tmp, tmp + ntmp, 0.0f);
        std::fill(res, res + nres, 0.0f);
        if (!run_expression(e, std::span<float>(in, nin), std::span<float>(tmp, ntmp), std::span<float>(res, nres))) return;
    }
    for (size_t r = 0; r < e.output_registers && r < regs.size(); ++r) {
        float* dst = &regs[r].x;
        for (size_t c = 0; c < 4; ++c)
            if (bake.written[r * 4 + c]) dst[c] = res[r * 4 + c];
    }
}

}  // namespace alamo::materials

// FF pass extraction and per-draw evaluation. See fixed_function.hpp.
#include "alamo/materials/fixed_function.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "fx2_reader.hpp"

namespace alamo::materials {
namespace {

FfValue value_of(const fx::Effect& effect, const fx::State& s) {
    FfValue v;
    for (size_t i = 0; i < s.value.words.size() && i < 16; ++i) {
        v.constant[i] = s.type.type == fx::ParamType::Float ? std::bit_cast<float>(s.value.words[i])
                                                            : static_cast<float>(s.value.words[i]);
    }
    switch (s.kind) {
    case fx::State::Kind::Expression: {
        Expression e;
        if (parse_expression(s.blob.data(), s.blob.size(), e)) {
            v.kind = FfValue::Kind::Expression;
            v.expr = std::move(e);
        }
        break;
    }
    case fx::State::Kind::ParameterRef:
        v.kind = FfValue::Kind::ParameterRef;
        v.param = s.refName;
        break;
    default:
        v.kind = FfValue::Kind::Constant;
        break;
    }
    (void)effect;
    return v;
}

// Evaluates to 4 registers (16 floats).
std::array<float, 16> eval(const FfValue& v, const fx::Effect& effect, const ParamLookup& lookup,
                           const std::array<float, 16>& fallback) {
    switch (v.kind) {
    case FfValue::Kind::Constant: return v.constant;
    case FfValue::Kind::Expression: {
        std::array<float4, 4> regs{};
        for (int r = 0; r < 4; ++r)
            regs[static_cast<size_t>(r)] = float4{v.constant[size_t(r) * 4], v.constant[size_t(r) * 4 + 1],
                                                  v.constant[size_t(r) * 4 + 2], v.constant[size_t(r) * 4 + 3]};
        std::vector<float4> out(std::max<uint32_t>(4, v.expr->output_registers));
        for (size_t r = 0; r < 4; ++r) out[r] = regs[r];
        std::vector<float4> res(out.size());
        if (!evaluate(*v.expr, effect, lookup, res)) return fallback;
        // keep the initial value in components the expression does not write
        for (const auto& ins : v.expr->code) {
            if (ins.output.table != 4) continue;
            const uint32_t n = ins.op == 0x500 ? 1u : std::max<uint32_t>(1, ins.comps);
            for (uint32_t c = 0; c < n; ++c) {
                const uint32_t o = ins.output.offset + c;
                if (o / 4 < out.size()) (&out[o / 4].x)[o % 4] = (&res[o / 4].x)[o % 4];
            }
        }
        std::array<float, 16> f{};
        for (size_t r = 0; r < 4; ++r) {
            f[r * 4] = out[r].x;
            f[r * 4 + 1] = out[r].y;
            f[r * 4 + 2] = out[r].z;
            f[r * 4 + 3] = out[r].w;
        }
        return f;
    }
    case FfValue::Kind::ParameterRef: {
        if (const fx::Parameter* p = effect.findParameter(v.param)) {
            std::vector<float> pv = param_value(*p, lookup);
            std::array<float, 16> f{};
            std::copy_n(pv.begin(), std::min<size_t>(16, pv.size()), f.begin());
            return f;
        }
        return fallback;
    }
    default: return fallback;
    }
}

float4 to4(const std::array<float, 16>& a) { return float4{a[0], a[1], a[2], a[3]}; }

float4 unpack_color(uint32_t argb) {
    return float4{static_cast<float>((argb >> 16) & 0xFF) / 255.0f, static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(argb & 0xFF) / 255.0f, static_cast<float>((argb >> 24) & 0xFF) / 255.0f};
}
float q8(float x) { return std::round(std::clamp(x, 0.0f, 1.0f) * 255.0f) / 255.0f; }

}  // namespace

FfPassState extract_ff_state(const fx::Effect& effect, const fx::Pass& pass, const DeviceState& st) {
    FfPassState ff;
    for (int s = 0; s < kMaxTextureStages; ++s) {
        if (st.get(s, TSS::ColorOp) == d3d::kDisable) break;
        if (s >= kFfStages) {
            ff.stages_truncated = true;
            if (ff.unsupported.empty()) ff.unsupported = "more than 3 texture stages";
            break;
        }
        FfStage& g = ff.stage[static_cast<size_t>(s)];
        g.color_op = st.get(s, TSS::ColorOp);
        g.color_arg1 = st.get(s, TSS::ColorArg1);
        g.color_arg2 = st.get(s, TSS::ColorArg2);
        g.alpha_op = st.get(s, TSS::AlphaOp);
        g.alpha_arg1 = st.get(s, TSS::AlphaArg1);
        g.alpha_arg2 = st.get(s, TSS::AlphaArg2);
        g.texcoord_index = st.get(s, TSS::TexCoordIndex);
        g.transform_flags = st.get(s, TSS::TextureTransformFlags);
        ff.stage_texture[static_cast<size_t>(s)] = st.stage[static_cast<size_t>(s)].texture_param;
        if (ff.unsupported.empty() && (g.color_op == d3d::kBumpEnvMap || g.color_op == d3d::kBumpEnvMapLuminance))
            ff.unsupported = "BUMPENVMAP stage";
        ff.active_stages = s + 1;
    }
    ff.lighting = st.get(RS::Lighting) != 0;
    ff.specular = st.get(RS::SpecularEnable) != 0;
    ff.color_vertex = st.get(RS::ColorVertex) != 0;
    ff.sources = {st.get(RS::DiffuseMaterialSource), st.get(RS::SpecularMaterialSource),
                  st.get(RS::AmbientMaterialSource), st.get(RS::EmissiveMaterialSource)};
    if (st.slot(RS::TextureFactor).origin == StateOrigin::Expression ||
        st.slot(RS::TextureFactor).origin == StateOrigin::ParameterRef) {
        const int i = st.slot(RS::TextureFactor).source_state;
        if (st.refers_to_last_pass(st.slot(RS::TextureFactor)) && static_cast<size_t>(i) < pass.states.size())
            ff.texture_factor = value_of(effect, pass.states[static_cast<size_t>(i)]);
    } else {
        ff.texture_factor.kind = FfValue::Kind::Constant;
        const float4 c = unpack_color(st.get(RS::TextureFactor));
        ff.texture_factor.constant = {c.x, c.y, c.z, c.w};
    }
    for (const auto& s : pass.states) {
        switch (s.operation) {
        case op::kMaterialDiffuse: ff.mat_diffuse = value_of(effect, s); break;
        case op::kMaterialAmbient: ff.mat_ambient = value_of(effect, s); break;
        case op::kMaterialSpecular: ff.mat_specular = value_of(effect, s); break;
        case op::kMaterialEmissive: ff.mat_emissive = value_of(effect, s); break;
        case op::kMaterialPower: ff.mat_power = value_of(effect, s); break;
        case op::kTransformTexture0:
            if (s.index < static_cast<uint32_t>(kFfStages)) ff.tex_transform[s.index] = value_of(effect, s);
            break;
        default: break;
        }
    }
    return ff;
}

FfDrawData build_ff_draw_data(const FfPassState& ff, const fx::Effect& effect, const ParamLookup& lookup,
                              bool vertex_has_color) {
    FfDrawData d{};
    const std::array<float, 16> zero{};
    d.mat_diffuse = to4(eval(ff.mat_diffuse, effect, lookup, zero));
    d.mat_ambient = to4(eval(ff.mat_ambient, effect, lookup, zero));
    d.mat_specular = to4(eval(ff.mat_specular, effect, lookup, zero));
    d.mat_emissive = to4(eval(ff.mat_emissive, effect, lookup, zero));
    const float power = eval(ff.mat_power, effect, lookup, zero)[0];
    if (ff.texture_factor.kind == FfValue::Kind::Constant) {
        d.texture_factor = to4(ff.texture_factor.constant);
    } else {
        // D3DX writes a float4 into the DWORD state as D3DCOLOR: quantise to 8 bits per channel.
        const float4 t = to4(eval(ff.texture_factor, effect, lookup, {1, 1, 1, 1}));
        d.texture_factor = float4{q8(t.x), q8(t.y), q8(t.z), q8(t.w)};
    }
    d.params = float4{power, ff.lighting ? 1.0f : 0.0f, ff.specular ? 1.0f : 0.0f, ff.color_vertex ? 1.0f : 0.0f};
    d.sources = uint4{ff.sources[0], ff.sources[1], ff.sources[2], ff.sources[3]};
    for (int s = 0; s < kFfStages; ++s) {
        const auto& tt = ff.tex_transform[static_cast<size_t>(s)];
        float4* rows = d.tex_transform[s];
        if (tt.kind == FfValue::Kind::Expression) {
            // FXLC outputs are already register rows (out[i] = dot(expr_i, tc)).
            const auto m = eval(tt, effect, lookup, zero);
            for (int r = 0; r < 4; ++r)
                rows[r] = float4{m[size_t(r) * 4], m[size_t(r) * 4 + 1], m[size_t(r) * 4 + 2], m[size_t(r) * 4 + 3]};
        } else if (tt.kind == FfValue::Kind::Constant || tt.kind == FfValue::Kind::ParameterRef) {
            const auto m = eval(tt, effect, lookup, zero);  // D3DMATRIX, row-vector convention
            matrix_rows_from_d3dx(m.data(), rows, 4);
        } else {
            rows[0] = float4{1, 0, 0, 0};
            rows[1] = float4{0, 1, 0, 0};
            rows[2] = float4{0, 0, 1, 0};
            rows[3] = float4{0, 0, 0, 1};
        }
        d.stage[s] = ff.stage[static_cast<size_t>(s)];
    }
    const uint32_t none = pack_texture(kNoTexture, kSamplerWrapLinear);
    d.tex = uint4{none, none, none, none};
    d.misc = uint4{static_cast<uint32_t>(ff.active_stages), vertex_has_color ? 1u : 0u, 0u, 0u};
    return d;
}

bool FfDrawTemplate::build(const FfPassState& ff, const fx::Effect& effect, const ParamLookup& material_lookup,
                           bool vertex_has_color, std::string* err) {
    items_.clear();
    base_ = build_ff_draw_data(ff, effect, material_lookup, vertex_has_color);
    auto add = [&](const FfValue& v, Target t, uint8_t stage) {
        if (v.kind != FfValue::Kind::Expression || !v.expr) return true;
        Item it;
        it.target = t;
        it.stage = stage;
        for (size_t r = 0; r < 4; ++r)
            it.init[r] = float4{v.constant[r * 4], v.constant[r * 4 + 1], v.constant[r * 4 + 2], v.constant[r * 4 + 3]};
        if (!it.expr.compile(*v.expr, effect, material_lookup, err)) return false;
        if (it.expr.dynamic()) items_.push_back(std::move(it));
        return true;
    };
    bool ok = add(ff.mat_diffuse, Target::Diffuse, 0) && add(ff.mat_ambient, Target::Ambient, 0) &&
              add(ff.mat_specular, Target::Specular, 0) && add(ff.mat_emissive, Target::Emissive, 0) &&
              add(ff.mat_power, Target::Power, 0) && add(ff.texture_factor, Target::TextureFactor, 0);
    for (int s = 0; ok && s < kFfStages; ++s)
        ok = add(ff.tex_transform[static_cast<size_t>(s)], Target::TexTransform0, static_cast<uint8_t>(s));
    return ok;
}

void FfDrawTemplate::update(const ExprInputs& in, FfDrawData& out) const {
    out = base_;
    for (const Item& it : items_) {
        std::array<float4, 4> r = it.init;
        it.expr.run(in, r);
        switch (it.target) {
        case Target::Diffuse: out.mat_diffuse = r[0]; break;
        case Target::Ambient: out.mat_ambient = r[0]; break;
        case Target::Specular: out.mat_specular = r[0]; break;
        case Target::Emissive: out.mat_emissive = r[0]; break;
        case Target::Power: out.params.x = r[0].x; break;
        case Target::TextureFactor:  // float4 -> D3DCOLOR DWORD state: 8 bits per channel
            out.texture_factor = float4{q8(r[0].x), q8(r[0].y), q8(r[0].z), q8(r[0].w)};
            break;
        case Target::TexTransform0:
            for (int k = 0; k < 4; ++k) out.tex_transform[it.stage][k] = r[static_cast<size_t>(k)];
            break;
        }
    }
}

}  // namespace alamo::materials
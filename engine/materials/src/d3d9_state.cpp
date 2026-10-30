// D3D9 defaults and FXO pass application. See d3d9_state.hpp.
#include "alamo/materials/d3d9_state.hpp"

#include <algorithm>
#include <bit>

#include "fx2_reader.hpp"

namespace alamo::materials {
namespace {

uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }

void set(DeviceState& st, RS s, uint32_t v) { st.rs[static_cast<size_t>(s)].value = v; }

}  // namespace

DeviceState d3d9_defaults() {
    DeviceState st;
    set(st, RS::ZEnable, 1);
    set(st, RS::FillMode, d3d::kSolid);
    set(st, RS::ShadeMode, 2);  // GOURAUD
    set(st, RS::ZWriteEnable, 1);
    set(st, RS::LastPixel, 1);
    set(st, RS::SrcBlend, d3d::kOne);
    set(st, RS::DestBlend, d3d::kZero);
    set(st, RS::CullMode, d3d::kCullCCW);
    set(st, RS::ZFunc, d3d::kLessEqual);
    set(st, RS::AlphaFunc, d3d::kAlways);
    set(st, RS::FogEnd, fbits(1.0f));
    set(st, RS::FogDensity, fbits(1.0f));
    set(st, RS::StencilFail, d3d::kKeep);
    set(st, RS::StencilZFail, d3d::kKeep);
    set(st, RS::StencilPass, d3d::kKeep);
    set(st, RS::StencilFunc, d3d::kAlways);
    set(st, RS::StencilMask, 0xFFFFFFFFu);
    set(st, RS::StencilWriteMask, 0xFFFFFFFFu);
    set(st, RS::TextureFactor, 0xFFFFFFFFu);
    set(st, RS::Clipping, 1);
    set(st, RS::Lighting, 1);
    set(st, RS::ColorVertex, 1);
    set(st, RS::LocalViewer, 1);
    set(st, RS::DiffuseMaterialSource, d3d::kMcsColor1);
    set(st, RS::SpecularMaterialSource, d3d::kMcsColor2);
    set(st, RS::AmbientMaterialSource, d3d::kMcsMaterial);
    set(st, RS::EmissiveMaterialSource, d3d::kMcsMaterial);
    set(st, RS::PointSize, fbits(1.0f));
    set(st, RS::PointSizeMin, fbits(1.0f));
    set(st, RS::PointSizeMax, fbits(64.0f));
    set(st, RS::PointScaleA, fbits(1.0f));
    set(st, RS::MultisampleAntialias, 1);
    set(st, RS::MultisampleMask, 0xFFFFFFFFu);
    set(st, RS::ColorWriteEnable, 0xF);
    set(st, RS::BlendOp, d3d::kAdd);
    set(st, RS::PositionDegree, 3);  // D3DDEGREE_CUBIC
    set(st, RS::NormalDegree, 1);    // D3DDEGREE_LINEAR
    set(st, RS::MinTessellationLevel, fbits(1.0f));
    set(st, RS::MaxTessellationLevel, fbits(1.0f));
    set(st, RS::AdaptiveTessW, fbits(1.0f));
    set(st, RS::CcwStencilFail, d3d::kKeep);
    set(st, RS::CcwStencilZFail, d3d::kKeep);
    set(st, RS::CcwStencilPass, d3d::kKeep);
    set(st, RS::CcwStencilFunc, d3d::kAlways);
    set(st, RS::ColorWriteEnable1, 0xF);
    set(st, RS::ColorWriteEnable2, 0xF);
    set(st, RS::ColorWriteEnable3, 0xF);
    set(st, RS::BlendFactor, 0xFFFFFFFFu);
    set(st, RS::SrcBlendAlpha, d3d::kOne);
    set(st, RS::DestBlendAlpha, d3d::kZero);
    set(st, RS::BlendOpAlpha, d3d::kAdd);

    for (int s = 0; s < kMaxTextureStages; ++s) {
        auto& t = st.tss[static_cast<size_t>(s)];
        auto put = [&](TSS k, uint32_t v) { t[static_cast<size_t>(k)].value = v; };
        put(TSS::ColorOp, s == 0 ? d3d::kModulate : d3d::kDisable);
        put(TSS::AlphaOp, s == 0 ? d3d::kSelectArg1 : d3d::kDisable);
        put(TSS::ColorArg0, d3d::kTaCurrent);
        put(TSS::ColorArg1, d3d::kTaTexture);
        put(TSS::ColorArg2, d3d::kTaCurrent);
        put(TSS::AlphaArg0, d3d::kTaCurrent);
        put(TSS::AlphaArg1, d3d::kTaTexture);
        put(TSS::AlphaArg2, d3d::kTaCurrent);
        put(TSS::ResultArg, d3d::kTaCurrent);
        put(TSS::TexCoordIndex, static_cast<uint32_t>(s));
    }
    for (auto& smp : st.ss) {
        auto put = [&](SS k, uint32_t v) { smp[static_cast<size_t>(k)].value = v; };
        put(SS::AddressU, d3d::kWrap);
        put(SS::AddressV, d3d::kWrap);
        put(SS::AddressW, d3d::kWrap);
        put(SS::MagFilter, d3d::kFilterPoint);
        put(SS::MinFilter, d3d::kFilterPoint);
        put(SS::MipFilter, d3d::kFilterNone);
        put(SS::MaxAnisotropy, 1);
    }
    return st;
}

DeviceState engine_device_defaults() {
    DeviceState st = d3d9_defaults();
    set(st, RS::CullMode, d3d::kCullCW);
    return st;
}

namespace {

StateSlot make_slot(const fx::State& s, StateOrigin origin, int index, uint32_t serial, ApplyReport* rep) {
    StateSlot slot;
    slot.origin = origin;
    slot.source_pass = serial;
    const int16_t idx = (index >= 0 && static_cast<size_t>(index) <= kMaxPassStates) ? static_cast<int16_t>(index) : int16_t(-1);
    switch (s.kind) {
    case fx::State::Kind::Expression:
        slot.origin = StateOrigin::Expression;
        slot.source_state = idx;
        if (rep) ++rep->expressions;
        break;
    case fx::State::Kind::ParameterRef:
    case fx::State::Kind::ArraySelector:
        slot.origin = StateOrigin::ParameterRef;
        slot.source_state = idx;
        if (rep) ++rep->parameter_refs;
        break;
    default:
        if (!s.value.words.empty()) slot.value = s.value.words[0];
        break;
    }
    return slot;
}

void apply_state(const fx::Effect& effect, const fx::State& s, int index, StateOrigin origin,
                 DeviceState& st, ApplyReport* rep, int depth);

void apply_sampler_states(const fx::Effect& effect, const std::vector<fx::State>& states, int n,
                          StateOrigin origin, DeviceState& st, int depth) {
    for (size_t i = 0; i < states.size(); ++i) {
        fx::State s = states[i];
        s.index = static_cast<uint32_t>(n);  // sampler_state blocks have no index of their own
        apply_state(effect, s, -1, origin, st, nullptr, depth + 1);
    }
}

void apply_state(const fx::Effect& effect, const fx::State& s, int index, StateOrigin origin,
                 DeviceState& st, ApplyReport* rep, int depth) {
    const uint32_t opi = s.operation;
    if (opi < op::kFirstTss) {
        st.rs[opi] = make_slot(s, origin, index, st.apply_serial, rep);
        return;
    }
    if (opi < op::kFirstTss + static_cast<uint32_t>(TSS::Count)) {
        if (s.index < kMaxTextureStages)
            st.tss[s.index][opi - op::kFirstTss] = make_slot(s, origin, index, st.apply_serial, rep);
        else if (rep) ++rep->ignored;
        return;
    }
    if (opi >= op::kFirstSampler && opi < op::kFirstSampler + static_cast<uint32_t>(SS::Count)) {
        if (s.index < kMaxSamplers)
            st.ss[s.index][opi - op::kFirstSampler] = make_slot(s, origin, index, st.apply_serial, rep);
        else if (rep) ++rep->ignored;
        return;
    }
    switch (opi) {
    case op::kVertexShader:
    case op::kPixelShader: {
        const bool vs = opi == op::kVertexShader;
        st.sets_shaders = true;
        // NULL shader = Constant kind with object id whose object has no data.
        bool non_null = s.kind == fx::State::Kind::Shader && !s.blob.empty();
        if (s.kind == fx::State::Kind::ParameterRef) non_null = true;  // = <shaderParam>
        if (!non_null && !s.value.objects.empty()) {
            auto it = effect.objects.find(s.value.objects[0]);
            non_null = it != effect.objects.end() && !it->second.data.empty();
        }
        (vs ? st.has_vertex_shader : st.has_pixel_shader) = non_null;
        (vs ? st.vertex_shader_state : st.pixel_shader_state) = non_null ? index : -1;
        (vs ? st.vertex_shader_pass : st.pixel_shader_pass) = st.apply_serial;
        return;
    }
    case op::kTexture:
        if (s.index < kMaxSamplers) {
            st.stage[s.index].texture_param =
                (s.kind == fx::State::Kind::ParameterRef) ? s.refName : std::string();
        }
        return;
    case op::kSampler:
        if (s.index < kMaxSamplers && depth < 4) {
            if (s.kind == fx::State::Kind::ParameterRef) {
                st.stage[s.index].sampler_param = s.refName;
                if (const fx::Parameter* p = effect.findParameter(s.refName)) {
                    if (!p->value.samplers.empty())
                        apply_sampler_states(effect, p->value.samplers[0], static_cast<int>(s.index),
                                             origin, st, depth);
                }
            } else if (!s.value.samplers.empty()) {  // inline sampler_state
                apply_sampler_states(effect, s.value.samplers[0], static_cast<int>(s.index), origin, st,
                                     depth);
            }
        }
        return;
    default:
        if (fx::stateInfo(opi).cls == fx::StateClass::Unknown) {
            if (rep) rep->unknown.push_back("op" + std::to_string(opi));
        } else if (rep) {
            ++rep->ignored;  // transforms, materials, lights, shader constants: FF / per draw data
        }
        return;
    }
}

}  // namespace

void apply_pass(const fx::Effect& effect, const fx::Pass& pass, StateOrigin origin, DeviceState& st,
                ApplyReport* report) {
    ++st.apply_serial;  // new pass: indices recorded for earlier passes become stale
    const size_t n = std::min(pass.states.size(), kMaxPassStates);
    if (n < pass.states.size() && report) report->unknown.push_back("pass has more than 32767 states");
    for (size_t i = 0; i < n; ++i)
        apply_state(effect, pass.states[i], static_cast<int>(i), origin, st, report, 0);
}

void apply_sampler_param(const fx::Effect& effect, const std::string& sampler_param, int n,
                         StateOrigin origin, DeviceState& st) {
    if (n < 0 || n >= kMaxSamplers) return;
    const fx::Parameter* p = effect.findParameter(sampler_param);
    if (!p || p->value.samplers.empty()) return;
    st.stage[static_cast<size_t>(n)].sampler_param = sampler_param;
    for (const auto& s : p->value.samplers[0]) {
        if (s.operation == op::kTexture && s.kind == fx::State::Kind::ParameterRef)
            st.stage[static_cast<size_t>(n)].texture_param = s.refName;
    }
    apply_sampler_states(effect, p->value.samplers[0], n, origin, st, 0);
}

}  // namespace alamo::materials

// Track B integration. See translated.hpp.
#include "alamo/materials/translated.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "alamo/core/hash.hpp"
#include "alamo/materials/effect_loader.hpp"
#include "alamo/materials/fxlc.hpp"
#include "alamo/materials/register_file.hpp"
#include "fx2_reader.hpp"

namespace alamo::materials {

uint32_t register_program_id(uint64_t full_hash) {
    // Lookup-only map (never iterated): no ordering dependence on the hash container.
    static std::mutex id_mutex;
    static std::unordered_map<uint32_t, uint64_t> ids;
    uint32_t id = static_cast<uint32_t>(full_hash ^ (full_hash >> 32)) | kProgramIdTranslatedBit;
    const std::lock_guard<std::mutex> lock(id_mutex);
    for (;;) {
        const auto [it, inserted] = ids.try_emplace(id, full_hash);
        if (inserted || it->second == full_hash) return id;
        id = ((id + 1u) & ~kProgramIdTranslatedBit) | kProgramIdTranslatedBit;  // fold clash: next id
    }
}

uint32_t translated_program_id(std::span<const uint8_t> bytecode, const shaderxlat::TranslateOptions& options) {
    uint64_t h = alamo::fnv1a64(bytecode.data(), bytecode.size());
    const uint64_t ok = options.key();
    const uint32_t ver = shaderxlat::kTranslatorVersion;
    h = alamo::fnv1a64(&ok, sizeof ok, h);
    h = alamo::fnv1a64(&ver, sizeof ver, h);
    return register_program_id(h);
}

PsoKey TranslatedProgram::key(const PsoKey& pass_key) const {
    PsoKey k = pass_key;
    k.vs_id = vs_id;
    k.ps_id = ps_id;
    k.flags = static_cast<uint8_t>(k.flags | kPsoTranslated | (writes_depth ? kPsoWritesDepth : 0));
    return k;
}

namespace {
constexpr ResourceBinding kVsConst{0, 0, BindingKind::UniformBuffer, "VSConstants", 1, "float4 c[256], CTAB register image"};
constexpr ResourceBinding kPsConst{0, 1, BindingKind::UniformBuffer, "PSConstants", 1,
                                   "float4 c[32]; ps_1_x bump env c16+n / c24+n"};
constexpr ResourceBinding kVsInt{0, 2, BindingKind::UniformBuffer, "VSIntConstants", 1, "int4 i[16]; uint4 b[4]"};
constexpr ResourceBinding kPsInt{0, 3, BindingKind::UniformBuffer, "PSIntConstants", 1, "int4 i[16]; uint4 b[4]"};
const char* const kSamplerNames[16] = {"s0", "s1", "s2",  "s3",  "s4",  "s5",  "s6",  "s7",
                                       "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15"};

// float -> int32 without UB: truncates toward zero, saturates out-of-range values, NaN -> 0.
int32_t float_to_int_sat(float f) {
    if (std::isnan(f)) return 0;
    if (f >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
    if (f <= -2147483648.0f) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(f);
}
}  // namespace

std::shared_ptr<const TranslatedProgram> translate_pass(const ResolvedPass& pass, const shaderxlat::RuntimeDxc& dxc,
                                                        const shaderxlat::ShaderCache* cache, std::string* err) {
    if (pass.program_status != ProgramStatus::Translated || pass.vs_bytecode.empty() || pass.ps_bytecode.empty()) {
        if (err) *err = "pass " + pass.name + " is not a translated pass";
        return nullptr;
    }
    auto tp = std::make_shared<TranslatedProgram>();
    std::string e;
    if (!shaderxlat::translate_and_compile(pass.vs_bytecode, pass.vs_options, dxc, cache, tp->vs, &e)) {
        if (err) *err = pass.name + " VS: " + e;
        return nullptr;
    }
    if (!shaderxlat::translate_and_compile(pass.ps_bytecode, pass.ps_options, dxc, cache, tp->ps, &e)) {
        if (err) *err = pass.name + " PS: " + e;
        return nullptr;
    }
    tp->vs_id = translated_program_id(pass.vs_bytecode, pass.vs_options);
    tp->ps_id = translated_program_id(pass.ps_bytecode, pass.ps_options);
    tp->writes_depth = tp->ps.iface.writes_depth;
    tp->can_discard = tp->ps.iface.can_discard;
    // Bump-env registers exist only for ps_1_x (c16..c31 are ordinary constants for ps_2_x).
    tp->bump_env = tp->ps.iface.bump_env && tp->ps.iface.major == 1;
    tp->vs_int_bool = tp->vs.iface.int_bool_constants;
    tp->ps_int_bool = tp->ps.iface.int_bool_constants;
    const VertexLayoutDesc layout = vertex_layout_desc(pass.pso[0].vertex_layout);
    for (const auto& in : tp->vs.iface.inputs) {
        TranslatedInput ti;
        ti.location = in.location;
        ti.usage = in.usage;
        ti.usage_index = in.usage_index;
        ti.components = in.components;
        for (const auto& a : layout.attributes)
            if (a.location == in.location) {
                ti.fed = true;
                ti.vk_format = a.vk_format;
                ti.offset = a.offset;
            }
        tp->inputs.push_back(ti);
    }
    tp->bindings.push_back(kVsConst);
    tp->bindings.push_back(kPsConst);
    if (tp->vs_int_bool) tp->bindings.push_back(kVsInt);
    if (tp->ps_int_bool) tp->bindings.push_back(kPsInt);
    for (const auto& s : tp->ps.iface.samplers)
        if (s.slot < 16)
            tp->bindings.push_back({1, s.slot, BindingKind::CombinedImageSampler, kSamplerNames[s.slot], 1,
                                    s.dim == shaderxlat::TextureDim::Cube     ? "TextureCube"
                                    : s.dim == shaderxlat::TextureDim::Volume ? "Texture3D"
                                                                              : "Texture2D"});
    return tp;
}

void fill_bump_env(const ResolvedPass& pass, const fx::Effect& effect, const ParamLookup& lookup,
                   std::span<float4> regs) {
    if (!pass.uses_bump_env) return;  // ps_2_x / no bump op: c16..c31 belong to the CTAB fill
    const fx::Pass* src = nullptr;
    if (pass.technique >= 0 && static_cast<size_t>(pass.technique) < effect.techniques.size() && pass.pass_index >= 0 &&
        static_cast<size_t>(pass.pass_index) < effect.techniques[static_cast<size_t>(pass.technique)].passes.size())
        src = &effect.techniques[static_cast<size_t>(pass.technique)].passes[static_cast<size_t>(pass.pass_index)];
    auto value = [&](int stage, TSS t) -> float {
        const StateSlot& s = pass.state.tss[static_cast<size_t>(stage)][static_cast<size_t>(t)];
        if (s.origin == StateOrigin::Expression) {  // e.g. SceneHeat BUMPENVMAT00 = DistortionAmount
            if (!src || !pass.state.refers_to_last_pass(s) || static_cast<size_t>(s.source_state) >= src->states.size())
                return 0.0f;
            const fx::State& st = src->states[static_cast<size_t>(s.source_state)];
            Expression ex;
            std::vector<float4> out(1);
            if (parse_expression(st.blob.data(), st.blob.size(), ex) && evaluate(ex, effect, lookup, out)) return out[0].x;
            return 0.0f;
        }
        return std::bit_cast<float>(s.value);
    };
    for (int stage = 0; stage < kMaxTextureStages; ++stage) {
        if (16u + static_cast<size_t>(stage) < regs.size())
            regs[16u + static_cast<size_t>(stage)] = float4{value(stage, TSS::BumpEnvMat00), value(stage, TSS::BumpEnvMat01),
                                                           value(stage, TSS::BumpEnvMat10), value(stage, TSS::BumpEnvMat11)};
        if (24u + static_cast<size_t>(stage) < regs.size())
            regs[24u + static_cast<size_t>(stage)] =
                float4{value(stage, TSS::BumpEnvLScale), value(stage, TSS::BumpEnvLOffset), 0.0f, 0.0f};
    }
}

void fill_int_bool_constants(const fx::Effect& effect, const RegisterFilePlan& plan, const ParamLookup& lookup,
                             IntBoolConstants& out) {
    out = IntBoolConstants{};
    for (const auto& b : plan.ints) {
        if (b.parameter < 0) continue;
        const std::vector<float> v = param_value(effect.parameters[static_cast<size_t>(b.parameter)], lookup);
        for (uint32_t r = 0; r < b.count && b.reg + r < 16; ++r)
            for (uint32_t c = 0; c < 4; ++c) {
                const size_t i = size_t(r) * 4 + c;
                out.i[b.reg + r][c] = i < v.size() ? float_to_int_sat(v[i]) : 0;
            }
    }
    for (const auto& b : plan.bools) {
        if (b.parameter < 0) continue;
        const std::vector<float> v = param_value(effect.parameters[static_cast<size_t>(b.parameter)], lookup);
        for (uint32_t r = 0; r < std::max<uint16_t>(b.count, 1) && b.reg + r < 16; ++r) {
            const uint32_t reg = b.reg + r;
            out.b[reg / 4][reg % 4] = (r < v.size() && v[r] != 0.0f) ? 1u : 0u;
        }
    }
}

}  // namespace alamo::materials

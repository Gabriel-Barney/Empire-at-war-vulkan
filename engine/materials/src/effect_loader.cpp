// FXO material loader. See effect_loader.hpp.
#include "alamo/materials/effect_loader.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <set>
#include <utility>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/shaderxlat/bytecode.hpp"
#include "fx2_reader.hpp"

namespace alamo::materials {
namespace {

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}

struct PhaseInfo { RenderPhase p; const char* name; const char* file; };
constexpr PhaseInfo kPhases[] = {
    {RenderPhase::Opaque, "Opaque", "Data\\Art\\Shaders\\Engine\\PhaseOpaque.fxo"},
    {RenderPhase::Transparent, "Transparent", "Data\\Art\\Shaders\\Engine\\PhaseTransparent.fxo"},
    {RenderPhase::Shadow, "Shadow", "Data\\Art\\Shaders\\Engine\\PhaseShadow.fxo"},
    {RenderPhase::TerrainMesh, "TerrainMesh", "Data\\Art\\Shaders\\Engine\\PhaseTerrainMesh.fxo"},
    {RenderPhase::TerrainWater, "TerrainWater", "Data\\Art\\Shaders\\Engine\\PhaseWater.fxo"},
    {RenderPhase::Heat, "Heat", "Data\\Art\\Shaders\\Engine\\PhaseHeat.fxo"},
    {RenderPhase::Occluded, "Occluded", "Data\\Art\\Shaders\\Engine\\PhaseOccluded.fxo"},
    {RenderPhase::Terrain, "Terrain", "Data\\Art\\Shaders\\Engine\\PhaseTerrain.fxo"},
    {RenderPhase::Water, "Water", "Data\\Art\\Shaders\\Engine\\PhaseWater.fxo"},
    {RenderPhase::Bloom, "Bloom", "Data\\Art\\Shaders\\Engine\\PhaseBloom.fxo"},
    {RenderPhase::Fissure, "Fissure", "Data\\Art\\Shaders\\Engine\\PhaseFissure.fxo"},
};

std::string string_param(const fx::Effect& e, const fx::Parameter& p) {
    if (p.type.type != fx::ParamType::String || p.value.objects.empty()) return {};
    return e.string(p.value.objects[0]).value_or(std::string());
}

uint32_t blob_id(std::span<const uint8_t> b) {
    // Track-B (translated bytecode) ids always carry kProgramIdTranslatedBit, so they can never
    // collide with hand-written program ids (bit clear); the registry keeps them unique among
    // themselves (fold clashes probe to the next free id).
    return register_program_id(alamo::fnv1a64(b.data(), b.size()));
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    return upper(hay).find(upper(needle)) != std::string::npos;
}

// Lookup that answers LIGHT_SCALE with a fixed value (fade probing) and nothing else.
ParamLookup light_scale_lookup(float w) {
    return [w](const fx::Parameter& p, std::span<float> out) {
        if (upper(p.type.semantic) != "LIGHT_SCALE" || out.size() < 4) return false;
        out[0] = out[1] = out[2] = 1.0f;
        out[3] = w;
        return true;
    };
}

int eval_state_int(const fx::Effect& e, const fx::State& s, const ParamLookup& lookup, bool* ok) {
    Expression ex;
    std::vector<float4> out(4);
    *ok = parse_expression(s.blob.data(), s.blob.size(), ex) && evaluate(ex, e, lookup, out);
    return (*ok && out[0].x != 0.0f) ? 1 : 0;
}

}  // namespace

RenderPhase render_phase_from_name(std::string_view s) {
    const std::string u = upper(s);
    for (const auto& p : kPhases)
        if (u == upper(p.name)) return p.p;
    return RenderPhase::None;
}
const char* render_phase_name(RenderPhase p) {
    for (const auto& i : kPhases)
        if (i.p == p) return i.name;
    return "None";
}
const char* phase_effect_file(RenderPhase p) {
    for (const auto& i : kPhases)
        if (i.p == p) return i.file;
    return nullptr;
}

TechniqueLod technique_lod_from_string(std::string_view lod, std::string_view name) {
    const std::string u = upper(lod);
    if (u == "DX9") return TechniqueLod::DX9;
    if (u == "DX8ATI") return TechniqueLod::DX8ATI;
    if (u == "DX8") return TechniqueLod::DX8;
    if (u == "FIXEDFUNCTION") return TechniqueLod::FixedFunction;
    if (u == "FF") return TechniqueLod::FF;
    if (upper(name) == "MAX_VIEWPORT") return TechniqueLod::MaxViewport;
    return TechniqueLod::Unknown;
}
const char* technique_lod_name(TechniqueLod l) {
    switch (l) {
    case TechniqueLod::DX9: return "DX9";
    case TechniqueLod::DX8ATI: return "DX8ATI";
    case TechniqueLod::DX8: return "DX8";
    case TechniqueLod::FixedFunction: return "FIXEDFUNCTION";
    case TechniqueLod::FF: return "FF";
    case TechniqueLod::MaxViewport: return "max_viewport";
    default: return "unknown";
    }
}

int pick_best_technique(const fx::Effect& e) {
    int best = -1;
    TechniqueLod best_lod = TechniqueLod::MaxViewport;
    for (size_t i = 0; i < e.techniques.size(); ++i) {
        const TechniqueLod l = technique_lod_from_string(e.techniqueLod(e.techniques[i]), e.techniques[i].name);
        if (best < 0 || static_cast<int>(l) < static_cast<int>(best_lod)) {
            best = static_cast<int>(i);
            best_lod = l;
        }
    }
    return best;
}

bool pass_has_program(const ResolvedPass& p) {
    return p.restore_only || p.program_status == ProgramStatus::Translated || (p.program_status == ProgramStatus::Ok && p.program.vs);
}

size_t MaterialEffect::draw_pass_count() const {
    return static_cast<size_t>(std::count_if(passes.begin(), passes.end(), [](const ResolvedPass& p) { return !p.restore_only; }));
}

ResolvedPass resolve_pass(const fx::Effect& e, int technique, int pass_index, const DeviceState& baseline,
                          const LoadOptions& options, std::string_view effect_name, VertexLayout layout) {
    const fx::Pass& pass = e.techniques[static_cast<size_t>(technique)].passes[static_cast<size_t>(pass_index)];
    ResolvedPass r;
    r.name = pass.name;
    r.pass_index = pass_index;
    r.technique = technique;
    DeviceState st = baseline;
    apply_pass(e, pass, StateOrigin::Constant, st, &r.report);

    // Restore / cleanup passes: no shader states and only sampler (or no) states.
    bool only_samplers = true;
    for (const auto& s : pass.states) {
        const fx::StateClass c = fx::stateInfo(s.operation).cls;
        if (c != fx::StateClass::Sampler && c != fx::StateClass::SetSampler) only_samplers = false;
    }
    r.restore_only = pass_index > 0 && (only_samplers || contains_ci(pass.name, "cleanup"));
    // RENDER.md 2.1: a pass annotated AlamoCleanup = true is excluded from the draw passes; the
    // annotation, when present, overrides the heuristic.
    for (const auto& a : pass.annotations)
        if (upper(a.type.name) == "ALAMOCLEANUP" && !a.value.words.empty()) r.restore_only = a.value.words[0] != 0;
    r.fixed_function = !st.has_vertex_shader && !st.has_pixel_shader;
    const bool mixed = st.has_vertex_shader != st.has_pixel_shader;

    // Fade switch: evaluate a dynamic ALPHABLENDENABLE at LIGHT_SCALE.w = 1 and 0.5.
    int blend_opaque = -1, blend_fading = -1;
    const StateSlot& abe = st.slot(RS::AlphaBlendEnable);
    if (abe.origin == StateOrigin::Expression && st.refers_to_last_pass(abe) &&
        static_cast<size_t>(abe.source_state) < pass.states.size()) {
        const fx::State& s = pass.states[static_cast<size_t>(abe.source_state)];
        bool ok1 = false, ok2 = false;
        blend_opaque = eval_state_int(e, s, light_scale_lookup(1.0f), &ok1);
        blend_fading = eval_state_int(e, s, light_scale_lookup(0.5f), &ok2);
        if (!ok1 || !ok2) {
            blend_opaque = 0;
            blend_fading = 1;
        }
        r.fade_dynamic = blend_opaque != blend_fading;
        Expression ex;
        auto ce = std::make_shared<CompiledExpression>();
        if (r.fade_dynamic && parse_expression(s.blob.data(), s.blob.size(), ex) && ce->compile(ex, e, nullptr))
            r.fade_expr = std::move(ce);
    }

    // Program.
    if (r.fixed_function) {
        ProgramLookup pl = find_program(effect_name);
        if (pl.status == ProgramStatus::Ok && pl.program.family == ProgramFamily::FixedFunction) {
            r.program = pl.program;
        } else {
            r.program = ff_program(ff_inputs_for_layout(layout));
        }
        r.program_status = ProgramStatus::Ok;
        r.ff = extract_ff_state(e, pass, st);
    } else if (!mixed) {
        ProgramLookup pl = find_program(effect_name);
        if (pl.status == ProgramStatus::Ok && pl.program.family != ProgramFamily::FixedFunction) {
            r.program = pl.program;
            r.program_status = ProgramStatus::Ok;
        } else {
            r.program_status = ProgramStatus::NeedsTranslation;
        }
    } else {
        r.program_status = ProgramStatus::NeedsTranslation;  // shader + FF mix (not in vanilla)
    }

    // Bytecode, register plans, sampler bindings.
    std::span<const uint8_t> vsb, psb;
    const int vsi = st.last_pass_vertex_shader(), psi = st.last_pass_pixel_shader();
    if (vsi >= 0 && static_cast<size_t>(vsi) < pass.states.size()) vsb = shader_bytecode(e, pass.states[static_cast<size_t>(vsi)]);
    if (psi >= 0 && static_cast<size_t>(psi) < pass.states.size()) psb = shader_bytecode(e, pass.states[static_cast<size_t>(psi)]);
    if (!vsb.empty()) {
        RegisterFilePlan plan;
        if (build_register_plan(e, vsb, plan)) r.vs_plan = std::move(plan);
        r.vs_bytecode_hash = alamo::fnv1a64(vsb.data(), vsb.size());
    }
    if (!psb.empty()) {
        RegisterFilePlan plan;
        if (build_register_plan(e, psb, plan)) r.ps_plan = std::move(plan);
        r.ps_bytecode_hash = alamo::fnv1a64(psb.data(), psb.size());
    }
    auto texture_semantic = [&](const std::string& tex) -> std::string {
        if (const fx::Parameter* p = e.findParameter(tex)) return p->type.semantic;
        return {};
    };
    if (r.ps_plan) {
        for (const auto& sr : r.ps_plan->samplers) {
            for (uint16_t k = 0; k < sr.count; ++k) {
                const int slot = sr.reg + k;
                if (slot >= kMaxSamplers) continue;
                DeviceState s2 = st;
                apply_sampler_param(e, sr.sampler_param, slot, StateOrigin::Constant, s2);
                SamplerBinding b;
                b.slot = static_cast<uint16_t>(slot);
                b.sampler_param = sr.sampler_param;
                b.texture_param = s2.stage[static_cast<size_t>(slot)].texture_param;
                b.texture_semantic = texture_semantic(b.texture_param);
                b.key = resolve_sampler(s2, slot, options.engine_sampler);
                r.samplers.push_back(std::move(b));
            }
        }
    } else if (r.fixed_function && r.ff) {
        for (int s = 0; s < r.ff->active_stages; ++s) {
            SamplerBinding b;
            b.slot = static_cast<uint16_t>(s);
            b.sampler_param = st.stage[static_cast<size_t>(s)].sampler_param;
            b.texture_param = st.stage[static_cast<size_t>(s)].texture_param;
            b.texture_semantic = texture_semantic(b.texture_param);
            b.key = resolve_sampler(st, s, options.engine_sampler);
            r.samplers.push_back(std::move(b));
        }
    }

    // Track B: keep the bytecode, compute the translation options and push defaults, and mark the
    // pass Translated. No DXC here: translate_pass() compiles on demand.
    if (r.program_status == ProgramStatus::NeedsTranslation && !vsb.empty() && !psb.empty()) {
        shaderxlat::Program vp, pp;
        if (shaderxlat::parse(vsb, vp) && shaderxlat::parse(psb, pp) && vp.is_vs() && pp.is_ps() && vp.major <= 2 &&
            pp.major <= 2) {
            r.vs_bytecode.assign(vsb.begin(), vsb.end());
            r.ps_bytecode.assign(psb.begin(), psb.end());
            if (pp.ps1())
                for (const auto& in : pp.instrs)
                    if (in.op == shaderxlat::Op::TexBem || in.op == shaderxlat::Op::TexBemL || in.op == shaderxlat::Op::Bem)
                        r.uses_bump_env = true;
            if (pp.ps1()) {
                for (int s = 0; s < kMaxTextureStages; ++s) {
                    // D3DTTFF_PROJECTED on ps_1_1..1_3: tex divides by the coordinate's w.
                    if (pp.minor <= 3 && (st.get(s, TSS::TextureTransformFlags) & d3d::kTtffProjected))
                        r.ps_options.projected_stages = static_cast<uint16_t>(r.ps_options.projected_stages | (1u << s));
                    // Stripped CTAB (mods): the texture parameter bound to the stage gives the dimension.
                    if (!pp.has_ctab) {
                        const StageBinding& sb = st.stage[static_cast<size_t>(s)];
                        const fx::Parameter* tp = e.findParameter(sb.texture_param);
                        if (!tp) tp = e.findParameter(sb.sampler_param);
                        if (tp) {
                            shaderxlat::TextureDim d = shaderxlat::TextureDim::Tex2D;
                            if (tp->type.type == fx::ParamType::TextureCube || tp->type.type == fx::ParamType::SamplerCube)
                                d = shaderxlat::TextureDim::Cube;
                            if (tp->type.type == fx::ParamType::Texture3D || tp->type.type == fx::ParamType::Sampler3D)
                                d = shaderxlat::TextureDim::Volume;
                            r.ps_options.sampler_dims[static_cast<size_t>(s)] = d;
                        }
                    }
                }
                if (r.samplers.empty()) {  // no CTAB samplers: ps_1_x stage n = sampler n
                    for (int s = 0; s < 4; ++s) {
                        const StageBinding& sb = st.stage[static_cast<size_t>(s)];
                        if (sb.texture_param.empty() && sb.sampler_param.empty()) continue;
                        SamplerBinding b;
                        b.slot = static_cast<uint16_t>(s);
                        b.sampler_param = sb.sampler_param;
                        b.texture_param = sb.texture_param;
                        b.texture_semantic = texture_semantic(b.texture_param);
                        b.key = resolve_sampler(st, s, options.engine_sampler);
                        r.samplers.push_back(std::move(b));
                    }
                }
            }
            const PushConstants pc = resolve_push(st);
            shaderxlat::TranslatedPush& tp = r.translated_push;
            tp.alpha_func = pc.alpha_func;
            tp.alpha_ref = pc.alpha_ref;
            tp.flags = 0;
            if (st.get(RS::FogEnable))
                tp.flags = shaderxlat::kPushFog | ((st.get(RS::FogTableMode) & 3u) << shaderxlat::kFogTableShift);
            const uint32_t fc = st.get(RS::FogColor);
            tp.fog_color[0] = static_cast<float>((fc >> 16) & 0xFF) / 255.0f;
            tp.fog_color[1] = static_cast<float>((fc >> 8) & 0xFF) / 255.0f;
            tp.fog_color[2] = static_cast<float>(fc & 0xFF) / 255.0f;
            tp.fog_params[0] = std::bit_cast<float>(st.get(RS::FogStart));
            tp.fog_params[1] = std::bit_cast<float>(st.get(RS::FogEnd));
            tp.fog_params[2] = std::bit_cast<float>(st.get(RS::FogDensity));
            r.program_status = ProgramStatus::Translated;
        }
    }

    // Keys.
    for (int v = 0; v < 2; ++v) {
        int override_blend = -1;
        if (abe.origin == StateOrigin::Expression) override_blend = v == 0 ? blend_opaque : blend_fading;
        PsoKey k = resolve_pso(st, override_blend);
        k.vertex_layout = layout;
        if (r.program_status == ProgramStatus::Ok) {
            k.vs_id = r.program.vs ? r.program.vs->id : 0;
            k.ps_id = r.program.ps ? r.program.ps->id : 0;
        } else if (r.program_status == ProgramStatus::Translated) {
            k.vs_id = translated_program_id(vsb, r.vs_options);
            k.ps_id = translated_program_id(psb, r.ps_options);
            k.flags = static_cast<uint8_t>(k.flags | kPsoTranslated);
        } else {
            k.vs_id = vsb.empty() ? 0 : blob_id(vsb);
            k.ps_id = psb.empty() ? 0 : blob_id(psb);
        }
        r.pso[v] = k;
    }
    r.dynamic = resolve_dynamic(st);
    r.push = resolve_push(st);
    r.state = st;
    return r;
}

std::string partial_state_signature(const fx::Effect& e, int technique, int pass_index) {
    const fx::Pass& pass = e.techniques[static_cast<size_t>(technique)].passes[static_cast<size_t>(pass_index)];
    static const RS kPso[] = {RS::ZEnable, RS::FillMode, RS::ZWriteEnable, RS::AlphaTestEnable, RS::SrcBlend,
                              RS::DestBlend, RS::CullMode, RS::ZFunc, RS::AlphaRef, RS::AlphaFunc,
                              RS::AlphaBlendEnable, RS::StencilEnable, RS::StencilFail, RS::StencilZFail,
                              RS::StencilPass, RS::StencilFunc, RS::ColorWriteEnable, RS::BlendOp,
                              RS::TwoSidedStencilMode, RS::CcwStencilFail, RS::CcwStencilZFail,
                              RS::CcwStencilPass, RS::CcwStencilFunc, RS::DepthBias, RS::SlopeScaleDepthBias,
                              RS::SeparateAlphaBlendEnable, RS::SrcBlendAlpha, RS::DestBlendAlpha, RS::BlendOpAlpha};
    std::set<std::string> items;
    for (const auto& s : pass.states) {
        if (s.operation >= op::kFirstTss) continue;
        bool pso = false;
        for (RS r : kPso)
            if (static_cast<uint32_t>(r) == s.operation) pso = true;
        if (!pso) continue;
        std::string v;
        if (s.kind == fx::State::Kind::Expression) v = "dyn";
        else if (s.kind == fx::State::Kind::ParameterRef) v = "<" + s.refName + ">";
        else v = s.value.words.empty() ? "?" : std::to_string(s.value.words[0]);
        items.insert(std::string(fx::stateInfo(s.operation).name) + "=" + v);
    }
    std::string out;
    for (const auto& i : items) {
        if (!out.empty()) out += ' ';
        out += i;
    }
    return out;
}

namespace {

std::string effect_base_name(std::string_view n) {
    const size_t slash = n.find_last_of("\\/");
    if (slash != std::string_view::npos) n = n.substr(slash + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string_view::npos) n = n.substr(0, dot);
    return upper(n);
}

EffectRemap normalize_remap(const EffectRemap& in) {
    EffectRemap out;
    for (const auto& [from, to] : in) {
        if (from.empty() || to.empty()) continue;
        out.emplace(effect_base_name(from), to);  // first wins on keys equal after normalisation
    }
    return out;
}

}  // namespace

const EffectRemap& compat_missing_effect_remap() {
    static const EffectRemap kTable = {{"FISSUREMESHGLOSS", "MeshGloss.fx"}};
    return kTable;
}

EffectLibrary::EffectLibrary(const alamo::FileProvider& files, LoadOptions options)
    : files_(files), options_(std::move(options)) {
    options_.missing_effect_remap = normalize_remap(options_.missing_effect_remap);
}
EffectLibrary::~EffectLibrary() = default;

void EffectLibrary::set_missing_effect_remap(EffectRemap remap) {
    options_.missing_effect_remap = normalize_remap(remap);
}

bool host_path_under(std::string_view host, std::string_view dir) {
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.remove_suffix(1);
    if (dir.empty() || host.size() <= dir.size() + 1) return false;
    auto fold = [](char c) {
        if (c == '/') return '\\';
        if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
        return c;
    };
    for (size_t i = 0; i < dir.size(); ++i)
        if (fold(host[i]) != fold(dir[i])) return false;
    return fold(host[dir.size()]) == '\\';
}

namespace {
// Candidate game paths of an effect name for one extension (".fxo" / ".fx"): a name with a directory
// is tried as given (only when its own extension is `ext` or absent) and with `ext`; a bare name under
// Shaders\, Shaders\Engine\ and Shaders\Terrain\ in that order (RENDER.md 6).
std::vector<std::string> effect_candidates(std::string_view effect, std::string_view ext) {
    std::string n(effect);
    std::string base = n;
    std::string own_ext;
    const size_t dot = base.find_last_of('.');
    const size_t slash = base.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        own_ext = upper(base.substr(dot));
        base = base.substr(0, dot);
    }
    std::vector<std::string> cands;
    if (slash != std::string::npos) {
        if (own_ext.empty() || own_ext == upper(std::string(ext))) cands.push_back(n);
        cands.push_back(base + std::string(ext));
    } else {
        for (const char* dir : {"Data\\Art\\Shaders\\", "Data\\Art\\Shaders\\Engine\\", "Data\\Art\\Shaders\\Terrain\\"})
            cands.push_back(std::string(dir) + base + std::string(ext));
    }
    return cands;
}
}  // namespace

std::optional<std::string> EffectLibrary::find_effect_path(std::string_view effect) const {
    for (const auto& c : effect_candidates(effect, ".fxo"))
        if (files_.exists(c)) return c;
    return std::nullopt;
}

std::optional<std::string> EffectLibrary::find_effect_source(std::string_view effect) const {
    for (const auto& c : effect_candidates(effect, ".fx"))
        if (files_.exists(c)) return c;
    return std::nullopt;
}

const DeviceState& EffectLibrary::phase_baseline(RenderPhase p) {
    auto it = baselines_.find(p);
    if (it != baselines_.end()) return it->second;
    DeviceState st = engine_device_defaults();
    if (const char* file = phase_effect_file(p)) {
        try {
            std::vector<uint8_t> bytes;
            if (files_.read(file, bytes)) {
                fx::Effect pe = fx::parseEffect(bytes.data(), bytes.size());
                const int t = pick_best_technique(pe);
                if (t >= 0 && !pe.techniques[static_cast<size_t>(t)].passes.empty())
                    apply_pass(pe, pe.techniques[static_cast<size_t>(t)].passes[0], StateOrigin::Phase, st);
            }
        } catch (const std::exception&) {
            st = engine_device_defaults();  // malformed phase effect: D3D9 defaults
        }
    }
    return baselines_.emplace(p, st).first->second;
}

std::optional<MaterialEffect> EffectLibrary::load(std::string_view effect, std::string* err) {
    try {
        return load_file(effect, err);
    } catch (const std::exception& ex) {
        if (err) *err = std::string(effect) + ": " + ex.what();
    }
    return std::nullopt;
}

std::optional<MaterialEffect> EffectLibrary::load_file(std::string_view effect, std::string* err) {
    auto path = find_effect_path(effect);
    if (!path) {
        if (auto src = find_effect_source(effect)) {
            // The original compiles the HLSL with D3DX here; the port loads compiled .fxo only.
            if (err) *err = "effect source " + *src + " has no compiled .fxo (HLSL .fx compilation is not supported)";
            return std::nullopt;
        }
        if (err) *err = "effect not found: " + std::string(effect);
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    if (!files_.read(*path, bytes)) {
        if (err) *err = "cannot read " + *path;
        return std::nullopt;
    }
    std::string name = *path;
    auto r = load_from_memory(name, bytes, err);
    if (r) r->path = *path;
    return r;
}

std::optional<MaterialEffect> EffectLibrary::load_from_memory(std::string_view name_or_path,
                                                              const std::vector<uint8_t>& bytes, std::string* err) {
    // Load boundary: no exception (bad_alloc, length_error, out_of_range, ParseError...) escapes.
    try {
        return load_impl(name_or_path, bytes, err);
    } catch (const std::exception& ex) {
        if (err) *err = std::string(name_or_path) + ": " + ex.what();
    }
    return std::nullopt;
}

std::optional<MaterialEffect> EffectLibrary::load_impl(std::string_view name_or_path,
                                                       const std::vector<uint8_t>& bytes, std::string* err) {
    return load_parsed(name_or_path, std::make_shared<fx::Effect>(fx::parseEffect(bytes.data(), bytes.size())), err);
}

std::optional<MaterialEffect> EffectLibrary::load_parsed(std::string_view name_or_path, std::shared_ptr<fx::Effect> e,
                                                         std::string* err) {
    MaterialEffect m;
    {
        std::string_view n = name_or_path;
        const size_t slash = n.find_last_of("\\/");
        if (slash != std::string_view::npos) n = n.substr(slash + 1);
        const size_t dot = n.find_last_of('.');
        if (dot != std::string_view::npos) n = n.substr(0, dot);
        m.name = upper(n);
        m.path = std::string(name_or_path);
    }
    m.effect = e;
    for (const auto& t : e->techniques)
        for (const auto& p : t.passes)
            if (p.states.size() > kMaxPassStates) {
                if (err) *err = m.name + ": pass " + p.name + " has more than 32767 states";
                return std::nullopt;
            }

    // Metadata.
    std::string phase_string;
    for (const auto& p : e->parameters) {
        const std::string& pn = p.type.name;
        auto word = [&]() -> uint32_t { return p.value.words.empty() ? 0u : p.value.words[0]; };
        if (pn == "_ALAMO_RENDER_PHASE") m.meta.render_phase = string_param(*e, p);
        else if (pn == "_ALAMO_VERTEX_PROC") m.meta.vertex_proc = string_param(*e, p);
        else if (pn == "_ALAMO_VERTEX_TYPE") m.meta.vertex_type = string_param(*e, p);
        else if (pn == "_ALAMO_TANGENT_SPACE") m.meta.tangent_space = word() != 0;
        else if (pn == "_ALAMO_SHADOW_VOLUME") m.meta.shadow_volume = word() != 0;
        else if (pn == "_ALAMO_Z_SORT") m.meta.z_sort = word() != 0;
        else if (pn == "_ALAMO_BONES_PER_VERTEX") m.meta.bones_per_vertex = static_cast<int>(word());
        else if (pn == "RenderPhase" && p.type.type == fx::ParamType::String) phase_string = string_param(*e, p);
    }
    if (options_.phase_override) m.phase = *options_.phase_override;
    else m.phase = render_phase_from_name(!m.meta.render_phase.empty() ? m.meta.render_phase : phase_string);
    m.layout = vertex_layout_from_name(m.meta.vertex_type.c_str());
    if (m.layout == VertexLayout::Unknown) {
        ProgramLookup pl = find_program(m.name);
        if (pl.status == ProgramStatus::Ok) m.layout = pl.program.vertex_layout;
    }

    // Technique.
    m.technique = pick_best_technique(*e);
    if (m.technique < 0) {
        if (err) *err = m.name + ": no technique";
        technique_failed_ = true;
        return std::nullopt;
    }
    m.lod = technique_lod_from_string(e->techniqueLod(e->techniques[static_cast<size_t>(m.technique)]),
                                      e->techniques[static_cast<size_t>(m.technique)].name);
    if (options_.ff_fallback_for_untranslated && m.lod != TechniqueLod::FixedFunction && m.lod != TechniqueLod::FF &&
        find_program(m.name).status != ProgramStatus::Ok) {
        for (size_t i = 0; i < e->techniques.size(); ++i) {
            const TechniqueLod l = technique_lod_from_string(e->techniqueLod(e->techniques[i]), e->techniques[i].name);
            if (l == TechniqueLod::FixedFunction || l == TechniqueLod::FF) {
                m.technique = static_cast<int>(i);
                m.lod = l;
                m.used_ff_fallback = true;
                break;
            }
        }
    }
    const DeviceState& base = phase_baseline(m.phase);
    auto resolve_technique = [&](int t) {
        std::vector<ResolvedPass> out;
        const fx::Technique& tq = e->techniques[static_cast<size_t>(t)];
        for (size_t p = 0; p < tq.passes.size(); ++p)
            out.push_back(resolve_pass(*e, t, static_cast<int>(p), base, options_, m.name, m.layout));
        return out;
    };
    auto drawable = [](const std::vector<ResolvedPass>& ps) {
        return std::all_of(ps.begin(), ps.end(), [](const ResolvedPass& p) { return pass_has_program(p); });
    };
    std::vector<ResolvedPass> passes = resolve_technique(m.technique);
    if (!drawable(passes)) {
        // [port] A technique with a shader pass the port has no program for (vs_3_0 / ps_3_0 in mods) is
        // skipped like one that fails ValidateTechnique in the original (RENDER.md 6): the next technique
        // by LOD, then file order, whose passes all have a program (max_viewport never). None: keep the
        // best one (its submeshes draw unlit). docs/MODS.md, shader section.
        std::vector<int> order;
        for (size_t i = 0; i < e->techniques.size(); ++i)
            if (static_cast<int>(i) != m.technique &&
                technique_lod_from_string(e->techniqueLod(e->techniques[i]), e->techniques[i].name) != TechniqueLod::MaxViewport)
                order.push_back(static_cast<int>(i));
        auto lod_of = [&](int i) {
            return static_cast<int>(technique_lod_from_string(e->techniqueLod(e->techniques[static_cast<size_t>(i)]),
                                                              e->techniques[static_cast<size_t>(i)].name));
        };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return lod_of(a) < lod_of(b); });
        for (const int t : order) {
            std::vector<ResolvedPass> alt = resolve_technique(t);
            if (!drawable(alt)) continue;
            m.skipped_technique = e->techniques[static_cast<size_t>(m.technique)].name;
            m.skipped_technique_index = m.technique;
            for (const ResolvedPass& rp : passes)
                if (!pass_has_program(rp)) {
                    m.skipped_pass = rp.pass_index;
                    break;
                }
            m.technique = t;
            m.lod = static_cast<TechniqueLod>(lod_of(t));
            passes = std::move(alt);
            m.warnings.push_back("technique " + m.skipped_technique + " has a shader pass without a program; using " +
                                 e->techniques[static_cast<size_t>(t)].name);
            break;
        }
    }
    m.technique_name = e->techniques[static_cast<size_t>(m.technique)].name;
    for (ResolvedPass& rp : passes) {
        for (const auto& u : rp.report.unknown) m.warnings.push_back(rp.name + ": unknown state " + u);
        if (rp.ff && !rp.ff->unsupported.empty()) m.warnings.push_back(rp.name + ": FF " + rp.ff->unsupported);
        m.passes.push_back(std::move(rp));
    }

    // Parameter bindings.
    for (const auto& p : e->parameters) {
        ParamBinding b;
        b.name = p.type.name;
        b.semantic = p.type.semantic;
        b.type = static_cast<uint32_t>(p.type.type);
        b.cls = static_cast<uint32_t>(p.type.cls);
        b.rows = p.type.rows;
        b.columns = p.type.columns;
        b.elements = p.type.elements;
        b.engine = is_engine_parameter(p);
        b.texture = p.type.type >= fx::ParamType::Texture && p.type.type <= fx::ParamType::TextureCube;
        b.sampler = p.type.type >= fx::ParamType::Sampler && p.type.type <= fx::ParamType::SamplerCube;
        b.numeric = param_float_count(p) > 0;
        m.params.push_back(std::move(b));
    }
    m.generic_layout = build_generic_layout(*e);
    if (m.generic_layout.size > 512) m.warnings.push_back("generic material record exceeds 512 bytes");
    return m;
}

bool ResolvedPass::blend_enabled(const ExprInputs& in) const {
    if (!fade_dynamic) return pso[0].blend_enable != 0;
    if (!fade_expr) return select(in.light_scale.w).blend_enable != 0;
    std::array<float4, 1> r{};
    fade_expr->run(in, r);
    return r[0].x != 0.0f;
}

const PsoKey& ResolvedPass::select_exact(const ExprInputs& in) const {
    if (!fade_dynamic) return pso[0];
    const uint8_t want = blend_enabled(in) ? 1 : 0;
    return pso[1].blend_enable == want ? pso[1] : pso[0];
}

// ------------------------------------------------------------------ D.3 default effect
namespace {

fx::State int_state(uint32_t operation, uint32_t value, uint32_t index = 0) {
    fx::State s;
    s.operation = operation;
    s.index = index;
    s.type.type = fx::ParamType::Int;
    s.type.cls = fx::ParamClass::Scalar;
    s.type.rows = s.type.columns = 1;
    s.value.words = {value};
    return s;
}
fx::State tss_state(uint32_t stage, TSS t, uint32_t value) {
    return int_state(op::kFirstTss + static_cast<uint32_t>(t), value, stage);
}
fx::State green_state(uint32_t operation) {
    fx::State s;
    s.operation = operation;
    s.type.type = fx::ParamType::Float;
    s.type.cls = fx::ParamClass::Vector;
    s.type.rows = 1;
    s.type.columns = 4;
    for (float f : {0.0f, 1.0f, 0.0f, 1.0f}) s.value.words.push_back(std::bit_cast<uint32_t>(f));
    return s;
}

// The `TNoShader` technique of the original's embedded default effect source.
std::shared_ptr<fx::Effect> make_default_fx() {
    auto e = std::make_shared<fx::Effect>();
    fx::Technique t;
    t.name = "TNoShader";
    fx::Pass p;
    p.name = "p0";
    p.states = {green_state(op::kMaterialAmbient), green_state(op::kMaterialDiffuse), green_state(op::kMaterialSpecular),
                green_state(op::kMaterialEmissive),
                tss_state(0, TSS::ColorOp, d3d::kSelectArg1), tss_state(0, TSS::ColorArg1, d3d::kTaDiffuse),
                tss_state(0, TSS::AlphaOp, d3d::kSelectArg1), tss_state(0, TSS::AlphaArg1, d3d::kTaDiffuse),
                tss_state(1, TSS::ColorOp, d3d::kDisable), tss_state(1, TSS::AlphaOp, d3d::kDisable),
                int_state(static_cast<uint32_t>(RS::AlphaBlendEnable), 0),
                int_state(static_cast<uint32_t>(RS::SrcBlend), d3d::kOne),
                int_state(static_cast<uint32_t>(RS::DestBlend), d3d::kZero)};
    t.passes.push_back(std::move(p));
    e->techniques.push_back(std::move(t));
    return e;
}

}  // namespace

const MaterialEffect& EffectLibrary::default_effect() {
    if (default_) return *default_;
    std::string err;
    std::optional<MaterialEffect> m;
    try {
        m = load_parsed(kDefaultEffectName, make_default_fx(), &err);
    } catch (const std::exception&) {
        m.reset();
    }
    if (!m) m.emplace();  // cannot happen with the fixed source; keep the contract (non-null)
    m->name = kDefaultEffectName;
    m->path.clear();
    m->phase = RenderPhase::Opaque;
    m->layout = VertexLayout::Unknown;  // no _ALAMO_VERTEX_TYPE: the mesh keeps its ALO format
    m->lod = TechniqueLod::FixedFunction;
    m->fallback = EffectFallback::BuiltinDefault;
    for (auto& p : m->passes) p.samplers.clear();  // stage 0 reads DIFFUSE only: no texture
    m->warnings.clear();
    default_ = std::move(m);
    return *default_;
}

MaterialEffect EffectLibrary::load_or_default(std::string_view effect, std::string* err) {
    std::string e;
    technique_failed_ = false;
    if (auto m = load(effect, &e)) return std::move(*m);
    const bool no_technique = technique_failed_;
    technique_failed_ = false;
    // A .fx source without a .fxo counts as existing: the original compiles it, and when that compile
    // fails it re-creates the green effect under the effect's name (D.3), which is what the port draws.
    std::optional<std::string> found = find_effect_path(effect);
    if (!found) found = find_effect_source(effect);
    const bool exists = found.has_value();
    if (!exists && !no_technique && !options_.missing_effect_remap.empty()) {
        // Opt-in compatibility (not original behaviour): a missing name mapped to a shipped effect.
        const std::string base = effect_base_name(effect);
        auto it = options_.missing_effect_remap.find(base);
        if (it != options_.missing_effect_remap.end() && effect_base_name(it->second) != base) {
            std::string re;
            if (auto m = load(it->second, &re)) {
                technique_failed_ = false;
                m->remapped_from = std::string(effect);
                if (err) err->clear();
                return std::move(*m);
            }
            technique_failed_ = false;  // the mapped effect failed too: faithful green default below
        }
    }
    MaterialEffect d = default_effect();
    d.requested = std::string(effect);
    if (exists && no_technique) {
        // D.3: technique selection failed: the object is deleted and the shared default effect
        // object is returned (identity = kDefaultEffectName, rank 0); the error is still reported.
        if (err) *err = e;
        d.fallback = EffectFallback::Failed;
    } else if (exists) {
        // D.3: a file that fails to load is re-created from the same green source under its own name.
        if (err) *err = e;
        std::string_view n = effect;
        const size_t slash = n.find_last_of("\\/");
        if (slash != std::string_view::npos) n = n.substr(slash + 1);
        const size_t dot = n.find_last_of('.');
        if (dot != std::string_view::npos) n = n.substr(0, dot);
        d.name = upper(n);
        d.path = *found;
        d.fallback = EffectFallback::Failed;
    } else {
        if (err) err->clear();  // the original logs nothing for a missing file
        d.fallback = EffectFallback::Missing;
    }
    return d;
}

// ------------------------------------------------------------------ D.1 creation order

const std::vector<std::string>& engine_effect_load_sequence() {
    static const std::vector<std::string> kSeq = {
        kDefaultEffectName, "GLOBAL", "ALAMOENGINE",
        // device init, code order
        "PRIMOPAQUE", "PRIMADDITIVE", "PRIMALPHA", "PRIMMODULATE", "PRIMDEPTHSPRITEADDITIVE", "PRIMDEPTHSPRITEALPHA",
        "PRIMDEPTHSPRITEMODULATE", "STENCILDARKEN", "STENCILDARKENTOALPHA", "STENCILDARKENFINALBLUR", "PRIMDIFFUSEALPHA",
        "PRIMHEAT", "PRIMPARTICLEBUMPALPHA", "PRIMDECALBUMPALPHA", "PRIMALPHASCANLINES", "SCENEBLOOM", "SCENEHEAT",
        "FRAMEEFFECTFAKEMOTIONBLUR"};
    return kSeq;
}

EffectLoadOrder::EffectLoadOrder() {
    for (const auto& n : engine_effect_load_sequence()) add(n);
}

EffectLoadOrder::EffectLoadOrder(std::span<const ShaderFileEntry> shader_files) : EffectLoadOrder() {
    struct Key {
        int pattern;      // 0 = *.fx, 1 = *.fxo
        int dir;          // dir_order * 2 + (Terrain ? 1 : 0)
        int archived;     // 0 = loose, 1 = MEG entry
        uint32_t crc;     // MEG table order
        std::string path; // normalised; loose name order and final tie-break
        std::string base;
    };
    std::vector<Key> keys;
    for (const auto& f : shader_files) {
        std::string p = alamo::normalize_name(f.path);
        if (p.rfind(".\\", 0) == 0) p.erase(0, 2);
        constexpr std::string_view kRoot = "DATA\\ART\\SHADERS\\";
        std::string_view rest = p;
        if (rest.substr(0, kRoot.size()) != kRoot) continue;
        rest.remove_prefix(kRoot.size());
        int sub = 0;
        if (rest.substr(0, 8) == "TERRAIN\\") {
            sub = 1;
            rest.remove_prefix(8);
        }
        if (rest.find('\\') != std::string_view::npos) continue;  // Engine\ and other folders are not searched
        int pattern = -1;
        if (rest.size() > 3 && rest.substr(rest.size() - 3) == ".FX") pattern = 0;
        else if (rest.size() > 4 && rest.substr(rest.size() - 4) == ".FXO") pattern = 1;
        if (pattern < 0) continue;
        Key k;
        k.pattern = pattern;
        k.dir = f.dir_order * 2 + sub;
        k.archived = f.loose ? 0 : 1;
        k.crc = f.loose ? 0u : alamo::name_crc(p);  // MEG tables: ascending CRC-32 of the upper-case path
        k.path = p;
        k.base = effect_base_name(p);
        keys.push_back(std::move(k));
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.pattern != b.pattern) return a.pattern < b.pattern;
        if (a.dir != b.dir) return a.dir < b.dir;
        if (a.archived != b.archived) return a.archived < b.archived;
        if (a.archived && a.crc != b.crc) return a.crc < b.crc;
        return a.path < b.path;
    });
    for (auto& k : keys) add(std::move(k.base));
}

void EffectLoadOrder::add(std::string name) {
    if (index_.count(name)) return;
    index_.emplace(name, static_cast<uint32_t>(names_.size()));
    names_.push_back(std::move(name));
}

uint32_t EffectLoadOrder::rank(std::string_view effect) const {
    auto it = index_.find(effect_base_name(effect));
    return it == index_.end() ? static_cast<uint32_t>(names_.size()) : it->second;
}

uint32_t EffectLoadOrder::rank(const MaterialEffect& e) const {
    if (e.fallback == EffectFallback::BuiltinDefault || e.fallback == EffectFallback::Missing) return 0;
    return rank(e.name);
}

}  // namespace alamo::materials
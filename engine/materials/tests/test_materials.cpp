// alamo_materials tests (no framework; non-zero exit on failure).
// Data-driven part: ALAMO_FOC_DATA = folder containing Data\ (FoC "corruption" dir); MEGs are
// mounted through alamo_vfs when the test is built with it.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/materials/constants.hpp"
#include "alamo/materials/d3d9_state.hpp"
#include "alamo/materials/effect_audit.hpp"
#include "alamo/materials/effect_loader.hpp"
#include "alamo/materials/fixed_function.hpp"
#include "alamo/materials/fx_source.hpp"
#include "alamo/materials/fxlc.hpp"
#include "alamo/materials/params.hpp"
#include "alamo/materials/programs.hpp"
#include "alamo/materials/pso.hpp"
#include "alamo/materials/register_file.hpp"
#include "fx2_reader.hpp"
#include "fx_compile.hpp"
#ifdef ALAMO_MATERIALS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::materials;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

// ------------------------------------------------------------------ hand-built effect helpers
fx::State st_const(uint32_t op, uint32_t value, uint32_t index = 0) {
    fx::State s;
    s.operation = op;
    s.index = index;
    s.type.type = fx::ParamType::Int;
    s.type.cls = fx::ParamClass::Scalar;
    s.type.rows = s.type.columns = 1;
    s.value.words = {value};
    return s;
}
fx::State rs(RS r, uint32_t v) { return st_const(static_cast<uint32_t>(r), v); }
fx::State tss(int stage, TSS t, uint32_t v) {
    return st_const(op::kFirstTss + static_cast<uint32_t>(t), v, static_cast<uint32_t>(stage));
}
fx::State ss(int sampler, SS t, uint32_t v) {
    return st_const(op::kFirstSampler + static_cast<uint32_t>(t), v, static_cast<uint32_t>(sampler));
}
fx::State st_expr(uint32_t op, std::vector<uint8_t> blob) {
    fx::State s = st_const(op, 0);
    s.kind = fx::State::Kind::Expression;
    s.blob = std::move(blob);
    return s;
}

fx::Parameter float_param(const char* name, const char* semantic, std::vector<float> v, uint32_t rows,
                          uint32_t cols, uint32_t elements = 0) {
    fx::Parameter p;
    p.type.type = fx::ParamType::Float;
    p.type.cls = rows > 1 ? fx::ParamClass::MatrixRows : (cols > 1 ? fx::ParamClass::Vector : fx::ParamClass::Scalar);
    p.type.name = name;
    p.type.semantic = semantic;
    p.type.rows = rows;
    p.type.columns = cols;
    p.type.elements = elements;
    for (float f : v) p.value.words.push_back(std::bit_cast<uint32_t>(f));
    return p;
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void comment(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p = payload;
    while (p.size() % 4) p.push_back(0);
    put32(out, 0xFFFEu | (static_cast<uint32_t>(p.size() / 4) << 16));
    out.insert(out.end(), p.begin(), p.end());
}

// FXLC stream with CTAB { float4 m_lightScale : c0 }, the given literals and code words (after
// the FXLC fourcc). Default: expr0.x = (m_lightScale.w < 1) ? 1 : 0 (the opaque-mesh fade switch).
std::vector<uint8_t> make_fxlc(const std::vector<uint32_t>& code_words, const std::vector<double>& lits) {
    std::vector<uint8_t> out;
    put32(out, 0x46580200u);
    // CTAB: one float4 constant m_lightScale at c0
    std::vector<uint8_t> ct;
    put32(ct, 0x42415443u);  // 'CTAB'
    const size_t base = ct.size();
    // header (28 bytes) + 1 constant info (20) + type info (16) + name
    put32(ct, 28);               // size
    put32(ct, 0);                // creator
    put32(ct, 0x46580200u);      // version
    put32(ct, 1);                // constants
    put32(ct, 28);               // constant info offset
    put32(ct, 0);                // flags
    put32(ct, 0);                // target
    const uint32_t type_off = 28 + 20, name_off = type_off + 16;
    put32(ct, name_off);
    put16(ct, 2);  // float4 set
    put16(ct, 0);  // reg
    put16(ct, 1);  // count
    put16(ct, 0);
    put32(ct, type_off);
    put32(ct, 0);
    put16(ct, 1);  // class vector
    put16(ct, 3);  // float
    put16(ct, 1);  // rows
    put16(ct, 4);  // columns
    put16(ct, 0);  // elements
    put16(ct, 0);
    put32(ct, 0);
    for (char c : std::string("m_lightScale")) ct.push_back(static_cast<uint8_t>(c));
    ct.push_back(0);
    (void)base;
    comment(out, ct);
    std::vector<uint8_t> clit;
    put32(clit, 0x54494C43u);
    put32(clit, static_cast<uint32_t>(lits.size()));
    for (double d : lits) {
        uint64_t ob;
        std::memcpy(&ob, &d, 8);
        put32(clit, static_cast<uint32_t>(ob));
        put32(clit, static_cast<uint32_t>(ob >> 32));
    }
    comment(out, clit);
    std::vector<uint8_t> code;
    put32(code, 0x434C5846u);
    for (uint32_t w : code_words) put32(code, w);
    comment(out, code);
    put32(out, 0x0000FFFFu);
    return out;
}

// lt out0.x, c0.w, literal 1.0 (output offset configurable for the bounds tests)
std::vector<uint8_t> make_fade_fxlc(uint32_t out_table = 4, uint32_t out_offset = 0, uint32_t in_offset = 3) {
    return make_fxlc({1, (0x202u << 20) | 1u, 2, 0, 2, in_offset, 0, 1, 0, 0, out_table, out_offset}, {1.0});
}

fx::Effect make_effect(std::vector<fx::State> states, std::vector<fx::Parameter> params = {}) {
    fx::Effect e;
    e.parameters = std::move(params);
    fx::Technique t;
    t.name = "t0";
    fx::Pass p;
    p.name = "p0";
    p.states = std::move(states);
    t.passes.push_back(std::move(p));
    e.techniques.push_back(std::move(t));
    return e;
}

uint64_t ref_fnv(const void* d, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<const uint8_t*>(d)[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// ------------------------------------------------------------------ unit tests
void test_layouts() {
    CHECK(sizeof(FrameCB) == 992);
    CHECK(sizeof(DrawData) == 80);
    CHECK(sizeof(MeshMaterial) == 96);
    CHECK(sizeof(BoneMatrix) == 48);
    CHECK(sizeof(FfDrawData) == 432);
    CHECK(sizeof(PushConstants) == 16);
    CHECK(sizeof(PsoKey) == 40);
    CHECK(sizeof(SamplerKey) == 20);
    CHECK(offsetof(FrameCB, sph_fill) == 800);
    CHECK(offsetof(DrawData, material_index) == 64);
    CHECK(offsetof(MeshMaterial, tex) == 80);
    CHECK(pack_texture(5, kSamplerClampPoint) == (5u | (3u << 24)));
    float m[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    float4 rows[4];
    matrix_rows_from_d3dx(m, rows, 4);
    CHECK(rows[0].x == 1 && rows[0].y == 5 && rows[0].z == 9 && rows[0].w == 13);
    CHECK(rows[3].x == 4 && rows[3].w == 16);
}

void test_defaults_and_keys() {
    // Plain D3D9 defaults (D3DCULL_CCW) cull the outward = counter-clockwise faces.
    CHECK(resolve_pso(d3d9_defaults()).cull == CullMode::Front);
    DeviceState d = engine_device_defaults();
    PsoKey k = resolve_pso(d);
    CHECK(k.blend_enable == 0);
    CHECK(k.depth_test == 1 && k.depth_write == 1);
    CHECK(k.depth_compare == CompareOp::LessOrEqual);
    CHECK(k.cull == CullMode::Back);
    CHECK(k.color_write_mask == 0xF);
    CHECK(k.polygon_mode == PolygonMode::Fill);
    CHECK(k.stencil_enable == 0);
    CHECK(k.alpha_test == CompareOp::Always);
    // hashing: FNV-1a over the bytes, independent of instance and stable
    PsoKey k2 = k;
    CHECK(hash(k) == hash(k2));
    CHECK(hash(k) == ref_fnv(&k, sizeof k));
    CHECK(std::hash<PsoKey>{}(k) == static_cast<size_t>(hash(k)));
    k2.cull = CullMode::None;
    CHECK(hash(k) != hash(k2));
    std::printf("  default PsoKey hash = 0x%016llx\n", static_cast<unsigned long long>(hash(k)));
    CHECK(hash(k) == 0xd9545a81a269dd6cull);  // golden: changes only if the key layout changes

    // enum mapping
    CHECK(to_blend_factor(d3d::kSrcAlpha) == BlendFactor::SrcAlpha);
    CHECK(to_blend_factor(d3d::kInvSrcAlpha) == BlendFactor::OneMinusSrcAlpha);
    CHECK(to_blend_factor(d3d::kDestColor) == BlendFactor::DstColor);
    CHECK(to_compare(d3d::kGreater) == CompareOp::Greater);
    CHECK(to_stencil_op(d3d::kIncr) == StencilOp::IncrementAndWrap);
    CHECK(to_stencil_op(d3d::kIncrSat) == StencilOp::IncrementAndClamp);
    CHECK(to_cull(d3d::kCullCW) == CullMode::Back);
    CHECK(to_cull(d3d::kCullCCW) == CullMode::Front);
    CHECK(to_cull(d3d::kCullNone) == CullMode::None);
    CHECK(kFrontFaceCounterClockwise);
    CHECK(static_cast<int>(RS::Count) == 103);
    CHECK(std::string(fx::stateInfo(static_cast<uint32_t>(RS::TwoSidedStencilMode)).name) == "TWOSIDEDSTENCILMODE");
    CHECK(std::string(fx::stateInfo(static_cast<uint32_t>(RS::Lighting)).name) == "LIGHTING");
    CHECK(std::string(fx::stateInfo(op::kFirstTss + static_cast<uint32_t>(TSS::TexCoordIndex)).name) == "TEXCOORDINDEX");
    CHECK(std::string(fx::stateInfo(op::kFirstSampler + static_cast<uint32_t>(SS::MipFilter)).name) == "MipFilter");
    CHECK(std::string(fx::stateInfo(op::kSampler).name) == "Sampler");
    CHECK(std::string(fx::stateInfo(op::kTransformTexture0).name) == "TEXTURE0");
    CHECK(std::string(fx::stateInfo(op::kMaterialPower).name) == "MaterialPower");
}

void test_phase_material_resolution() {
    // Phase pass 0 (PhaseShadow-like) then the MeshShadowVolume t0_zfail pass.
    fx::Effect phase = make_effect({rs(RS::ZFunc, d3d::kLess), rs(RS::ColorWriteEnable, 0)});
    DeviceState base = d3d9_defaults();
    apply_pass(phase, phase.techniques[0].passes[0], StateOrigin::Phase, base);
    CHECK(base.slot(RS::ZFunc).origin == StateOrigin::Phase);

    fx::Effect sv = make_effect({rs(RS::ColorWriteEnable, 0), rs(RS::ZFunc, d3d::kLess), rs(RS::ZWriteEnable, 0),
                                 rs(RS::StencilEnable, 1), rs(RS::StencilRef, 1), rs(RS::StencilMask, 63),
                                 rs(RS::StencilWriteMask, 63), rs(RS::CullMode, d3d::kCullNone),
                                 rs(RS::TwoSidedStencilMode, 1), rs(RS::StencilFunc, d3d::kAlways),
                                 rs(RS::StencilPass, d3d::kKeep), rs(RS::StencilZFail, d3d::kIncr),
                                 rs(RS::StencilFail, d3d::kKeep), rs(RS::CcwStencilFunc, d3d::kAlways),
                                 rs(RS::CcwStencilPass, d3d::kKeep), rs(RS::CcwStencilZFail, d3d::kDecr),
                                 rs(RS::CcwStencilFail, d3d::kKeep)});
    LoadOptions opt;
    ResolvedPass rp = resolve_pass(sv, 0, 0, base, opt, "MeshShadowVolume", VertexLayout::N);
    const PsoKey& k = rp.pso[0];
    CHECK(k.color_write_mask == 0);
    CHECK(k.depth_compare == CompareOp::Less);
    CHECK(k.depth_write == 0);
    CHECK(k.cull == CullMode::None);
    CHECK(k.stencil_enable == 1);
    // z-fail: clockwise (D3D STENCIL*) faces are back faces -> Vulkan back increments.
    CHECK(k.back.depth_fail_op == StencilOp::IncrementAndWrap);
    CHECK(k.front.depth_fail_op == StencilOp::DecrementAndWrap);
    CHECK(k.front.compare == CompareOp::Always && k.back.pass_op == StencilOp::Keep);
    CHECK(rp.dynamic.stencil_ref == 1 && rp.dynamic.stencil_read_mask == 63 && rp.dynamic.stencil_write_mask == 63);
    CHECK(!rp.fade_dynamic);
    CHECK(rp.pso[0] == rp.pso[1]);
    CHECK(k.vertex_layout == VertexLayout::N);
    CHECK(rp.fixed_function);  // hand-built pass has no shader states

    // One-sided stencil copies front to back.
    fx::Effect one = make_effect({rs(RS::StencilEnable, 1), rs(RS::StencilFunc, d3d::kNotEqual)});
    ResolvedPass r1 = resolve_pass(one, 0, 0, d3d9_defaults(), opt, "X", VertexLayout::Unknown);
    CHECK(r1.pso[0].back.compare == CompareOp::NotEqual);
}

void test_fade_and_alpha_test() {
    // Tree DX8 pass: ATEST GREATER 128 + dynamic fade.
    std::vector<fx::Parameter> params = {float_param("m_lightScale", "LIGHT_SCALE", {1, 1, 1, 1}, 1, 4)};
    fx::Effect tree = make_effect({rs(RS::ZWriteEnable, 1), rs(RS::ZFunc, d3d::kLessEqual),
                                   rs(RS::DestBlend, d3d::kInvSrcAlpha), rs(RS::SrcBlend, d3d::kSrcAlpha),
                                   rs(RS::AlphaTestEnable, 1), rs(RS::AlphaRef, 128),
                                   rs(RS::AlphaFunc, d3d::kGreater),
                                   st_expr(static_cast<uint32_t>(RS::AlphaBlendEnable), make_fade_fxlc())},
                                  params);
    LoadOptions opt;
    ResolvedPass rp = resolve_pass(tree, 0, 0, d3d9_defaults(), opt, "Tree", VertexLayout::NU2);
    CHECK(rp.fade_dynamic);
    CHECK(rp.pso[0].blend_enable == 0);
    CHECK(rp.pso[1].blend_enable == 1);
    CHECK(rp.pso[1].src_color == BlendFactor::SrcAlpha && rp.pso[1].dst_color == BlendFactor::OneMinusSrcAlpha);
    CHECK(&rp.select(1.0f) == &rp.pso[0]);
    CHECK(&rp.select(0.25f) == &rp.pso[1]);
    CHECK(rp.push.alpha_func == static_cast<uint32_t>(CompareOp::Greater));
    CHECK(rp.push.alpha_ref == 128);
    CHECK(rp.pso[0].alpha_test == CompareOp::Greater);

    // FXLC evaluator directly
    std::vector<uint8_t> blob = make_fade_fxlc();
    Expression ex;
    std::string err;
    CHECK(parse_expression(blob.data(), blob.size(), ex, &err));
    CHECK(ex.inputs.size() == 1 && ex.inputs[0].name == "m_lightScale");
    CHECK(ex.literals.size() == 1 && ex.literals[0] == 1.0);
    CHECK(expression_supported(ex));
    ParamStore store;
    store.set_semantic("LIGHT_SCALE", {1, 1, 1, 0.5f});
    std::vector<float4> out(1);
    CHECK(evaluate(ex, tree, store.lookup(), out));
    CHECK(out[0].x == 1.0f);
    store.set_semantic("LIGHT_SCALE", {1, 1, 1, 1});
    CHECK(evaluate(ex, tree, store.lookup(), out));
    CHECK(out[0].x == 0.0f);
}

void test_samplers() {
    fx::Parameter tex;
    tex.type.type = fx::ParamType::Texture;
    tex.type.cls = fx::ParamClass::Object;
    tex.type.name = "BaseTexture";
    fx::Parameter smp;
    smp.type.type = fx::ParamType::Sampler2D;
    smp.type.cls = fx::ParamClass::Object;
    smp.type.name = "BaseSampler";
    fx::State t;
    t.operation = op::kTexture;
    t.kind = fx::State::Kind::ParameterRef;
    t.refName = "BaseTexture";
    smp.value.samplers.push_back({t, ss(0, SS::MinFilter, d3d::kFilterPoint), ss(0, SS::MagFilter, d3d::kFilterPoint),
                                  ss(0, SS::AddressU, d3d::kClamp)});
    fx::Effect e = make_effect({}, {tex, smp});
    DeviceState st = d3d9_defaults();
    apply_sampler_param(e, "BaseSampler", 1, StateOrigin::Constant, st);
    CHECK(st.stage[1].texture_param == "BaseTexture");
    SamplerKey def;
    def.max_anisotropy = 8;
    SamplerKey k = resolve_sampler(st, 1, def);
    CHECK(k.min == Filter::Nearest && k.mag == Filter::Nearest);
    CHECK(k.u == AddressMode::ClampToEdge && k.v == AddressMode::Repeat);
    CHECK(k.mip == MipmapMode::Linear && k.mip_enable == 1);  // MipFilter untouched: engine default
    CHECK(k.max_anisotropy == 1);
    CHECK(k.from_effect == 1);
    SamplerKey k0 = resolve_sampler(st, 0, def);  // untouched sampler: engine defaults
    CHECK(k0 == def);
    CHECK(hash(k0) == hash(def));
}

void test_fixed_function() {
    // MeshAlpha FIXEDFUNCTION pass.
    std::vector<fx::Parameter> params = {float_param("m_lightScale", "LIGHT_SCALE", {1, 1, 1, 1}, 1, 4),
                                         float_param("Diffuse", "", {1, 0.5f, 0.25f, 0.75f}, 1, 4)};
    fx::State md;  // MaterialDiffuse = constant (expression path covered by the data test)
    md.operation = op::kMaterialDiffuse;
    md.type.type = fx::ParamType::Float;
    md.value.words = {std::bit_cast<uint32_t>(0.5f), std::bit_cast<uint32_t>(0.25f), std::bit_cast<uint32_t>(1.0f),
                      std::bit_cast<uint32_t>(0.5f)};
    fx::State vs = st_const(op::kVertexShader, 0);
    vs.type.type = fx::ParamType::VertexShader;
    vs.value.words.clear();
    vs.value.objects = {77};  // object 77 has no data: NULL
    fx::Effect e = make_effect({rs(RS::ZWriteEnable, 0), rs(RS::AlphaBlendEnable, 1),
                                rs(RS::DestBlend, d3d::kInvSrcAlpha), rs(RS::SrcBlend, d3d::kSrcAlpha),
                                rs(RS::Lighting, 1), tss(0, TSS::ColorOp, d3d::kModulate2x),
                                tss(0, TSS::ColorArg1, d3d::kTaTexture), tss(0, TSS::ColorArg2, d3d::kTaDiffuse),
                                tss(0, TSS::AlphaOp, d3d::kModulate), tss(0, TSS::AlphaArg1, d3d::kTaTexture),
                                tss(0, TSS::AlphaArg2, d3d::kTaDiffuse), tss(1, TSS::ColorOp, d3d::kDisable),
                                tss(1, TSS::AlphaOp, d3d::kDisable), vs, md},
                               params);
    e.objects[77] = fx::Object{};
    LoadOptions opt;
    ResolvedPass rp = resolve_pass(e, 0, 0, d3d9_defaults(), opt, "MeshAlpha", VertexLayout::NU2);
    CHECK(rp.fixed_function);
    CHECK(rp.program_status == ProgramStatus::Ok);
    CHECK(rp.program.family == ProgramFamily::FixedFunction);
    CHECK(rp.program.vs && std::string(rp.program.vs->name) == "ff_vs_n");
    CHECK(rp.ff.has_value());
    if (rp.ff) {
        CHECK(rp.ff->active_stages == 1);
        CHECK(rp.ff->stage[0].color_op == d3d::kModulate2x);
        CHECK(rp.ff->stage[0].color_arg1 == d3d::kTaTexture);
        CHECK(rp.ff->lighting);
        FfDrawData d = build_ff_draw_data(*rp.ff, e, nullptr, false);
        CHECK(d.misc.x == 1);
        CHECK(d.tex.x == pack_texture(0, kSamplerWrapLinear) && (d.tex.x & 0x00FFFFFFu) == 0);
        CHECK(d.mat_diffuse.x == 0.5f && d.mat_diffuse.w == 0.5f);
        CHECK(d.texture_factor.x == 1.0f && d.texture_factor.w == 1.0f);  // default TFACTOR 0xFFFFFFFF
        CHECK(d.params.y == 1.0f);
        CHECK(d.tex_transform[0][0].x == 1.0f && d.tex_transform[0][1].y == 1.0f);
    }
    CHECK(rp.pso[0].blend_enable == 1 && rp.pso[0].depth_write == 0);
    CHECK(rp.pso[0].vs_id == find_shader_blob("ff_vs_n")->id);
}

void test_params() {
    std::vector<fx::Parameter> params = {
        float_param("Emissive", "", {0.1f, 0.2f, 0.3f}, 1, 3),
        float_param("Shininess", "", {32}, 1, 1),
        float_param("Colorization", "", {0, 1, 0, 1}, 1, 4),
        float_param("UVOffset", "", {0.5f, 0.25f, 0, 0}, 1, 4),
        float_param("m_world", "WORLD", std::vector<float>(16, 0.0f), 4, 4),
        float_param("BaseFFTM", "", std::vector<float>(16, 2.0f), 4, 4),
        float_param("Two", "", {1, 2}, 1, 2),
    };
    fx::Effect e = make_effect({}, params);
    GenericLayout L = build_generic_layout(e);
    CHECK(L.fields.size() == 6);  // m_world is engine supplied
    if (L.fields.size() == 6) {
        CHECK(L.fields[0].offset == 0);    // float3
        CHECK(L.fields[1].offset == 12);   // float packs into .w
        CHECK(L.fields[2].offset == 16);   // float4
        CHECK(L.fields[3].offset == 32);
        CHECK(L.fields[4].offset == 48 && L.fields[4].registers == 4);  // float4x4
        CHECK(L.fields[5].offset == 112);
    }
    CHECK(L.size == 128);
    std::vector<float> rec(L.size / 4);
    fill_generic_record(e, L, nullptr, rec);
    CHECK(rec[3] == 32.0f && rec[12] == 2.0f && rec[28] == 1.0f && rec[29] == 2.0f);

    MeshMaterial m = build_mesh_material(e, nullptr);
    CHECK(m.emissive.x == 0.1f && m.emissive.w == 32.0f);
    CHECK(m.colorization.y == 1.0f);
    CHECK(m.uv_offset_scroll.x == 0.5f && m.uv_offset_scroll.y == 0.25f);
    CHECK(m.diffuse.x == 1.0f);  // default when the effect has no Diffuse
    ParamStore store;
    store.set("shininess", {16});
    MeshMaterial m2 = build_mesh_material(e, store.lookup());
    CHECK(m2.emissive.w == 16.0f);

    MaterialTable table;
    const uint32_t a = table.intern(m), b = table.intern(m2), c = table.intern(m);
    CHECK(a == 0 && b == 1 && c == 0);
    CHECK(table.records().size() == 2 && table.requests() == 3);

    // pack_registers: float4x3 column-major palette entry -> 3 registers of 4 floats
    std::vector<float> m43 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};  // 4 rows x 3 cols
    std::vector<float4> regs(3);
    CHECK(pack_registers(m43, 3, 4, 3, 0, 3, regs) == 3);
    CHECK(regs[0].x == 1 && regs[0].y == 4 && regs[0].z == 7 && regs[0].w == 10);
    CHECK(regs[2].x == 3 && regs[2].w == 12);
    std::vector<float4> r2(2);
    CHECK(pack_registers(std::vector<float>{1, 2, 3, 4, 5, 6, 7, 8}, 1, 1, 4, 2, 8, r2) == 2);
    CHECK(r2[1].x == 5);
}

// Review fixes: phase-pass state indices, FXLC bounds, exception boundary, id namespaces, FF skinning.
void test_review_regressions() {
    // 1. A phase pass with an expression (index 5) and a vertex shader (index 3), then a 1-state
    //    material pass: no phase index may be dereferenced against the material pass.
    std::vector<fx::Parameter> params = {float_param("m_lightScale", "LIGHT_SCALE", {1, 1, 1, 1}, 1, 4)};
    fx::State vs;
    vs.operation = op::kVertexShader;
    vs.type.type = fx::ParamType::VertexShader;
    vs.kind = fx::State::Kind::Shader;
    vs.blob = {0x01, 0x01, 0xFE, 0xFF, 0xFF, 0xFF, 0x00, 0x00};  // vs_1_1 + END
    fx::Effect phase = make_effect({rs(RS::ZFunc, d3d::kLess), rs(RS::ZWriteEnable, 1), rs(RS::CullMode, d3d::kCullCCW),
                                    vs, rs(RS::StencilEnable, 0),
                                    st_expr(static_cast<uint32_t>(RS::AlphaBlendEnable), make_fade_fxlc()),
                                    st_expr(static_cast<uint32_t>(RS::TextureFactor), make_fade_fxlc())},
                                   params);
    DeviceState base = d3d9_defaults();
    apply_pass(phase, phase.techniques[0].passes[0], StateOrigin::Phase, base);
    CHECK(base.slot(RS::AlphaBlendEnable).source_state == 5);
    fx::Effect mat = make_effect({rs(RS::ZWriteEnable, 0)}, params);
    LoadOptions opt;
    ResolvedPass rp = resolve_pass(mat, 0, 0, base, opt, "MeshAlpha", VertexLayout::NU2);
    CHECK(!rp.state.refers_to_last_pass(rp.state.slot(RS::AlphaBlendEnable)));
    CHECK(rp.state.last_pass_vertex_shader() == -1);
    CHECK(!rp.fade_dynamic);
    CHECK(rp.vs_bytecode_hash == 0 && !rp.vs_plan);
    CHECK(rp.pso[0].depth_write == 0 && rp.pso[0].depth_compare == CompareOp::Less);
    if (rp.ff) CHECK(rp.ff->texture_factor.kind != FfValue::Kind::Expression);
    // The same expression in the material pass itself is still honoured.
    fx::Effect mat2 = make_effect({rs(RS::ZWriteEnable, 0), st_expr(static_cast<uint32_t>(RS::AlphaBlendEnable),
                                                                   make_fade_fxlc())},
                                  params);
    ResolvedPass rp2 = resolve_pass(mat2, 0, 0, base, opt, "X", VertexLayout::NU2);
    CHECK(rp2.fade_dynamic && rp2.pso[1].blend_enable == 1);

    // >32767 states: ignored beyond the limit and reported.
    fx::Effect big = make_effect(std::vector<fx::State>(kMaxPassStates + 1, rs(RS::ZWriteEnable, 0)));
    DeviceState bs = d3d9_defaults();
    ApplyReport brep;
    apply_pass(big, big.techniques[0].passes[0], StateOrigin::Constant, bs, &brep);
    CHECK(!brep.unknown.empty());

    // 2. FXLC register bounds: wrapping / huge offsets fail to parse instead of allocating.
    Expression ex;
    std::string err;
    std::vector<uint8_t> b1 = make_fade_fxlc(4, 0xFFFFFFF0u);
    CHECK(!parse_expression(b1.data(), b1.size(), ex, &err));
    std::vector<uint8_t> b2 = make_fade_fxlc(4, 0, 0xFFFFFFFFu);
    CHECK(!parse_expression(b2.data(), b2.size(), ex, &err));
    std::vector<uint8_t> b3 = make_fade_fxlc(7, 4 * kMaxFxlcRegisters);   // temp register 256
    CHECK(!parse_expression(b3.data(), b3.size(), ex, &err));
    std::vector<uint8_t> b4 = make_fade_fxlc(4, 4 * (kMaxFxlcRegisters - 1));  // last legal register
    CHECK(parse_expression(b4.data(), b4.size(), ex, &err) && ex.output_registers == kMaxFxlcRegisters);
    std::vector<uint8_t> b5 = make_fade_fxlc(2, 0);  // writing to the input table
    CHECK(!parse_expression(b5.data(), b5.size(), ex, &err));
    // Truncated stream at every length: never throws, never crashes.
    std::vector<uint8_t> good = make_fade_fxlc();
    for (size_t n = 0; n < good.size(); ++n) {
        Expression t;
        parse_expression(good.data(), n, t);
    }

    // 4. Indexed operand with a NaN index reads 0 (no out-of-range float -> int conversion).
    //    mov out0.x, c[lit0.x + 0].x  with lit0 = NaN
    std::vector<uint8_t> nan_blob =
        make_fxlc({1, (0x100u << 20) | 1u, 1, 1, 1, 0, 2, 0, 0, 4, 0}, {std::nan("")});
    Expression ne;
    CHECK(parse_expression(nan_blob.data(), nan_blob.size(), ne, &err));
    std::vector<float4> out(1, float4{9, 9, 9, 9});
    fx::Effect pe = make_effect({}, params);
    CHECK(evaluate(ne, pe, nullptr, out));
    CHECK(out[0].x == 0.0f);

    // 3. Load boundary: malformed bytes give an error, not an exception.
    struct NoFiles final : FileProvider {
        bool read(std::string_view, std::vector<uint8_t>&) const override { return false; }
        bool exists(std::string_view) const override { return false; }
        std::string origin(std::string_view) const override { return {}; }
    } nofiles;
    EffectLibrary lib(nofiles);
    std::vector<uint8_t> junk = {0x01, 0x09, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0x7F, 0, 0, 0, 0};
    bool threw = false;
    try {
        CHECK(!lib.load_from_memory("JUNK.fxo", junk, &err));
        CHECK(!err.empty());
        CHECK(!lib.load("Missing.fx", &err));
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);

    // 4. Program-id namespaces: hand-written ids have bit 31 clear.
    for (const auto& b : all_shader_blobs()) CHECK((b.id & kProgramIdTranslatedBit) == 0 && b.id != 0);

    // 5. Skinned layouts use the skinned FF vertex shader.
    CHECK(ff_inputs_for_layout(VertexLayout::RSkinNU2) == (kPermFfNormal | kPermFfSkinned));
    CHECK(std::string(ff_program(ff_inputs_for_layout(VertexLayout::RSkinNU2)).vs->name) == "ff_vs_n_skin");
    CHECK(std::string(ff_program(ff_inputs_for_layout(VertexLayout::RSkinNU2C)).vs->name) == "ff_vs_nc_skin");
    CHECK(std::string(ff_program(kPermFfSkinned).vs->name) == "ff_vs_n_skin");
    ResolvedPass rs_ff = resolve_pass(mat, 0, 0, d3d9_defaults(), opt, "SomeFfOnlyEffect", VertexLayout::RSkinNU2);
    CHECK(rs_ff.fixed_function && rs_ff.program.vs == find_shader_blob("ff_vs_n_skin"));
    // 7. Colour inputs state their VkFormat.
    const ProgramInterface* ffi = ff_program(kPermFfColor).iface;
    bool color_doc = false;
    for (const auto& in : ffi->inputs)
        if (in.location == kLocColor0)
            color_doc = std::string(in.format).find("B8G8R8A8_UNORM") != std::string::npos &&
                        std::string(in.format).find("R8G8B8A8_UNORM") != std::string::npos;
    CHECK(color_doc);
    // Attribute tables mirror alamo_assets vertex_layout.hpp rev 2.
    VertexLayoutDesc sk = vertex_layout_desc(VertexLayout::RSkinNU2C);
    CHECK(sk.stride == 64 && sk.attributes.size() == 6);
    for (const auto& a : sk.attributes) {
        if (a.location == kLocColor0) CHECK(a.offset == 60 && a.vk_format == vkfmt::kR8G8B8A8Unorm);
        if (a.location == kLocNormal) CHECK(a.offset == 12 && a.vk_format == vkfmt::kR32G32B32A32Sfloat);
        if (a.location == kLocTexcoord0) CHECK(a.offset == 28);
    }
    VertexLayoutDesc st = vertex_layout_desc(VertexLayout::NU2U3U3);
    CHECK(st.stride == 64);
    for (const auto& a : st.attributes) {
        if (a.location == kLocColor0) CHECK(a.offset == 56 && a.vk_format == vkfmt::kR8G8B8A8Unorm);
        if (a.location == kLocBinormal) CHECK(a.offset == 44);
    }
    VertexLayoutDesc pv = vertex_layout_desc(VertexLayout::PrimVert);
    CHECK(pv.stride == 24 && pv.attributes.size() == 3);
    for (const auto& a : pv.attributes)
        if (a.location == kLocColor0) CHECK(a.offset == 12 && a.vk_format == vkfmt::kB8G8R8A8Unorm);
    VertexLayoutDesc pl = vertex_layout_desc(VertexLayout::PrimVertLit);
    CHECK(pl.stride == 36 && pl.attributes.size() == 4);
    for (const auto& a : pl.attributes) {
        if (a.location == kLocColor0) CHECK(a.offset == 24 && a.vk_format == vkfmt::kB8G8R8A8Unorm);
        if (a.location == kLocTexcoord0) CHECK(a.offset == 28);
    }
    CHECK(ff_inputs_for_layout(VertexLayout::PrimVertLit) == (kPermFfNormal | kPermFfColor));
}

// C4b per-draw evaluation: FfDrawTemplate / CompiledExpression / select_exact.
void test_per_draw() {
    // MaterialDiffuse = m_lightScale * Diffuse as a real FXLC stream: mul out0.xyzw, c0, c1.
    std::vector<fx::Parameter> params = {float_param("m_lightScale", "LIGHT_SCALE", {1, 1, 1, 1}, 1, 4),
                                         float_param("Diffuse", "", {0.8f, 0.6f, 0.4f, 1.0f}, 1, 4)};
    // CTAB of make_fxlc only declares m_lightScale@c0; build a 2-input stream by hand instead:
    // lt-based fade (blend) + a Diffuse-free check via light scale squared (mul out0, c0, c0).
    std::vector<uint8_t> sq = make_fxlc({1, (0x205u << 20) | 4u, 2, 0, 2, 0, 0, 2, 0, 0, 4, 0}, {1.0});
    fx::State md = st_expr(op::kMaterialDiffuse, sq);
    md.type.type = fx::ParamType::Float;
    md.value.words = {0, 0, 0, std::bit_cast<uint32_t>(0.25f)};
    fx::Effect e = make_effect({rs(RS::Lighting, 1), tss(0, TSS::ColorOp, d3d::kModulate), md,
                                st_expr(static_cast<uint32_t>(RS::AlphaBlendEnable), make_fade_fxlc())},
                               params);
    LoadOptions opt;
    ResolvedPass rp = resolve_pass(e, 0, 0, d3d9_defaults(), opt, "MeshAlpha", VertexLayout::NU2);
    CHECK(rp.ff.has_value() && rp.fade_dynamic && rp.fade_expr != nullptr);
    if (!rp.ff) return;
    FfDrawTemplate tpl;
    std::string err;
    CHECK(tpl.build(*rp.ff, e, nullptr, false, &err));
    CHECK(tpl.dynamic());
    ExprInputs in;
    const FfDrawData d1 = tpl.evaluate(in);
    const FfDrawData ref = build_ff_draw_data(*rp.ff, e, nullptr, false);
    CHECK(std::memcmp(&d1, &ref, sizeof d1) == 0);  // defaults == load-time evaluation
    in.light_scale = {0.5f, 0.25f, 1.0f, 0.5f};
    FfDrawData d2;
    tpl.update(in, d2);
    CHECK(d2.mat_diffuse.x == 0.25f && d2.mat_diffuse.y == 0.0625f && d2.mat_diffuse.z == 1.0f &&
          d2.mat_diffuse.w == 0.25f);
    CHECK(std::memcmp(&d2.stage, &ref.stage, sizeof ref.stage) == 0);
    // Exact fade switch per draw.
    CHECK(!rp.blend_enabled(ExprInputs{}));
    CHECK(rp.blend_enabled(in));
    CHECK(&rp.select_exact(in) == &rp.pso[1] && rp.select_exact(in).blend_enable == 1);
    CHECK(&rp.select_exact(ExprInputs{}) == &rp.pso[0]);
    // Thread safety smoke test: concurrent updates give identical results.
    std::vector<FfDrawData> outs(8);
    std::vector<std::thread> th;
    for (size_t i = 0; i < outs.size(); ++i)
        th.emplace_back([&, i] {
            for (int k = 0; k < 1000; ++k) tpl.update(in, outs[i]);
        });
    for (auto& t : th) t.join();
    for (const auto& o : outs) CHECK(std::memcmp(&o, &d2, sizeof o) == 0);

    // Track B program ids: deterministic, translated bit set, options are part of the id.
    const uint8_t bc[8] = {0x01, 0x01, 0xFE, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    shaderxlat::TranslateOptions o1, o2;
    o2.projected_stages = 2;
    CHECK(translated_program_id(bc, o1) == translated_program_id(bc, o1));
    CHECK((translated_program_id(bc, o1) & kProgramIdTranslatedBit) != 0);
    CHECK(translated_program_id(bc, o1) != translated_program_id(bc, o2));
    CHECK(PsoKey{}.flags == 0);
}

// ------------------------------------------------------------------ hardening (review round 3)
// fx_2_0 container: tag, data size, data words (data offset 0 should be a zero word = empty name),
// then the header words (npar, ntech, ..., nstr, nres) verbatim.
std::vector<uint8_t> make_fxo(const std::vector<uint32_t>& data, const std::vector<uint32_t>& header) {
    std::vector<uint8_t> b;
    put32(b, 0xFEFF0901u);
    put32(b, static_cast<uint32_t>(data.size() * 4));
    for (uint32_t w : data) put32(b, w);
    for (uint32_t w : header) put32(b, w);
    return b;
}
// One parameter (typedef at data offset toff, value at voff), no techniques.
std::vector<uint8_t> make_param_fxo(const std::vector<uint32_t>& data, uint32_t toff, uint32_t voff) {
    return make_fxo(data, {1, 0, 0, 0, toff, voff, 0, 0, 0, 0});
}
// A small well-formed effect: float4 parameter + technique/pass with ZWRITEENABLE = 0.
std::vector<uint8_t> make_valid_fxo() {
    const uint32_t one = std::bit_cast<uint32_t>(1.0f);
    std::vector<uint32_t> data = {0,
                                  3, 1, 0, 0, 0, 4, 1,   // @4  float4 typedef
                                  one, one, one, one,    // @32 value
                                  2, 0, 0, 0, 0, 1, 1,   // @48 int typedef
                                  0};                    // @76 value
    return make_fxo(data, {1, 1, 0, 0,        // npar, ntech, -, nobj
                           4, 32, 0, 0,       // parameter
                           0, 0, 1,           // technique: name, nann, npass
                           0, 0, 1,           // pass: name, nann, nstates
                           3, 0, 48, 76,      // state ZWRITEENABLE
                           0, 0});            // nstr, nres
}

enum class ParseOutcome { Ok, Error, Crash };
ParseOutcome try_parse(const std::vector<uint8_t>& b, std::string* what = nullptr) {
    try {
        fx::parseEffect(b.data(), b.size());
        return ParseOutcome::Ok;
    } catch (const fx::ParseError& e) {
        if (what) *what = e.what();
        return ParseOutcome::Error;
    } catch (...) {
        return ParseOutcome::Crash;  // anything but ParseError (bad_alloc ...) is a finding
    }
}
double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void test_hardening() {
    using clock = std::chrono::steady_clock;
    std::string what;
    // 1. Zero-size array elements (0x0 scalar) used to spin 65535x per nesting level (11.9 s / never).
    {
        auto t0 = clock::now();
        auto b = make_param_fxo({0, 3, 0, 0, 0, 65535, 0, 0, 0}, 4, 32);
        CHECK(try_parse(b, &what) == ParseOutcome::Error && what.find("zero-size") != std::string::npos);
        // struct[65535] { float m[65535] (0x0) }
        auto b2 = make_param_fxo({0, 0, 5, 0, 0, 65535, 1, 3, 0, 0, 0, 65535, 0, 0, 0}, 4, 56);
        CHECK(try_parse(b2) == ParseOutcome::Error);
        // struct[65535] { float m[65535] (1x1) }: elements consume bytes -> past end, quickly
        auto b3 = make_param_fxo({0, 0, 5, 0, 0, 65535, 1, 3, 0, 0, 0, 65535, 1, 1, 0}, 4, 56);
        CHECK(try_parse(b3) == ParseOutcome::Error);
        // empty struct repeated
        auto b4 = make_param_fxo({0, 0, 5, 0, 0, 100, 0, 0}, 4, 28);
        CHECK(try_parse(b4) == ParseOutcome::Error);
        // one zero-size element is harmless
        auto b5 = make_param_fxo({0, 3, 0, 0, 0, 1, 0, 0, 0}, 4, 32);
        CHECK(try_parse(b5) == ParseOutcome::Ok);
        CHECK(seconds_since(t0) < 1.0);
    }
    // 2. Deeply nested struct typedefs: parse error (was a stack overflow at 100,000 levels).
    auto nested = [](uint32_t levels) {
        std::vector<uint32_t> d = {0};
        for (uint32_t i = 0; i < levels; ++i)
            for (uint32_t w : {0u, 5u, 0u, 0u, 0u, 1u}) d.push_back(w);  // struct { 1 member }
        for (uint32_t w : {3u, 0u, 0u, 0u, 0u, 1u, 1u}) d.push_back(w);   // float
        const uint32_t voff = static_cast<uint32_t>(d.size() * 4);
        d.push_back(std::bit_cast<uint32_t>(2.5f));
        return make_param_fxo(d, 4, voff);
    };
    {
        auto t0 = clock::now();
        CHECK(try_parse(nested(100000), &what) == ParseOutcome::Error && what.find("nesting") != std::string::npos);
        CHECK(try_parse(nested(32)) == ParseOutcome::Error);
        auto ok = nested(31);  // 31 structs + leaf = depth 32
        CHECK(try_parse(ok) == ParseOutcome::Ok);
        if (try_parse(ok) == ParseOutcome::Ok) {
            fx::Effect e = fx::parseEffect(ok.data(), ok.size());
            CHECK(e.parameters.size() == 1 && e.parameters[0].value.words.size() == 1);
        }
        CHECK(seconds_since(t0) < 2.0);
    }
    // Fuzz: truncations and deterministic byte mutations of a valid effect, through the parser and
    // the materials load boundary. Every case must end quickly with a result or an error.
    {
        struct NoFiles final : FileProvider {
            bool read(std::string_view, std::vector<uint8_t>&) const override { return false; }
            bool exists(std::string_view) const override { return false; }
            std::string origin(std::string_view) const override { return {}; }
        } nofiles;
        EffectLibrary lib(nofiles);
        const std::vector<uint8_t> good = make_valid_fxo();
        CHECK(try_parse(good) == ParseOutcome::Ok);
        std::string err;
        CHECK(lib.load_from_memory("FUZZ.fxo", good, &err).has_value());
        int crashes = 0, threw = 0;
        double worst = 0.0;
        auto run = [&](const std::vector<uint8_t>& b) {
            auto t0 = clock::now();
            if (try_parse(b) == ParseOutcome::Crash) ++crashes;
            try {
                std::string e;
                (void)lib.load_from_memory("FUZZ.fxo", b, &e);
            } catch (...) {
                ++threw;
            }
            worst = std::max(worst, seconds_since(t0));
        };
        for (size_t n = 0; n < good.size(); ++n) run(std::vector<uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n)));
        uint32_t rng = 0x12345678u;
        auto next = [&] {
            rng = rng * 1664525u + 1013904223u;
            return rng >> 8;
        };
        const uint32_t interesting[] = {0, 1, 2, 4, 5, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFu, 0x10000u, 65535u, 4096u};
        for (int i = 0; i < 3000; ++i) {
            std::vector<uint8_t> b = good;
            const int edits = 1 + static_cast<int>(next() % 3);
            for (int k = 0; k < edits; ++k) {
                const size_t at = (next() % (b.size() / 4)) * 4;
                const uint32_t v = (next() & 1) ? interesting[next() % std::size(interesting)] : next();
                for (int j = 0; j < 4; ++j) b[at + static_cast<size_t>(j)] = static_cast<uint8_t>(v >> (8 * j));
            }
            run(b);
        }
        std::printf("  fx2 fuzz: %zu truncations + 3000 mutations, worst case %.3f ms\n", good.size(), worst * 1000.0);
        CHECK(crashes == 0 && threw == 0);
        CHECK(worst < 0.5);
    }

    // 3. Bump-env registers only for ps_1_x passes with texbem/texbeml/bem.
    {
        auto shader_state = [](uint32_t op_, std::vector<uint32_t> words) {
            fx::State s;
            s.operation = op_;
            s.type.type = op_ == op::kVertexShader ? fx::ParamType::VertexShader : fx::ParamType::PixelShader;
            s.kind = fx::State::Kind::Shader;
            for (uint32_t w : words) put32(s.blob, w);
            return s;
        };
        const uint32_t tdst = 0x80000000u | (3u << 28) | (0xFu << 16), tsrc = 0x80000000u | (3u << 28) | (0xE4u << 16);
        const std::vector<uint32_t> vs11 = {0xFFFE0101u, 0x0000FFFFu};
        const std::vector<uint32_t> ps11_texbem = {0xFFFF0101u, 66u, tdst | 0, 67u, tdst | 1, tsrc | 0, 0x0000FFFFu};
        const std::vector<uint32_t> ps11_plain = {0xFFFF0101u, 66u, tdst | 0, 0x0000FFFFu};
        const std::vector<uint32_t> ps20 = {0xFFFF0200u, 0x0000FFFFu};
        const fx::State mat00 = tss(0, TSS::BumpEnvMat00, std::bit_cast<uint32_t>(0.5f));
        LoadOptions opt;
        auto resolve = [&](const std::vector<uint32_t>& ps) {
            fx::Effect e = make_effect({shader_state(op::kVertexShader, vs11), shader_state(op::kPixelShader, ps), mat00});
            return std::make_pair(resolve_pass(e, 0, 0, d3d9_defaults(), opt, "NoHotProgram", VertexLayout::NU2), e);
        };
        auto [bp, be] = resolve(ps11_texbem);
        CHECK(bp.program_status == ProgramStatus::Translated && bp.uses_bump_env);
        auto [pp1, pe1] = resolve(ps11_plain);
        CHECK(pp1.program_status == ProgramStatus::Translated && !pp1.uses_bump_env);
        auto [p2, e2] = resolve(ps20);
        CHECK(p2.program_status == ProgramStatus::Translated && !p2.uses_bump_env);
        std::vector<float4> regs(32, float4{9, 9, 9, 9});
        fill_bump_env(p2, e2, nullptr, regs);
        fill_bump_env(pp1, pe1, nullptr, regs);
        bool untouched = true;
        for (const auto& r : regs) untouched = untouched && r.x == 9 && r.y == 9 && r.z == 9 && r.w == 9;
        CHECK(untouched);  // ps_2_0 CTAB constants in c16..c31 survive
        fill_bump_env(bp, be, nullptr, regs);
        CHECK(regs[16].x == 0.5f && regs[16].y == 0.0f && regs[24].x == 0.0f && regs[15].x == 9.0f);
    }

    // 4. Program-id registry: fold clashes probe to a free id, same hash -> same id.
    {
        const uint64_t h1 = 0x0000000100000000ull, h2 = 0x0000000000000001ull, h3 = 0x8000000000000001ull;
        const uint32_t a = register_program_id(h1), b = register_program_id(h2), c = register_program_id(h3);
        CHECK(a == 0x80000001u && b == 0x80000002u && c == 0x80000003u);
        CHECK(register_program_id(h1) == a && register_program_id(h2) == b && register_program_id(h3) == c);
        const uint64_t top = 0x000000007FFFFFFFull, top2 = 0x000000017FFFFFFEull;  // both fold to 0xFFFFFFFF
        const uint32_t t1 = register_program_id(top), t2 = register_program_id(top2);
        CHECK(t1 == 0xFFFFFFFFu && t2 == 0x80000000u);  // wraps inside the translated namespace
        CHECK((t2 & kProgramIdTranslatedBit) != 0);
    }

    // 5. Int constants from non-finite / huge floats: saturated, NaN -> 0 (was UB).
    {
        const float inf = std::numeric_limits<float>::infinity();
        fx::Effect e = make_effect({}, {float_param("I", "", {std::nanf(""), 3.0e9f, -3.0e9f, inf, -inf, -7.9f, 2147483520.0f, -2147483648.0f}, 2, 4)});
        RegisterFilePlan plan;
        RegisterBinding rb;
        rb.name = "I";
        rb.parameter = 0;
        rb.reg = 0;
        rb.count = 2;
        plan.ints.push_back(rb);
        IntBoolConstants ib;
        fill_int_bool_constants(e, plan, nullptr, ib);
        const int32_t mx = std::numeric_limits<int32_t>::max(), mn = std::numeric_limits<int32_t>::min();
        CHECK(ib.i[0][0] == 0 && ib.i[0][1] == mx && ib.i[0][2] == mn && ib.i[0][3] == mx);
        CHECK(ib.i[1][0] == mn && ib.i[1][1] == -7 && ib.i[1][2] == 2147483520 && ib.i[1][3] == mn);
    }
}

uint32_t spv_entry_model(const ShaderBlob& b, std::string* name) {
    const auto& w = b.spirv;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xFFFFu, n = w[i] >> 16;
        if (n == 0) break;
        if (op == 15 && i + 3 < w.size()) {  // OpEntryPoint
            const char* s = reinterpret_cast<const char*>(&w[i + 3]);
            *name = std::string(s, strnlen(s, 4 * (n - 3)));
            return w[i + 1];
        }
        i += n;
    }
    return 0xFFFFFFFFu;
}

void test_spirv_registry() {
    auto blobs = all_shader_blobs();
    CHECK(blobs.size() == 24);
    std::set<uint32_t> ids;
    for (const auto& b : blobs) {
        CHECK(b.spirv.size() > 5);
        CHECK(!b.spirv.empty() && b.spirv[0] == 0x07230203u);
        std::string ep;
        const uint32_t model = spv_entry_model(b, &ep);
        CHECK(ep == b.entry_point);
        CHECK(model == (b.stage == ShaderStage::Vertex ? 0u : 4u));  // Vertex / Fragment
        ids.insert(b.id);
    }
    CHECK(ids.size() == blobs.size());
    CHECK(programs_fingerprint() != 0);

    uint32_t covered = 0;
    for (const auto& h : hot_effects()) {
        ProgramLookup pl = find_program(h.effect);
        CHECK(pl.status == ProgramStatus::Ok);
        CHECK(pl.program.vs && pl.program.ps && pl.program.iface);
        covered += h.combined_drawn;
    }
    std::printf("  hot set covers %u / %u combined drawn submeshes (%.1f%%)\n", covered, kCatalogCombinedDrawn,
                100.0 * covered / kCatalogCombinedDrawn);
    CHECK(covered * 100 >= kCatalogCombinedDrawn * 94);

    CHECK(find_program("MeshBumpColorize.fx").status == ProgramStatus::Ok);
    CHECK(find_program("Data\\Art\\Shaders\\MESHGLOSS.FXO").status == ProgramStatus::Ok);
    ProgramLookup skin = find_program("MeshBumpColorize", kPermSkinned);
    CHECK(skin.status == ProgramStatus::Ok && std::string(skin.program.vs->name) == "mesh_vs_bump_skin");
    CHECK(skin.program.vertex_layout == VertexLayout::RSkinNU2U3U3);
    CHECK(find_program("RSkinShadowVolume").program.vs == find_shader_blob("shadow_vs_skin"));
    CHECK(find_program("TerrainWater").status == ProgramStatus::NeedsTranslation);
    CHECK(find_program("MeshBumpColorize", 1u << 5).status == ProgramStatus::UnknownPermutation);
    for (const char* ff : {"MeshAlpha", "PrimAlpha", "PrimAdditive", "PrimDiffuseAlpha", "PrimOpaque"}) {
        ProgramLookup p = find_program(ff);
        CHECK(p.status == ProgramStatus::Ok && p.program.family == ProgramFamily::FixedFunction);
    }
    CHECK(std::string(find_program("PrimAlpha", kPermFfColor | kPermFfNormal).program.vs->name) == "ff_vs_nc");
    // C4b regression: the lit particle effect must get normals (it was drawn with ff_vs_c, i.e. N = 0).
    CHECK(std::string(find_program("PrimDiffuseAlpha").program.vs->name) == "ff_vs_nc");
    CHECK(find_program("PrimDiffuseAlpha").program.vertex_layout == VertexLayout::PrimVertLit);
    for (const auto& h : hot_effects())
        if (h.family == ProgramFamily::FixedFunction) CHECK(std::string(find_program(h.effect).program.vs->name) == h.vs);
    const ProgramInterface* mi = find_program("RSkinGloss").program.iface;
    CHECK(mi && std::string(mi->notes).find("NORMAL.w") != std::string::npos);
}

// D.3 default effect + D.1 creation order (synthetic).
void test_default_effect_and_order() {
    struct OneBroken final : FileProvider {
        bool read(std::string_view p, std::vector<uint8_t>& out) const override {
            if (!exists(p)) return false;
            if (normalize_path(p) == "DATA\\ART\\SHADERS\\NOTECH.FXO") {
                const uint32_t one = std::bit_cast<uint32_t>(1.0f);
                out = make_param_fxo({0, 3, 1, 0, 0, 0, 4, 1, one, one, one, one}, 4, 32);  // parses, no technique
                return true;
            }
            out = {0x01, 0x09, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00};
            return true;
        }
        bool exists(std::string_view p) const override {
            const std::string n = normalize_path(p);
            return n == "DATA\\ART\\SHADERS\\BROKEN.FXO" || n == "DATA\\ART\\SHADERS\\NOTECH.FXO";
        }
        std::string origin(std::string_view) const override { return {}; }
    } files;
    EffectLibrary lib(files);
    const MaterialEffect& d = lib.default_effect();
    CHECK(d.name == kDefaultEffectName && d.fallback == EffectFallback::BuiltinDefault && d.is_default_effect());
    CHECK(d.phase == RenderPhase::Opaque && d.layout == VertexLayout::Unknown && d.vertex_type_override() == VertexLayout::Unknown);
    CHECK(d.technique_name == "TNoShader" && d.passes.size() == 1 && d.warnings.empty());
    if (d.passes.size() == 1) {
        const ResolvedPass& p = d.passes[0];
        CHECK(p.fixed_function && p.program_status == ProgramStatus::Ok && p.ff.has_value());
        CHECK(p.samplers.empty());
        CHECK(p.pso[0].blend_enable == 0 && p.pso[0].src_color == BlendFactor::One && p.pso[0].dst_color == BlendFactor::Zero);
        CHECK(p.pso[0].depth_write == 1 && !p.fade_dynamic);
        if (p.ff) {
            CHECK(p.ff->active_stages == 1 && p.ff->lighting);
            CHECK(p.ff->stage[0].color_op == d3d::kSelectArg1);
            ParamStore store;
            FfDrawData dd = build_ff_draw_data(*p.ff, *d.effect, store.lookup(), false);
            CHECK(dd.mat_emissive.x == 0.0f && dd.mat_emissive.y == 1.0f && dd.mat_emissive.z == 0.0f && dd.mat_emissive.w == 1.0f);
            CHECK(dd.mat_diffuse.y == 1.0f && dd.mat_ambient.y == 1.0f && dd.mat_diffuse.x == 0.0f);
        }
    }
    std::string err = "x";
    MaterialEffect miss = lib.load_or_default("FissureMeshGloss.fx", &err);
    CHECK(miss.fallback == EffectFallback::Missing && miss.name == kDefaultEffectName && err.empty());
    CHECK(miss.requested == "FissureMeshGloss.fx" && miss.passes.size() == 1);
    MaterialEffect broken = lib.load_or_default("Broken.fx", &err);
    CHECK(broken.fallback == EffectFallback::Failed && broken.name == "BROKEN" && !err.empty());
    CHECK(broken.passes.size() == 1 && broken.passes[0].fixed_function);
    // Technique-selection failure: the shared default object (its identity, rank 0), error reported.
    err.clear();
    MaterialEffect notech = lib.load_or_default("NoTech.fx", &err);
    CHECK(notech.fallback == EffectFallback::Failed && notech.name == kDefaultEffectName && notech.path.empty());
    CHECK(err.find("no technique") != std::string::npos && notech.requested == "NoTech.fx");
    CHECK(notech.technique_name == "TNoShader" && notech.passes.size() == 1);

    // Opt-in missing-effect remap: off by default; keys normalised; only MISSING names remap; a
    // mapped effect that is missing / broken / the same name falls back to the faithful green default.
    CHECK(lib.missing_effect_remap().empty() && miss.remapped_from.empty());
    CHECK(compat_missing_effect_remap().size() == 1 && compat_missing_effect_remap().count("FISSUREMESHGLOSS") == 1);
    {
        LoadOptions ro;
        ro.missing_effect_remap = {{"Data\\Art\\Shaders\\fissuremeshgloss.fx", "Nowhere.fx"}, {"Gone.fxo", "Broken.fx"},
                                   {"Self", "SELF.fx"}, {"NoTech.fx", "Broken.fx"}, {"", "x"}};
        EffectLibrary rlib(files, ro);
        CHECK(rlib.missing_effect_remap().size() == 4 && rlib.missing_effect_remap().count("FISSUREMESHGLOSS") == 1 &&
              rlib.missing_effect_remap().count("GONE") == 1);
        err = "x";
        MaterialEffect a = rlib.load_or_default("FissureMeshGloss.fx", &err);
        CHECK(a.fallback == EffectFallback::Missing && a.name == kDefaultEffectName && a.remapped_from.empty() && err.empty());
        MaterialEffect b = rlib.load_or_default("Gone.fx", &err);
        CHECK(b.fallback == EffectFallback::Missing && b.remapped_from.empty() && err.empty());
        MaterialEffect c = rlib.load_or_default("Self.fx", &err);
        CHECK(c.fallback == EffectFallback::Missing && c.remapped_from.empty());
        // NoTech exists (fails technique selection): never remapped, same result as without the table.
        MaterialEffect n = rlib.load_or_default("NoTech.fx", &err);
        CHECK(n.fallback == EffectFallback::Failed && n.name == kDefaultEffectName && n.remapped_from.empty());
        rlib.set_missing_effect_remap({});
        CHECK(rlib.missing_effect_remap().empty());
        rlib.set_missing_effect_remap(compat_missing_effect_remap());
        CHECK(rlib.missing_effect_remap().count("FISSUREMESHGLOSS") == 1);
        // MeshGloss is not in this provider: still the green default.
        CHECK(rlib.load_or_default("FissureMeshGloss.fx").fallback == EffectFallback::Missing);
    }

    // Order: head, then pattern-major / directory-major / loose-then-MEG(CRC).
    std::vector<ShaderFileEntry> in = {
        {"Data\\Art\\Shaders\\Zeta.fxo", false, 1},  {"Data\\Art\\Shaders\\Alpha.fxo", false, 1},
        {"DATA\\ART\\SHADERS\\TERRAIN\\T1.FXO", false, 1}, {"Data\\Art\\Shaders\\Loose.fxo", true, 1},
        {"Data\\Art\\Shaders\\ModB.fxo", true, 0},  {"Data\\Art\\Shaders\\ModA.fx", true, 0},
        {"Data\\Art\\Shaders\\Engine\\PrimOpaque.fxo", false, 1}, {"Data\\Art\\Shaders\\ModA.fxo", false, 1},
        {"Data\\Art\\Shaders\\Terrain\\T0.fxo", true, 0}};
    EffectLoadOrder o(in);
    const size_t head = engine_effect_load_sequence().size();
    CHECK(head == 21 && o.rank(std::string_view(kDefaultEffectName)) == 0 && o.rank("Engine\\Global.fxo") == 1);
    CHECK(o.rank("PrimOpaque.fx") == 3 && o.rank("FrameEffectFakeMotionBlur") == 20);
    // *.fx first (ModA, mod dir 0), then *.fxo: mod 0 Shaders\ (ModB), mod 0 Terrain\ (T0), base
    // Shaders\ loose (Loose) then MEG by CRC (Alpha / Zeta; ModA already created), base Terrain\ (T1).
    const bool alpha_first = name_crc("DATA\\ART\\SHADERS\\ALPHA.FXO") < name_crc("DATA\\ART\\SHADERS\\ZETA.FXO");
    std::vector<std::string> want = {"MODA", "MODB", "T0", "LOOSE", alpha_first ? "ALPHA" : "ZETA",
                                     alpha_first ? "ZETA" : "ALPHA", "T1"};
    CHECK(o.size() == head + want.size());
    for (size_t i = 0; i < want.size() && head + i < o.size(); ++i) CHECK(o.names()[head + i] == want[i]);
    std::vector<ShaderFileEntry> rev(in.rbegin(), in.rend());
    CHECK(EffectLoadOrder(rev).names() == o.names());  // independent of input order
    CHECK(o.rank("NotThere.fx") == o.size());
    CHECK(o.rank(miss) == 0 && o.rank(d) == 0 && o.rank(notech) == 0);
    lib.set_shader_files(in);
    CHECK(lib.effect_load_rank("ModB") == head + 1 && lib.effect_load_rank(broken) == lib.load_order().size());
    CHECK(lib.effect_load_rank(notech) == 0);
}

// A fresh folder under the per-tree %TEMP% (the build scripts give each ctest run its own), unique per call.
std::filesystem::path unique_temp_dir(const char* tag) {
    static int counter = 0;
    char name[128];
    std::snprintf(name, sizeof name, "alamo_mat_%s_%lld_%d", tag,
                  static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()), counter++);
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / name;
}

// docs/MODS.md shader section (modc t11): .fx sources, the issue record and audit_effects().
void test_shader_issues() {
    struct ModShaders final : FileProvider {
        bool read(std::string_view p, std::vector<uint8_t>& out) const override {
            const std::string n = normalize_path(p);
            if (!exists(p)) return false;
            if (n.size() > 3 && n.substr(n.size() - 3) == ".FX") {
                const char* src = "technique T { pass P { } }";  // HLSL text: never parsed as an FXO
                out.assign(src, src + std::strlen(src));
                return true;
            }
            if (n == "DATA\\ART\\SHADERS\\BOTH.FXO") {
                const uint32_t one = std::bit_cast<uint32_t>(1.0f);
                out = make_param_fxo({0, 3, 1, 0, 0, 0, 4, 1, one, one, one, one}, 4, 32);  // parses, no technique
                return true;
            }
            out = {0x01, 0x09, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00};  // BROKEN.FXO
            return true;
        }
        bool exists(std::string_view p) const override {
            const std::string n = normalize_path(p);
            return n == "DATA\\ART\\SHADERS\\SRCONLY.FX" || n == "DATA\\ART\\SHADERS\\BOTH.FX" ||
                   n == "DATA\\ART\\SHADERS\\BOTH.FXO" || n == "DATA\\ART\\SHADERS\\TERRAIN\\BROKEN.FXO";
        }
        std::string origin(std::string_view) const override { return {}; }
    } files;
    LoadOptions iso;
    iso.fx_cache_dir = unique_temp_dir("issues");  // empty cache: the sources count as not compiled (modc t23)
    EffectLibrary lib(files, iso);
    CHECK(!lib.find_effect_path("SrcOnly.fx") && lib.find_effect_source("SrcOnly.fx").has_value());
    CHECK(lib.fx_source_status("SrcOnly") == FxSourceState::NotCompiled && lib.fx_source_status("Broken") == FxSourceState::None);
    CHECK(lib.find_effect_path("Both") && lib.find_effect_source("BOTH.FXO"));
    CHECK(!lib.find_effect_source("Broken.fx") && lib.find_effect_path("Broken.fx") == std::string("Data\\Art\\Shaders\\Terrain\\Broken.fxo"));
    // A path naming the .fx is never read as compiled bytecode.
    CHECK(!lib.find_effect_path("Data\\Art\\Shaders\\SrcOnly.fx"));
    std::string err;
    CHECK(!lib.load("Data\\Art\\Shaders\\SrcOnly.fx", &err) && err.find(".fxo") != std::string::npos);
    // .fx only: the original compiles it; the port draws its failed-compile result (green, own name).
    err.clear();
    MaterialEffect so = lib.load_or_default("SrcOnly.fx", &err);
    CHECK(so.fallback == EffectFallback::Failed && so.name == "SRCONLY" && so.path == "Data\\Art\\Shaders\\SrcOnly.fx");
    CHECK(!err.empty() && so.passes.size() == 1 && so.passes[0].fixed_function);
    std::vector<ShaderIssue> si = effect_load_issues(lib, "SrcOnly.fx", so, err);
    CHECK(si.size() == 1 && si[0].kind == ShaderIssueKind::SourceOnly && si[0].effect == "SRCONLY");
    // A missing name is not a shader issue (the original's silent green default).
    MaterialEffect miss = lib.load_or_default("Nothing.fx", &err);
    CHECK(miss.fallback == EffectFallback::Missing && effect_load_issues(lib, "Nothing.fx", miss, err).empty());

    CHECK(std::string(shader_issue_kind_name(ShaderIssueKind::SourceOnly)) == "source_only");
    CHECK(std::string(shader_issue_kind_name(ShaderIssueKind::UnsupportedPass)) == "unsupported_pass");
    CHECK(shader_path_key("data/Art\\shaders\\x.fxo") == "DATA\\ART\\SHADERS\\X.FXO");
    CHECK(host_path_under("C:/Mods/A/Data/Shaders.meg", "c:\\mods\\a") && host_path_under("C:\\Mods\\A\\x", "C:\\Mods\\A\\"));
    CHECK(!host_path_under("C:\\Mods\\AB\\x.meg", "C:\\Mods\\A") && !host_path_under("C:\\Mods\\A", "C:\\Mods\\A") && !host_path_under("x", ""));

    std::vector<ShaderFileEntry> in = {{"DATA\\ART\\SHADERS\\SRCONLY.FX", true, 0, true},
                                       {"DATA\\ART\\SHADERS\\BOTH.FX", true, 0, true},
                                       {"DATA\\ART\\SHADERS\\BOTH.FXO", false, 1, false},
                                       {"DATA\\ART\\SHADERS\\TERRAIN\\BROKEN.FXO", true, 1, false}};
    EffectAudit a = audit_effects(lib, in);
    CHECK(a.effects == 3 && a.mod_effects == 2 && a.loaded == 0 && a.translated_passes == 0 && !a.compiled);
    CHECK(a.issues.size() == 4);
    if (a.issues.size() == 4) {
        CHECK(a.issues[0].effect == "BOTH" && a.issues[0].kind == ShaderIssueKind::SourceShadowed && a.issues[0].in_mod);
        CHECK(a.issues[1].effect == "BOTH" && a.issues[1].kind == ShaderIssueKind::NoTechnique && !a.issues[1].in_mod);
        CHECK(a.issues[2].effect == "BROKEN" && a.issues[2].kind == ShaderIssueKind::LoadFailed && !a.issues[2].detail.empty());
        CHECK(a.issues[3].effect == "SRCONLY" && a.issues[3].kind == ShaderIssueKind::SourceOnly && a.issues[3].in_mod);
    }
    CHECK(a.count(ShaderIssueKind::SourceOnly) == 1 && a.count(ShaderIssueKind::TranslateFailed) == 0);
    std::vector<ShaderFileEntry> rev(in.rbegin(), in.rend());
    EffectAudit b = audit_effects(lib, rev);
    CHECK(b.issues.size() == a.issues.size());
    for (size_t i = 0; i < a.issues.size() && i < b.issues.size(); ++i)
        CHECK(a.issues[i].effect == b.issues[i].effect && a.issues[i].kind == b.issues[i].kind && a.issues[i].path == b.issues[i].path);
}

#ifdef ALAMO_MATERIALS_HAVE_VFS
// modc t23: .fx sources (fx_source.hpp), the FxCache, the loader's choice and the fxc prebuild (fx_compile.hpp).
struct MapFiles final : FileProvider {
    std::map<std::string, std::string> f;  // normalize_path -> contents
    void put(std::string_view p, std::string t) { f[normalize_path(p)] = std::move(t); }
    bool read(std::string_view p, std::vector<uint8_t>& out) const override {
        out.clear();
        const auto it = f.find(normalize_path(p));
        if (it == f.end()) return false;
        out.assign(it->second.begin(), it->second.end());
        return true;
    }
    bool exists(std::string_view p) const override { return f.count(normalize_path(p)) != 0; }
    std::string origin(std::string_view) const override { return {}; }
};

void test_fx_sources() {
    // 1. #include collection: own folder first, then Data\Art\Shaders\; comments skipped; cycles and repeats once.
    MapFiles m;
    m.put("Data\\Art\\Shaders\\Terrain\\Mod.fx",
          "// #include \"Commented.fxh\"\n/* #include \"Block.fxh\"\n */\n#include \"Local.fxh\"\n  #  include <Common.fxh>\r\n"
          "#include \"Sub/Deep.fxh\"\n#include \"Gone.fxh\"\nfloat x;\n");
    m.put("Data\\Art\\Shaders\\Terrain\\Local.fxh", "#include \"Mod.fx\"\nfloat a;\n");
    m.put("Data\\Art\\Shaders\\Common.fxh", "float c;\n");
    m.put("Data\\Art\\Shaders\\Terrain\\Sub\\Deep.fxh", "#include \"..\\Local.fxh\"\n#include \"Deeper.fxh\"\n");
    m.put("Data\\Art\\Shaders\\Terrain\\Sub\\Deeper.fxh", "float d;\n");
    FxSource src;
    std::string err;
    CHECK(collect_fx_source(m, "Data\\Art\\Shaders\\Terrain\\Mod.fx", src, &err));
    CHECK(src.files.size() == 5 && src.missing == std::vector<std::string>{"Gone.fxh"});
    if (src.files.size() == 5) {
        CHECK(normalize_path(src.files[1].path) == "DATA\\ART\\SHADERS\\TERRAIN\\LOCAL.FXH");
        CHECK(normalize_path(src.files[2].path) == "DATA\\ART\\SHADERS\\COMMON.FXH");
        CHECK(normalize_path(src.files[3].path) == "DATA\\ART\\SHADERS\\TERRAIN\\SUB\\DEEP.FXH");
        CHECK(normalize_path(src.files[4].path) == "DATA\\ART\\SHADERS\\TERRAIN\\SUB\\DEEPER.FXH");
    }
    const std::string key = src.key();
    CHECK(key.size() == 32 && key.find_first_not_of("0123456789abcdef") == std::string::npos);
    FxSource again;
    CHECK(collect_fx_source(m, "data/art/shaders/terrain/mod.fx", again) && again.key() == key);
    m.put("Data\\Art\\Shaders\\Terrain\\Sub\\Deeper.fxh", "float e;\n");  // an include changes: new key
    CHECK(collect_fx_source(m, "Data\\Art\\Shaders\\Terrain\\Mod.fx", again) && again.key() != key);
    m.put("Data\\Art\\Shaders\\Gone.fxh", "");  // a missing include appears: new key
    FxSource third;
    CHECK(collect_fx_source(m, "Data\\Art\\Shaders\\Terrain\\Mod.fx", third) && third.missing.empty() && third.key() != again.key());
    CHECK(!collect_fx_source(m, "Data\\Art\\Shaders\\Nope.fx", third, &err) && !err.empty());

    // 2. FxCache round trip; another key, a damaged file and an empty key are misses.
    const std::filesystem::path dir = unique_temp_dir("fxcache");
    const FxCache cache(dir);
    FxCacheEntry e;
    e.ok = true;
    e.compiler = "test";
    e.log = "warning X4717";
    e.fxo = make_valid_fxo();
    CHECK(cache.store(key, e));
    FxCacheEntry r;
    CHECK(cache.load(key, r) && r.ok && r.fxo == e.fxo && r.compiler == "test" && r.log == e.log);
    CHECK(!cache.load(again.key(), r) && !cache.load("", r) && !cache.store("", e));
    FxCacheEntry bad;
    bad.log = "x.fx(1,1): error X3000: syntax error";
    CHECK(cache.store(again.key(), bad) && cache.load(again.key(), r) && !r.ok && r.fxo.empty() && r.log == bad.log);
    {
        std::vector<uint8_t> bytes;
        CHECK(read_host_file(dir / (key + ".afx"), bytes) && bytes.size() > 40);
        bytes[bytes.size() / 2] ^= 0x5A;
        std::ofstream o(dir / (key + ".afx"), std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    CHECK(!cache.load(key, r));
    CHECK(fx_log_summary("w\n\nfoo.fx(3,1): error X3004: undeclared identifier 'k'\n") ==
          "foo.fx(3,1): error X3004: undeclared identifier 'k'");

    // 3. The loader: a compiled source wins over the .fxo (RENDER.md 6); a failed or missing entry keeps the
    //    .fxo (t11 fallback); use_fx_sources = false is the t11 behaviour.
    MapFiles g;
    g.put("Data\\Art\\Shaders\\Both.fx", "technique T { pass P { } }\n");
    g.put("Data\\Art\\Shaders\\Both.fxo", std::string(reinterpret_cast<const char*>(make_valid_fxo().data()), make_valid_fxo().size()));
    g.put("Data\\Art\\Shaders\\Lone.fx", "technique T { pass P { ZWriteEnable = false; } }\n");
    LoadOptions lo;
    lo.fx_cache_dir = unique_temp_dir("fxload");
    FxSource both, lone;
    CHECK(collect_fx_source(g, "Data\\Art\\Shaders\\Both.fx", both) && collect_fx_source(g, "Data\\Art\\Shaders\\Lone.fx", lone));
    {
        EffectLibrary lib(g, lo);
        CHECK(lib.fx_source_status("Both") == FxSourceState::NotCompiled);
        auto fx = lib.load("Both.fx", &err);
        CHECK(fx && fx->path == "Data\\Art\\Shaders\\Both.fxo");
        CHECK(!lib.load("Lone", &err) && err.find("alamo_fx_prebuild") != std::string::npos);
        std::vector<ShaderIssue> si = effect_load_issues(lib, "Lone", lib.load_or_default("Lone"), err);
        CHECK(si.size() == 1 && si[0].kind == ShaderIssueKind::SourceOnly && si[0].detail.find("alamo_fx_prebuild") != std::string::npos);
    }
    const FxCache lc(lo.fx_cache_dir);
    FxCacheEntry ok_entry;
    ok_entry.ok = true;
    ok_entry.fxo = make_valid_fxo();
    CHECK(lc.store(both.key(), ok_entry) && lc.store(lone.key(), ok_entry));
    {
        EffectLibrary lib(g, lo);
        CHECK(lib.fx_source_status("Both") == FxSourceState::Compiled && lib.fx_source_status("LONE.FX") == FxSourceState::Compiled);
        auto fx = lib.load("Both.fx", &err);
        CHECK(fx && fx->path == "Data\\Art\\Shaders\\Both.fx" && fx->name == "BOTH");
        auto lone_fx = lib.load("Lone", &err);
        CHECK(lone_fx && lone_fx->path == "Data\\Art\\Shaders\\Lone.fx");
        const MaterialEffect lm = lib.load_or_default("Lone", &err);
        CHECK(!lm.is_default_effect() && effect_load_issues(lib, "Lone", lm, err).empty());
        std::vector<ShaderFileEntry> in = {{"DATA\\ART\\SHADERS\\BOTH.FX", true, 0, true},
                                           {"DATA\\ART\\SHADERS\\BOTH.FXO", true, 0, true},
                                           {"DATA\\ART\\SHADERS\\LONE.FX", true, 0, true}};
        const EffectAudit a = audit_effects(lib, in);
        CHECK(a.effects == 2 && a.loaded == 2 && a.sources == 2 && a.sources_compiled == 2 && a.issues.empty());
        const std::vector<std::string> paths = fx_source_paths(lib, in);
        CHECK(paths.size() == 2 && normalize_path(paths[0]) == "DATA\\ART\\SHADERS\\BOTH.FX");
    }
    FxCacheEntry failed;
    failed.log = "Both.fx(1,1): error X3000: syntax error: unexpected token 'pass'\n";
    CHECK(lc.store(both.key(), failed));
    {
        EffectLibrary lib(g, lo);
        std::string why;
        CHECK(lib.fx_source_status("Both", &why) == FxSourceState::Failed && why.find("X3000") != std::string::npos);
        auto fx = lib.load("Both", &err);
        CHECK(fx && fx->path == "Data\\Art\\Shaders\\Both.fxo");  // t11 fallback
        std::vector<ShaderIssue> si = effect_load_issues(lib, "Both", *fx, err);
        CHECK(!si.empty() && si[0].kind == ShaderIssueKind::SourceShadowed && si[0].detail.find("X3000") != std::string::npos);
    }
    {
        LoadOptions off = lo;
        off.use_fx_sources = false;
        EffectLibrary lib(g, off);
        CHECK(lib.fx_source_status("Lone") == FxSourceState::Disabled && !lib.load("Lone", &err));
    }

    // 4. The prebuild with the toolchain's fxc.exe (skipped without it): compile, cache hit, compile error.
    const std::filesystem::path fxc = default_fxc_path();
    if (fxc.empty()) {
        std::printf("  fxc.exe not found (ALAMO_FXC / toolchain Windows SDK): fx prebuild check skipped\n");
    } else {
        MapFiles p;
        p.put("Data\\Art\\Shaders\\Inc\\Colors.fxh", "float4 K = {1, 0, 0, 1};\n");
        p.put("Data\\Art\\Shaders\\Good.fx",
              "#include \"Inc\\Colors.fxh\"\n"
              "float4 PS(float2 uv : TEXCOORD0) : COLOR { return K * uv.x; }\n"
              "technique T < string LOD = \"DX9\"; > { pass P { ZWriteEnable = false; PixelShader = compile ps_2_0 PS(); } }\n");
        p.put("Data\\Art\\Shaders\\Bad.fx", "technique T { pass P { VertexShader = compile vs_2_0 nope(); } }\n");
        const FxCache pc(unique_temp_dir("fxprebuild"));
        const std::vector<std::string> paths = {"Data\\Art\\Shaders\\Bad.fx", "Data\\Art\\Shaders\\Good.fx"};
        std::vector<FxPrebuildItem> items;
        CHECK(prebuild_fx_sources(p, paths, pc, fxc, false, items, &err));
        CHECK(items.size() == 2);
        if (items.size() == 2) {
            CHECK(!items[0].ok && !items[0].cached && items[0].detail.find("X3004") != std::string::npos);
            CHECK(items[0].detail.find("alamo_fxc_") == std::string::npos);  // temporary folder stripped
            std::printf("  fx prebuild: Bad.fx -> %s\n", items[0].detail.c_str());
            CHECK(items[1].ok && !items[1].cached);
        }
        CHECK(prebuild_fx_sources(p, paths, pc, fxc, false, items, &err) && items.size() == 2 && items[0].cached && items[1].cached &&
              items[1].ok);
        LoadOptions po;
        po.fx_cache_dir = pc.directory();
        EffectLibrary lib(p, po);
        auto good = lib.load("Good", &err);
        CHECK(good && good->path == "Data\\Art\\Shaders\\Good.fx" && good->passes.size() == 1);
        CHECK(lib.fx_source_status("Bad") == FxSourceState::Failed && !lib.load("Bad", &err));
        CHECK(!fxc_identity(fxc).empty());
        std::error_code ec;
        std::filesystem::remove_all(pc.directory(), ec);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove_all(lo.fx_cache_dir, ec);
}

void test_vanilla(const char* root) {
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    CHECK(v != nullptr);
    if (!v) {
        std::printf("  vfs: %s\n", err.c_str());
        return;
    }
    std::vector<std::string> files = v->list("DATA\\ART\\SHADERS\\*.FXO");
    for (auto& f : v->list("DATA\\ART\\SHADERS\\**\\*.FXO")) files.push_back(std::move(f));
    std::printf("  vanilla: %zu FXO files reachable\n", files.size());
    CHECK(files.size() >= 80);

    LoadOptions opt;
    EffectLibrary lib(*v, opt);
    std::set<PsoKey> keys, keys_all_passes, state_only;
    std::set<std::string> partial_draw, partial_all, partial_material;
    std::map<std::string, int> status;
    int parsed = 0, failed = 0, hot_ok = 0, translate = 0, ff = 0, draw_passes = 0, pres = 0, fade = 0;
    int translated = 0, translated_ok = 0, depth_writers = 0, bump_passes = 0;
    shaderxlat::RuntimeDxc dxc;
    std::string dxc_err;
    const bool have_dxc = dxc.load({}, &dxc_err);
    if (!have_dxc) std::printf("  runtime DXC not available (%s): translation sweep skipped\n", dxc_err.c_str());
    for (const auto& f : files) {
        std::string e;
        auto m = lib.load(f, &e);
        if (!m) {
            ++failed;
            std::printf("  FAIL load %s: %s\n", f.c_str(), e.c_str());
            continue;
        }
        ++parsed;
        CHECK(m->skipped_technique.empty());  // every shipped best technique has programs
        for (const auto& w : m->warnings) std::printf("  warn %s: %s\n", m->name.c_str(), w.c_str());
        const bool is_phase = m->name.rfind("PHASE", 0) == 0;
        for (const auto& p : m->passes) {
            const std::string sig = partial_state_signature(*m->effect, m->technique, p.pass_index);
            partial_all.insert(sig);
            if (!is_phase) partial_material.insert(sig);
            keys_all_passes.insert(p.pso[0]);
            keys_all_passes.insert(p.pso[1]);
            if (p.restore_only) continue;
            partial_draw.insert(sig);
            ++draw_passes;
            if (is_phase) continue;
            keys.insert(p.pso[0]);
            for (int vi = 0; vi < (p.fade_dynamic ? 2 : 1); ++vi) {
                PsoKey s = p.pso[vi];
                s.vs_id = s.ps_id = 0;
                s.flags = 0;
                s.vertex_layout = VertexLayout::Unknown;
                state_only.insert(s);
            }
            if (p.fade_dynamic) {
                keys.insert(p.pso[1]);
                ++fade;
            }
            if (p.program_status == ProgramStatus::Ok) {
                (p.fixed_function ? ff : hot_ok)++;
                CHECK((p.pso[0].vs_id & kProgramIdTranslatedBit) == 0);
            } else if (p.program_status == ProgramStatus::Translated) {
                ++translated;
                CHECK((p.pso[0].vs_id & kProgramIdTranslatedBit) != 0 && (p.pso[0].flags & kPsoTranslated) != 0);
                CHECK(p.pso[0].vs_id == translated_program_id(p.vs_bytecode, p.vs_options));
                if (have_dxc) {
                    std::string terr;
                    auto tp = translate_pass(p, dxc, nullptr, &terr);
                    if (tp) {
                        ++translated_ok;
                        CHECK(!tp->vs.spirv.empty() && tp->vs.spirv[0] == 0x07230203u && !tp->ps.spirv.empty());
                        CHECK(tp->key(p.pso[0]).vs_id == p.pso[0].vs_id && tp->key(p.pso[0]).ps_id == p.pso[0].ps_id);
                        if (tp->writes_depth) {
                            ++depth_writers;
                            CHECK((tp->key(p.pso[0]).flags & kPsoWritesDepth) != 0);
                        }
                        for (const auto& in : tp->inputs) CHECK(!in.fed || in.vk_format != 0);
                        CHECK(p.uses_bump_env == tp->bump_env);  // load-time scan == translator interface
                        if (p.uses_bump_env) ++bump_passes;
                    } else {
                        std::printf("  translation failed: %s %s: %s\n", m->name.c_str(), p.name.c_str(), terr.c_str());
                    }
                }
            } else {
                ++translate;
                if (p.pso[0].vs_id) CHECK((p.pso[0].vs_id & kProgramIdTranslatedBit) != 0);
                std::printf("  needs translation (not translatable): %s %s\n", m->name.c_str(), p.name.c_str());
            }
            if ((p.vs_plan && p.vs_plan->preshader) || (p.ps_plan && p.ps_plan->preshader)) ++pres;
        }
        status[m->name] = 1;
    }
    std::printf("  parsed %d, failed %d, draw passes %d (hot %d, FF %d, translated %d, untranslatable %d), fade passes %d, "
                "preshaders %d\n", parsed, failed, draw_passes, hot_ok, ff, translated, translate, fade, pres);
    if (have_dxc) {
        std::printf("  track B: %d/%d translated passes compiled through runtime DXC (%s), %d write depth, %d ps_1_x bump-env\n", translated_ok,
                    translated, dxc.version().c_str(), depth_writers, bump_passes);
        CHECK(translated_ok == translated);
    }
    CHECK(translate == 0);
    std::printf("  distinct partial render-state signatures (best technique): all passes %zu, material effects %zu, "
                "draw passes %zu\n", partial_all.size(), partial_material.size(), partial_draw.size());
    std::printf("  distinct resolved PsoKeys (material draw passes, opaque + fade variants): %zu "
                "(all passes incl. phase/cleanup, both variants: %zu)\n", keys.size(), keys_all_passes.size());
    const size_t non_empty = partial_all.size() - (partial_all.count(std::string()) ? 1 : 0);
    std::printf("  non-empty partial signatures: %zu (Phase 1 count: 52); resolved render-state-only keys: %zu\n", non_empty,
                state_only.size());
    CHECK(non_empty == 52);
    if (std::getenv("ALAMO_MATERIALS_VERBOSE")) {
        for (const auto& s : partial_all) std::printf("    sig [%s]\n", s.c_str());
        for (const auto& [n, s] : status) std::printf("    effect %s\n", n.c_str());
    }
    CHECK(failed == 0);
    CHECK(parsed >= 80);

    // Effect audit over the shipped shaders (translation only; the sweep above compiled them): retail has
    // no shader issue, and nothing comes from a mod.
    {
        const auto t0 = std::chrono::steady_clock::now();
        EffectLibrary alib(*v, opt);
        const std::vector<ShaderFileEntry> sf = shader_files_from_vfs(*v, true);
        const EffectAudit au = audit_effects(alib, sf);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  effect audit: %zu effects, %zu loaded, %zu/%zu track-B passes translate, %zu issues, %.0f ms\n", au.effects,
                    au.loaded, au.translated_ok, au.translated_passes, au.issues.size(), ms);
        for (const ShaderIssue& s : au.issues)
            std::printf("    %s %s %s: %s\n", shader_issue_kind_name(s.kind), s.effect.c_str(), s.path.c_str(), s.detail.c_str());
        CHECK(au.effects >= 80 && au.mod_effects == 0 && au.issues.empty() && au.translated_ok == au.translated_passes);
        CHECK(sf.size() > shader_files_from_vfs(*v).size());  // + the Shaders\Engine\ files
    }

    // Translated PsoKey stability: an independent library resolves identical keys and ids.
    {
        EffectLibrary lib2(*v, opt);
        auto a = lib.load("MeshShield.fx");
        auto b = lib2.load("MeshShield.fx");
        CHECK(a && b && !a->passes.empty() && !b->passes.empty());
        if (a && b && !a->passes.empty() && !b->passes.empty()) {
            CHECK(a->passes[0].program_status == ProgramStatus::Translated);
            CHECK(a->passes[0].pso[0] == b->passes[0].pso[0] && hash(a->passes[0].pso[0]) == hash(b->passes[0].pso[0]));
            std::printf("  MeshShield translated PsoKey hash 0x%016llx (vs 0x%08x ps 0x%08x)\n",
                        static_cast<unsigned long long>(hash(a->passes[0].pso[0])), a->passes[0].pso[0].vs_id,
                        a->passes[0].pso[0].ps_id);
        }
    }

    // Real-FXO fuzz (shaders, samplers, expressions): deterministic word mutations through the load
    // boundary; every case must finish quickly and nothing may escape.
    for (const char* fx_name : {"MeshShield.fx", "SceneHeat.fx", "MeshBumpColorize.fx"}) {
        std::vector<uint8_t> good;
        auto path = lib.find_effect_path(fx_name);
        if (!path || !v->read(*path, good) || good.size() < 16) continue;
        EffectLibrary flib(*v, opt);
        uint32_t rng = 0xC0FFEEu;
        auto next = [&] {
            rng = rng * 1664525u + 1013904223u;
            return rng >> 8;
        };
        int threw = 0;
        double worst = 0.0;
        for (int i = 0; i < 300; ++i) {
            std::vector<uint8_t> b = good;
            const size_t at = (next() % (b.size() / 4)) * 4;
            const uint32_t vals[] = {0u, 0xFFFFFFFFu, 0x7FFFFFFFu, 65535u, next(), next() & 0xFFu};
            const uint32_t val = vals[next() % std::size(vals)];
            for (int j = 0; j < 4; ++j) b[at + static_cast<size_t>(j)] = static_cast<uint8_t>(val >> (8 * j));
            if (i % 50 == 49) b.resize(next() % b.size());
            const auto t0 = std::chrono::steady_clock::now();
            try {
                std::string e;
                (void)flib.load_from_memory(fx_name, b, &e);
            } catch (...) {
                ++threw;
            }
            worst = std::max(worst, seconds_since(t0));
        }
        std::printf("  real-FXO fuzz %s: 300 mutations, worst %.3f ms\n", fx_name, worst * 1000.0);
        CHECK(threw == 0 && worst < 1.0);
    }

    // Bump-env registers: SceneHeat BUMPENVMAT00[1] = BUMPENVMAT11[1] = DistortionAmount (expressions).
    if (auto heat = lib.load("SceneHeat.fx"); heat && !heat->passes.empty()) {
        ParamStore store;
        store.set("DistortionAmount", {0.375f});
        // Its best technique is ps_2_0 (no texbem/bem): c16..c31 must stay untouched.
        PSConstants pc{};
        for (auto& r : pc.c) r = float4{7, 7, 7, 7};
        CHECK(!heat->passes[0].uses_bump_env);
        fill_bump_env(heat->passes[0], *heat->effect, store.lookup(), pc.c);
        CHECK(pc.c[17].x == 7.0f && pc.c[16].y == 7.0f && pc.c[31].w == 7.0f);
        // The expression path itself, as a ps_1_x bump pass would use it.
        ResolvedPass bump_pass = heat->passes[0];
        bump_pass.uses_bump_env = true;
        fill_bump_env(bump_pass, *heat->effect, store.lookup(), pc.c);
        CHECK(pc.c[17].x == 0.375f && pc.c[17].w == 0.375f && pc.c[17].y == 0.0f);
    }

    // Spot checks on the hot set.
    auto bump = lib.load("MeshBumpColorize.fx");
    CHECK(bump.has_value());
    if (bump) {
        CHECK(bump->lod == TechniqueLod::DX9);
        CHECK(bump->phase == RenderPhase::Opaque);
        CHECK(bump->layout == VertexLayout::NU2U3U3);
        CHECK(bump->passes.size() == 1);
        if (!bump->passes.empty()) {
            const auto& p = bump->passes[0];
            CHECK(p.fade_dynamic);
            CHECK(p.pso[0].cull == CullMode::Back);  // engine baseline D3DCULL_CW
            CHECK(p.pso[0].blend_enable == 0 && p.pso[1].blend_enable == 1);
            CHECK(p.program_status == ProgramStatus::Ok);
            CHECK(p.program.vs == find_shader_blob("mesh_vs_bump"));
            CHECK(p.samplers.size() == 2);
            if (p.samplers.size() == 2) {
                CHECK(p.samplers[0].texture_param == "BaseTexture");
                CHECK(p.samplers[1].texture_param == "NormalTexture");
                CHECK(p.samplers[0].key.u == AddressMode::Repeat && p.samplers[0].key.from_effect == 1);
            }
            CHECK(p.vs_plan && p.vs_plan->floats.size() >= 8);
            CHECK(p.ps_plan && p.ps_plan->samplers.size() == 2);
            // CTAB register fill: m_lightScale lands where the CTAB says.
            if (p.vs_plan) {
                ParamStore store;
                store.set_semantic("LIGHT_SCALE", {0.25f, 0.5f, 0.75f, 1.0f});
                VSConstants c{};
                fill_register_file(*bump->effect, *p.vs_plan, store.lookup(), c.c);
                for (const auto& rb : p.vs_plan->floats)
                    if (rb.name == "m_lightScale") CHECK(c.c[rb.reg].x == 0.25f && c.c[rb.reg].w == 1.0f);
            }
        }
    }
    auto sv = lib.load("MeshShadowVolume");
    CHECK(sv.has_value());
    if (sv && !sv->passes.empty()) {
        const auto& p = sv->passes[0];
        CHECK(sv->phase == RenderPhase::Shadow);
        CHECK(p.pso[0].stencil_enable == 1 && p.pso[0].cull == CullMode::None);
        CHECK(p.pso[0].back.depth_fail_op == StencilOp::IncrementAndWrap);
        CHECK(p.pso[0].front.depth_fail_op == StencilOp::DecrementAndWrap);
        CHECK(p.pso[0].color_write_mask == 0 && p.pso[0].depth_compare == CompareOp::Less);
        CHECK(p.program.vs == find_shader_blob("shadow_vs"));
        CHECK(p.vs_plan && p.vs_plan->preshader.has_value());
        if (p.vs_plan && p.vs_plan->preshader) {
            ParamStore store;
            store.set_semantic("SHADOW_EXTRUSION_DISTANCE", {10.0f});
            store.set_semantic("DIR_LIGHT_OBJ_VEC_0", {0.0f, 0.6f, 0.8f});
            VSConstants c{};
            fill_register_file(*sv->effect, *p.vs_plan, store.lookup(), c.c);
            std::printf("  MeshShadowVolume preshader c6 = (%g, %g, %g)\n", c.c[6].x, c.c[6].y, c.c[6].z);
            CHECK(std::fabs(c.c[6].y - 6.0f) < 1e-4f && std::fabs(c.c[6].z - 8.0f) < 1e-4f);
        }
    }
    auto tree = lib.load("Tree");
    CHECK(tree.has_value());
    if (tree && !tree->passes.empty()) {
        CHECK(tree->passes[0].push.alpha_func == static_cast<uint32_t>(CompareOp::Greater));
        CHECK(tree->passes[0].push.alpha_ref == 128);
        CHECK(tree->passes[0].fade_dynamic);
        CHECK(tree->passes.size() == 2 && tree->passes[1].restore_only);
    }
    auto alpha = lib.load("MeshAlpha");
    CHECK(alpha.has_value());
    if (alpha && !alpha->passes.empty()) {
        const auto& p = alpha->passes[0];
        CHECK(p.fixed_function && p.ff.has_value());
        CHECK(p.pso[0].blend_enable == 1 && p.pso[0].depth_write == 0);
        if (p.ff) {
            CHECK(p.ff->stage[0].color_op == d3d::kModulate2x);
            ParamStore store;
            store.set_semantic("LIGHT_SCALE", {1, 1, 1, 0.5f});
            store.set("Diffuse", {0.8f, 0.6f, 0.4f, 1.0f});
            FfDrawData d = build_ff_draw_data(*p.ff, *alpha->effect, store.lookup(), false);
            CHECK(std::fabs(d.mat_diffuse.x - 0.8f) < 1e-6f && std::fabs(d.mat_diffuse.w - 0.5f) < 1e-6f);
            CHECK(std::fabs(d.mat_ambient.y - 0.6f) < 1e-6f);
            CHECK(d.params.x == 1.0f);  // MaterialPower = 1
        }
    }
    for (const char* prim : {"PrimAlpha", "PrimAdditive"}) {
        auto pm = lib.load(prim);
        CHECK(pm.has_value());
        if (pm && !pm->passes.empty()) {
            CHECK(pm->passes[0].fixed_function && pm->passes[0].program_status == ProgramStatus::Ok);
            CHECK(pm->passes[0].pso[0].blend_enable == 1);
        }
    }
    // D.2: Planet.fx _ALAMO_VERTEX_TYPE overrides the ALO format.
    if (auto planet = lib.load("Planet.fx"); planet) {
        std::printf("  Planet.fx _ALAMO_VERTEX_TYPE = %s\n", planet->meta.vertex_type.c_str());
        CHECK(planet->vertex_type_override() == VertexLayout::NU2U3U3 && planet->phase == RenderPhase::Opaque);
    } else {
        CHECK(false);
    }
    // D.3: FissureMeshGloss.fx has no FXO -> default green effect.
    {
        std::string e;
        MaterialEffect fm = lib.load_or_default("FissureMeshGloss.fx", &e);
        CHECK(fm.fallback == EffectFallback::Missing && fm.phase == RenderPhase::Opaque && e.empty());
        CHECK(lib.load_or_default("MeshGloss.fx").fallback == EffectFallback::None);
        // Opt-in compat remap: FissureMeshGloss -> MeshGloss (same material interface, Opaque bucket).
        EffectLibrary rlib(*v);
        rlib.set_missing_effect_remap(compat_missing_effect_remap());
        e = "x";
        MaterialEffect rm = rlib.load_or_default("FissureMeshGloss.fx", &e);
        CHECK(rm.fallback == EffectFallback::None && !rm.is_default_effect() && rm.name == "MESHGLOSS" && e.empty());
        CHECK(rm.remapped_from == "FissureMeshGloss.fx" && rm.phase == RenderPhase::Opaque && !rm.passes.empty());
        bool has_base = false, has_shininess = false;
        for (const auto& p : rm.params) {
            has_base = has_base || (p.name == "BaseTexture" && p.texture && !p.engine);
            has_shininess = has_shininess || (p.name == "Shininess" && p.numeric && !p.engine);
        }
        CHECK(has_base && has_shininess);
        std::printf("  compat remap: FissureMeshGloss.fx -> %s (%s, %zu passes)\n", rm.name.c_str(),
                    render_phase_name(rm.phase), rm.passes.size());
        // Default library unchanged (faithful): still green.
        CHECK(lib.load_or_default("FissureMeshGloss.fx").fallback == EffectFallback::Missing);
    }
    // D.1: creation order on the real install.
    {
        const std::vector<ShaderFileEntry> sf = shader_files_from_vfs(*v);
        lib.set_shader_files(sf);
        const EffectLoadOrder& o = lib.load_order();
        std::printf("  effect creation order: %zu effects (%zu shader files); first 30:", o.size(), sf.size());
        for (size_t i = 0; i < o.size() && i < 30; ++i) std::printf("%s %s", i % 6 ? "," : "\n   ", o.names()[i].c_str());
        std::printf("\n");
        CHECK(sf.size() >= 45);
        std::set<std::string> bases;
        for (const auto& f : sf) {
            CHECK(o.rank(f.path) < o.size());
            std::string b = f.path.substr(f.path.find_last_of('\\') + 1);
            b = b.substr(0, b.find_last_of('.'));
            if (!bases.insert(b).second) std::printf("  duplicate base name (first wins): %s\n", f.path.c_str());
        }
        size_t head_dups = 0;
        for (const auto& h : engine_effect_load_sequence()) head_dups += bases.count(h);
        if (head_dups) std::printf("  %zu shader-dir names already created by the engine head\n", head_dups);
        CHECK(o.size() == engine_effect_load_sequence().size() + bases.size() - head_dups);
        // Shader-dir part is in ascending path-CRC order within each directory (vanilla: MEG only).
        uint32_t prev = 0;
        bool sorted = true, terrain = false;
        for (size_t i = engine_effect_load_sequence().size(); i < o.size(); ++i) {
            std::string p = "DATA\\ART\\SHADERS\\" + o.names()[i] + ".FXO";
            const bool in_root = v->exists(p);
            if (!in_root) p = "DATA\\ART\\SHADERS\\TERRAIN\\" + o.names()[i] + ".FXO";
            if (!in_root && !terrain) {
                terrain = true;
                prev = 0;
            }
            const uint32_t c = name_crc(p);
            if (c < prev) sorted = false;
            prev = c;
        }
        CHECK(sorted);
        CHECK(EffectLoadOrder(sf).names() == o.names());
        auto mg = lib.load("MeshGloss.fx");
        CHECK(mg && lib.effect_load_rank(*mg) >= engine_effect_load_sequence().size() && lib.effect_load_rank(*mg) < o.size());
        CHECK(lib.effect_load_rank("PrimOpaque") == 3);
    }
    auto scroll = lib.load("MeshAlphaScroll");
    if (scroll && !scroll->passes.empty() && scroll->passes[0].ff) {
        ParamStore store;
        store.set_semantic("TIME", {2.0f});
        store.set("UVScrollRate", {0.5f, 0.25f});
        FfDrawData d = build_ff_draw_data(*scroll->passes[0].ff, *scroll->effect, store.lookup(), false);
        // out.u = dot(row0, (u, v, 1, 0)) = u + time * rate.x
        CHECK(std::fabs(d.tex_transform[0][0].z - 1.0f) < 1e-6f && std::fabs(d.tex_transform[0][1].z - 0.5f) < 1e-6f);
        CHECK(d.tex_transform[0][0].x == 1.0f && d.tex_transform[0][1].y == 1.0f);
    }
}
#endif

}  // namespace

int main() {
    test_layouts();
    test_defaults_and_keys();
    test_phase_material_resolution();
    test_fade_and_alpha_test();
    test_samplers();
    test_fixed_function();
    test_params();
    test_spirv_registry();
    test_review_regressions();
    test_per_draw();
    test_hardening();
    test_default_effect_and_order();
    test_shader_issues();
    test_fx_sources();
    if (const char* root = std::getenv("ALAMO_FOC_DATA")) {
#ifdef ALAMO_MATERIALS_HAVE_VFS
        test_vanilla(root);
#else
        std::printf("  ALAMO_FOC_DATA set but built without alamo_vfs: vanilla sweep skipped\n");
#endif
    } else {
        std::printf("  ALAMO_FOC_DATA not set: vanilla FXO sweep skipped\n");
    }
    if (g_fail) {
        std::printf("alamo_materials_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_materials_tests: OK\n");
    return 0;
}

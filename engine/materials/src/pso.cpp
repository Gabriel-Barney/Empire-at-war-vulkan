// PsoKey / SamplerKey resolution. See pso.hpp.
#include "alamo/materials/pso.hpp"

#include <bit>
#include <cstring>

#include "alamo/core/hash.hpp"

namespace alamo::materials {

uint64_t hash(const PsoKey& k) { return alamo::fnv1a64(&k, sizeof k); }
uint64_t hash(const SamplerKey& k) { return alamo::fnv1a64(&k, sizeof k); }

namespace {
struct LayoutName { VertexLayout l; const char* name; };
constexpr LayoutName kLayouts[] = {
    {VertexLayout::N, "alD3dVertN"},
    {VertexLayout::NU2, "alD3dVertNU2"},
    {VertexLayout::NU2C, "alD3dVertNU2C"},
    {VertexLayout::NU2U3U3, "alD3dVertNU2U3U3"},
    {VertexLayout::NU2U3U3C, "alD3dVertNU2U3U3C"},
    {VertexLayout::RSkinNU2, "alD3dVertRSkinNU2"},
    {VertexLayout::RSkinNU2C, "alD3dVertRSkinNU2C"},
    {VertexLayout::RSkinNU2U3U3, "alD3dVertRSkinNU2U3U3"},
    {VertexLayout::PrimVert, "alPrimVert"},
    {VertexLayout::PrimVertLit, "alPrimVertLit"},
    {VertexLayout::Terrain, "Terrain"},
};

bool iequal(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

bool is_default(const StateSlot& s) { return s.origin == StateOrigin::Default; }
}  // namespace

VertexLayout vertex_layout_from_name(const char* n) {
    if (!n) return VertexLayout::Unknown;
    for (const auto& e : kLayouts)
        if (iequal(e.name, n)) return e.l;
    return VertexLayout::Unknown;
}

const char* vertex_layout_name(VertexLayout l) {
    for (const auto& e : kLayouts)
        if (e.l == l) return e.name;
    return "Unknown";
}

bool vertex_layout_skinned(VertexLayout l) {
    return l == VertexLayout::RSkinNU2 || l == VertexLayout::RSkinNU2C || l == VertexLayout::RSkinNU2U3U3;
}

BlendFactor to_blend_factor(uint32_t b) {
    switch (b) {
    case d3d::kZero: return BlendFactor::Zero;
    case d3d::kOne: return BlendFactor::One;
    case d3d::kSrcColor: return BlendFactor::SrcColor;
    case d3d::kInvSrcColor: return BlendFactor::OneMinusSrcColor;
    case d3d::kSrcAlpha: return BlendFactor::SrcAlpha;
    case d3d::kInvSrcAlpha: return BlendFactor::OneMinusSrcAlpha;
    case d3d::kDestAlpha: return BlendFactor::DstAlpha;
    case d3d::kInvDestAlpha: return BlendFactor::OneMinusDstAlpha;
    case d3d::kDestColor: return BlendFactor::DstColor;
    case d3d::kInvDestColor: return BlendFactor::OneMinusDstColor;
    case d3d::kSrcAlphaSat: return BlendFactor::SrcAlphaSaturate;
    case d3d::kBothSrcAlpha: return BlendFactor::SrcAlpha;          // dst handled by caller
    case d3d::kBothInvSrcAlpha: return BlendFactor::OneMinusSrcAlpha;
    case d3d::kBlendFactor: return BlendFactor::ConstantColor;
    case d3d::kInvBlendFactor: return BlendFactor::OneMinusConstantColor;
    default: return BlendFactor::One;
    }
}

BlendOp to_blend_op(uint32_t o) {
    return (o >= d3d::kAdd && o <= d3d::kMax) ? static_cast<BlendOp>(o - 1) : BlendOp::Add;
}
CompareOp to_compare(uint32_t c) {
    return (c >= d3d::kNever && c <= d3d::kAlways) ? static_cast<CompareOp>(c - 1) : CompareOp::Always;
}
StencilOp to_stencil_op(uint32_t s) {
    return (s >= d3d::kKeep && s <= d3d::kDecr) ? static_cast<StencilOp>(s - 1) : StencilOp::Keep;
}
CullMode to_cull(uint32_t c) {
    switch (c) {
    // Outward faces are counter-clockwise on screen = Vulkan front faces (pso.hpp header).
    case d3d::kCullNone: return CullMode::None;
    case d3d::kCullCW: return CullMode::Back;    // culls clockwise = inward faces
    default: return CullMode::Front;             // D3DCULL_CCW culls counter-clockwise faces
    }
}

namespace {
void blend_pair(uint32_t src, uint32_t dst, BlendFactor& s, BlendFactor& d) {
    s = to_blend_factor(src);
    d = to_blend_factor(dst);
    // D3DBLEND_BOTH*: SRCBLEND sets both factors, DESTBLEND is ignored.
    if (src == d3d::kBothSrcAlpha) d = BlendFactor::OneMinusSrcAlpha;
    if (src == d3d::kBothInvSrcAlpha) d = BlendFactor::SrcAlpha;
}
}  // namespace

PsoKey resolve_pso(const DeviceState& st, int alpha_blend_override) {
    PsoKey k;
    const StateSlot& abe = st.slot(RS::AlphaBlendEnable);
    uint32_t blend = abe.value;
    if (alpha_blend_override >= 0 && abe.origin == StateOrigin::Expression)
        blend = static_cast<uint32_t>(alpha_blend_override);
    k.blend_enable = blend ? 1 : 0;
    if (k.blend_enable) {
        blend_pair(st.get(RS::SrcBlend), st.get(RS::DestBlend), k.src_color, k.dst_color);
        k.color_op = to_blend_op(st.get(RS::BlendOp));
        if (st.get(RS::SeparateAlphaBlendEnable)) {
            blend_pair(st.get(RS::SrcBlendAlpha), st.get(RS::DestBlendAlpha), k.src_alpha, k.dst_alpha);
            k.alpha_op = to_blend_op(st.get(RS::BlendOpAlpha));
        } else {
            k.src_alpha = k.src_color;
            k.dst_alpha = k.dst_color;
            k.alpha_op = k.color_op;
        }
    }  // disabled: canonical factors so equal pipelines compare equal
    k.color_write_mask = static_cast<uint8_t>(st.get(RS::ColorWriteEnable) & 0xF);
    switch (st.get(RS::FillMode)) {
    case d3d::kPoint: k.polygon_mode = PolygonMode::Point; break;
    case d3d::kWireframe: k.polygon_mode = PolygonMode::Line; break;
    default: k.polygon_mode = PolygonMode::Fill; break;
    }
    k.cull = to_cull(st.get(RS::CullMode));
    k.depth_test = st.get(RS::ZEnable) ? 1 : 0;   // D3DZB_TRUE / D3DZB_USEW both enable
    k.depth_write = (k.depth_test && st.get(RS::ZWriteEnable)) ? 1 : 0;
    k.depth_compare = k.depth_test ? to_compare(st.get(RS::ZFunc)) : CompareOp::Always;
    k.stencil_enable = st.get(RS::StencilEnable) ? 1 : 0;
    if (k.stencil_enable) {
        // D3D9 STENCIL* = clockwise faces = Vulkan BACK; CCW_STENCIL* = Vulkan FRONT (pso.hpp).
        StencilFace cw;
        cw.fail_op = to_stencil_op(st.get(RS::StencilFail));
        cw.depth_fail_op = to_stencil_op(st.get(RS::StencilZFail));
        cw.pass_op = to_stencil_op(st.get(RS::StencilPass));
        cw.compare = to_compare(st.get(RS::StencilFunc));
        k.back = cw;
        if (st.get(RS::TwoSidedStencilMode)) {
            k.front.fail_op = to_stencil_op(st.get(RS::CcwStencilFail));
            k.front.depth_fail_op = to_stencil_op(st.get(RS::CcwStencilZFail));
            k.front.pass_op = to_stencil_op(st.get(RS::CcwStencilPass));
            k.front.compare = to_compare(st.get(RS::CcwStencilFunc));
        } else {
            k.front = cw;  // one-sided: the STENCIL* ops apply to every face
        }
    }
    k.depth_bias_enable = (st.get(RS::DepthBias) != 0 || st.get(RS::SlopeScaleDepthBias) != 0) ? 1 : 0;
    k.alpha_test = st.get(RS::AlphaTestEnable) ? to_compare(st.get(RS::AlphaFunc)) : CompareOp::Always;
    return k;
}

DynamicState resolve_dynamic(const DeviceState& st) {
    DynamicState d;
    d.stencil_ref = st.get(RS::StencilRef);
    d.stencil_read_mask = st.get(RS::StencilMask);
    d.stencil_write_mask = st.get(RS::StencilWriteMask);
    d.blend_factor = st.get(RS::BlendFactor);
    d.depth_bias = std::bit_cast<float>(st.get(RS::DepthBias));
    d.slope_scaled_depth_bias = std::bit_cast<float>(st.get(RS::SlopeScaleDepthBias));
    return d;
}

PushConstants resolve_push(const DeviceState& st) {
    PushConstants p;
    if (st.get(RS::AlphaTestEnable)) {
        p.alpha_func = static_cast<uint32_t>(to_compare(st.get(RS::AlphaFunc)));
        p.alpha_ref = st.get(RS::AlphaRef) & 0xFFu;
    }
    if (st.get(RS::FogEnable)) p.flags |= kPushFog;
    return p;
}

namespace {
Filter to_filter(uint32_t f) { return f == d3d::kFilterPoint ? Filter::Nearest : Filter::Linear; }
AddressMode to_address(uint32_t a) {
    switch (a) {
    case d3d::kMirror: return AddressMode::MirroredRepeat;
    case d3d::kClamp: return AddressMode::ClampToEdge;
    case d3d::kBorder: return AddressMode::ClampToBorder;
    case d3d::kMirrorOnce: return AddressMode::MirrorClampToEdge;
    default: return AddressMode::Repeat;
    }
}
}  // namespace

SamplerKey resolve_sampler(const DeviceState& st, int n, const SamplerKey& def) {
    SamplerKey k = def;
    k.from_effect = 0;
    if (n < 0 || n >= kMaxSamplers) return k;
    const auto& s = st.ss[static_cast<size_t>(n)];
    auto slot = [&](SS x) -> const StateSlot& { return s[static_cast<size_t>(x)]; };
    auto use = [&](SS x) {
        bool u = !is_default(slot(x));
        if (u) k.from_effect = 1;
        return u;
    };
    bool aniso = false;
    if (use(SS::MagFilter)) {
        k.mag = to_filter(slot(SS::MagFilter).value);
        aniso = aniso || slot(SS::MagFilter).value == d3d::kFilterAnisotropic;
    }
    if (use(SS::MinFilter)) {
        k.min = to_filter(slot(SS::MinFilter).value);
        aniso = aniso || slot(SS::MinFilter).value == d3d::kFilterAnisotropic;
    }
    if (use(SS::MipFilter)) {
        uint32_t m = slot(SS::MipFilter).value;
        k.mip_enable = m == d3d::kFilterNone ? 0 : 1;
        k.mip = m == d3d::kFilterPoint ? MipmapMode::Nearest : MipmapMode::Linear;
    }
    if (use(SS::AddressU)) k.u = to_address(slot(SS::AddressU).value);
    if (use(SS::AddressV)) k.v = to_address(slot(SS::AddressV).value);
    if (use(SS::AddressW)) k.w = to_address(slot(SS::AddressW).value);
    if (use(SS::BorderColor)) k.border_color = slot(SS::BorderColor).value;
    if (use(SS::MipMapLodBias)) k.mip_lod_bias_bits = slot(SS::MipMapLodBias).value;
    if (use(SS::MaxMipLevel)) k.max_mip_level = static_cast<uint8_t>(slot(SS::MaxMipLevel).value & 0xFF);
    if (use(SS::MaxAnisotropy)) {
        uint32_t a = slot(SS::MaxAnisotropy).value;
        k.max_anisotropy = static_cast<uint8_t>(a > 16 ? 16 : (a == 0 ? 1 : a));
    }
    if (!aniso && (!is_default(slot(SS::MagFilter)) || !is_default(slot(SS::MinFilter))) &&
        is_default(slot(SS::MaxAnisotropy)))
        k.max_anisotropy = 1;  // explicit LINEAR/POINT filters disable the engine's anisotropy
    return k;
}

}  // namespace alamo::materials

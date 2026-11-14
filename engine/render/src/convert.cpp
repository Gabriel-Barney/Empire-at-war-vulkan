#include "alamo/render/convert.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>

#include "alamo/core/ascii.hpp"
#include "alamo/core/log.hpp"

namespace alamo::render {

rhi::Format to_rhi_format(assets::TextureFormat f) {
    using TF = assets::TextureFormat;
    switch (f) {
    case TF::BC1_RGBA: return rhi::Format::BC1;
    case TF::BC2: return rhi::Format::BC2;
    case TF::BC3: return rhi::Format::BC3;
    case TF::RGBA8: return rhi::Format::RGBA8;
    case TF::BGRA8: return rhi::Format::BGRA8;
    case TF::R8: return rhi::Format::R8;
    case TF::R8G8: return rhi::Format::R8G8;
    case TF::Unknown: break;
    }
    return rhi::Format::Undefined;
}

bool to_rhi_swizzle(const assets::ComponentMapping& m, rhi::Swizzle& out) {
    using S = assets::Swizzle;
    auto is = [&](S r, S g, S b, S a) { return m.r == r && m.g == g && m.b == b && m.a == a; };
    out = rhi::Swizzle::Identity;
    if (is(S::R, S::G, S::B, S::A)) return true;
    if (is(S::R, S::R, S::R, S::One)) return out = rhi::Swizzle::RRR1, true;
    if (is(S::Zero, S::Zero, S::Zero, S::R)) return out = rhi::Swizzle::A8, true;
    if (is(S::R, S::R, S::R, S::G)) return out = rhi::Swizzle::RRRG, true;
    if (is(S::R, S::G, S::B, S::One)) return out = rhi::Swizzle::X8, true;
    return false;
}

namespace {

using ML = materials::VertexLayout;

struct NameMap {
    const char* name;
    ML layout;
};
// The 10 names of assets model.hpp (+ the materials-only RSkinNU2C).
constexpr NameMap kNames[] = {
    {"alD3dVertN", ML::N},
    {"alD3dVertNU2", ML::NU2},
    {"alD3dVertNU2C", ML::NU2C},
    {"alD3dVertNU2U3U3", ML::NU2U3U3},
    {"alD3dVertNU2U3U3C", ML::NU2U3U3C},
    {"alD3dVertGrass", ML::NU2},  // normal + uv0; the bend is a shader (Grass effect) matter
    {"alD3dVertRSkinNU2", ML::RSkinNU2},
    {"alD3dVertRSkinNU2C", ML::RSkinNU2C},
    {"alD3dVertRSkinNU2U3U3", ML::RSkinNU2U3U3},
    {"alD3dVertB4I4NU2", ML::RSkinNU2},          // legacy EaW 4-bone name, weights 1,0,0,0
    {"alD3dVertB4I4NU2U3U3", ML::RSkinNU2U3U3},
};

ML from_traits(const assets::VertexFormatTraits& t) {
    if (t.skinned) {
        if (t.tangent_frame) return ML::RSkinNU2U3U3;
        return t.color ? ML::RSkinNU2C : ML::RSkinNU2;
    }
    if (t.tangent_frame) return t.color ? ML::NU2U3U3C : ML::NU2U3U3;
    if (t.uv_sets > 0) return t.color ? ML::NU2C : ML::NU2;
    return ML::N;
}

}  // namespace

materials::VertexLayout vertex_layout_for(std::string_view name, const assets::VertexFormatTraits& traits) {
    for (const NameMap& n : kNames)
        if (ascii_iequals(name, n.name)) return n.layout;
    if (!traits.known && !traits.skinned && !traits.tangent_frame && traits.uv_sets == 0 && !traits.color &&
        !ascii_icontains(name, "alD3dVert"))
        return ML::Unknown;
    return from_traits(traits);
}

materials::VertexLayout vertex_layout_for(std::string_view name) {
    assets::VertexFormatTraits t;
    t.known = false;
    t.skinned = ascii_icontains(name, "RSkin") || ascii_icontains(name, "B4I4");
    t.tangent_frame = ascii_icontains(name, "U3U3");
    t.uv_sets = ascii_icontains(name, "U2") ? 1 : 0;
    t.color = !name.empty() && (name.back() == 'C' || name.back() == 'c') && ascii_icontains(name, "alD3dVert");
    return vertex_layout_for(name, t);
}

assets::VertexLayout assets_layout_for(materials::VertexLayout l) {
    return materials::vertex_layout_skinned(l) ? assets::VertexLayout::Skinned : assets::VertexLayout::Static;
}

uint32_t anisotropy_level(uint32_t level, uint32_t device_cap) {
    uint32_t a = level < 16u ? level : 16u;
    if (a > device_cap) a = device_cap;
    return a > 1 ? a : 0u;
}

rhi::SamplerKey to_rhi_sampler(const materials::SamplerKey& k, uint32_t max_anisotropy_cap, uint32_t anisotropy_override) {
    // [port] bits 8..15 of the override: Texture detail = top mip levels skipped (Renderer::set_texture_reduction).
    const uint32_t mip_skip = (anisotropy_override >> 8) & 0xFFu;
    anisotropy_override &= 0xFFu;
    rhi::SamplerKey r;
    r.mag_filter = static_cast<VkFilter>(k.mag);
    r.min_filter = static_cast<VkFilter>(k.min);
    r.mip_mode = static_cast<VkSamplerMipmapMode>(k.mip);
    r.address_u = static_cast<VkSamplerAddressMode>(k.u);
    r.address_v = static_cast<VkSamplerAddressMode>(k.v);
    r.address_w = static_cast<VkSamplerAddressMode>(k.w);
    // [port] The renderer-wide setting raises a linearly minified key to at least its level; point
    // minification (Tree, UI-style lookups) keeps the material's state.
    uint32_t want = k.max_anisotropy;
    if (anisotropy_override > 1 && k.min == materials::Filter::Linear && anisotropy_override > want) want = anisotropy_override;
    if (want > 1) {
        const uint32_t a = want < max_anisotropy_cap ? want : max_anisotropy_cap;
        r.max_anisotropy = static_cast<uint8_t>(a > 1 ? a : 0);
    } else {
        r.max_anisotropy = 0;
    }
    // D3DCOLOR ARGB -> nearest of Vulkan's fixed border colours.
    const uint32_t a = (k.border_color >> 24) & 0xFFu;
    const uint32_t rgb = ((k.border_color >> 16) & 0xFFu) + ((k.border_color >> 8) & 0xFFu) + (k.border_color & 0xFFu);
    if (a < 128) r.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else r.border = rgb >= 3 * 128 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    float bias = 0.0f;
    std::memcpy(&bias, &k.mip_lod_bias_bits, sizeof bias);
    r.lod_bias = bias == bias ? bias : 0.0f;  // NaN -> 0
    // D3DSAMP_MAXMIPLEVEL = most detailed level allowed -> minLod. No mip filter -> level 0 only.
    r.min_lod = static_cast<float>(k.max_mip_level);
    if (mip_skip > 0 && k.min == materials::Filter::Linear && k.mip_enable) r.min_lod = std::max(r.min_lod, static_cast<float>(mip_skip));
    r.max_lod = k.mip_enable ? VK_LOD_CLAMP_NONE : r.min_lod;
    if (!k.mip_enable) r.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return r;
}

namespace {

uint32_t location_of(assets::VertexSemantic s) {
    switch (s) {
    case assets::VertexSemantic::Position: return materials::kLocPosition;
    case assets::VertexSemantic::Normal: return materials::kLocNormal;
    case assets::VertexSemantic::TexCoord0: return materials::kLocTexcoord0;
    case assets::VertexSemantic::Tangent: return materials::kLocTangent;
    case assets::VertexSemantic::Binormal: return materials::kLocBinormal;
    case assets::VertexSemantic::Color: return materials::kLocColor0;
    }
    return UINT32_MAX;
}
uint32_t vk_format_of(assets::AttribFormat f) {
    switch (f) {
    case assets::AttribFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case assets::AttribFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case assets::AttribFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case assets::AttribFormat::UNorm8x4: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

}  // namespace

bool vertex_tables_agree(materials::VertexLayout l) {
    const materials::VertexLayoutDesc md = materials::vertex_layout_desc(l);
    if (md.stride == 0 || md.attributes.empty()) return false;
    const assets::VertexLayout al = assets_layout_for(l);
    if (md.stride != assets::vertex_stride(al)) return false;
    const auto attrs = assets::vertex_attributes(al);
    if (attrs.size() != md.attributes.size()) return false;
    for (const materials::VertexAttributeDesc& m : md.attributes) {
        bool found = false;
        for (const assets::VertexAttribute& a : attrs) {
            if (location_of(a.semantic) != m.location) continue;
            found = a.offset == m.offset && vk_format_of(a.format) == m.vk_format;
        }
        if (!found) return false;
    }
    return true;
}

namespace {
// Render-owned vertex streams (not ALO meshes): the engine's pre-lit prim vertex used by the
// stencil-darken pass (TERRAIN_WATER_POST.md 4.2: XYZ|DIFFUSE|TEX1, 24 bytes, D3DCOLOR = BGRA8) and
// the FLOAT2-only full-screen quad.
constexpr materials::VertexAttributeDesc kPrimAttrs[] = {
    {materials::kLocPosition, materials::vkfmt::kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 0},
    {materials::kLocColor0, materials::vkfmt::kB8G8R8A8Unorm, "VK_FORMAT_B8G8R8A8_UNORM", 12},
    {materials::kLocTexcoord0, materials::vkfmt::kR32G32Sfloat, "VK_FORMAT_R32G32_SFLOAT", 16},
};
constexpr materials::VertexAttributeDesc kFullScreenAttrs[] = {
    {materials::kLocPosition, materials::vkfmt::kR32G32Sfloat, "VK_FORMAT_R32G32_SFLOAT", 0},
};
constexpr uint32_t kLocColor1 = 13;  // shaderxlat vertex_location(Color, 1)
constexpr materials::VertexAttributeDesc kWaterAttrs[] = {
    {materials::kLocPosition, materials::vkfmt::kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 0},
    {materials::kLocNormal, materials::vkfmt::kR32G32B32Sfloat, "VK_FORMAT_R32G32B32_SFLOAT", 12},
    {materials::kLocColor0, materials::vkfmt::kB8G8R8A8Unorm, "VK_FORMAT_B8G8R8A8_UNORM", 24},
    {kLocColor1, materials::vkfmt::kB8G8R8A8Unorm, "VK_FORMAT_B8G8R8A8_UNORM", 28},
};
}  // namespace

bool render_layout_desc(materials::VertexLayout l, materials::VertexLayoutDesc& out) {
    if (l == materials::VertexLayout::PrimVert) {
        out = {24, kPrimAttrs};
        return true;
    }
    if (l == kFullScreenLayout) {
        out = {8, kFullScreenAttrs};
        return true;
    }
    if (l == kWaterLayout) {
        out = {32, kWaterAttrs};
        return true;
    }
    if (l == materials::VertexLayout::PrimVertLit) {  // terrain / decoration lit prim vertices (no assets table)
        out = materials::vertex_layout_desc(l);
        return true;
    }
    return false;
}

materials::VertexLayoutDesc effective_layout_desc(materials::VertexLayout l) {
    materials::VertexLayoutDesc d;
    if (render_layout_desc(l, d)) return d;
    return materials::vertex_layout_desc(l);
}

bool build_vertex_input(materials::VertexLayout l, std::span<const uint32_t> locations, VertexInputDesc& out) {
    out = {};
    materials::VertexLayoutDesc rd;
    if (render_layout_desc(l, rd)) {  // render-owned stream: no assets table to agree with
        out.binding = {0, rd.stride, false};
        for (const materials::VertexAttributeDesc& m : rd.attributes) {
            bool wanted = locations.empty();
            for (uint32_t loc : locations) wanted |= loc == m.location;
            if (wanted) out.attributes.push_back({m.location, 0, static_cast<VkFormat>(m.vk_format), m.offset});
        }
        for (uint32_t loc : locations) {
            bool have = false;
            for (const rhi::VertexAttribute& a : out.attributes) have |= a.location == loc;
            if (!have) return false;
        }
        return true;
    }
    const bool agree = vertex_tables_agree(l);
    assert(agree && "materials::vertex_layout_desc disagrees with assets::vertex_attributes");
    if (!agree) {
        log::errorf("render", "vertex layout %s: materials and assets attribute tables disagree",
                    materials::vertex_layout_name(l));
        return false;
    }
    const materials::VertexLayoutDesc md = materials::vertex_layout_desc(l);
    out.binding.binding = 0;
    out.binding.stride = md.stride;
    out.binding.per_instance = false;
    for (const materials::VertexAttributeDesc& m : md.attributes) {
        bool wanted = locations.empty();
        for (uint32_t loc : locations) wanted |= loc == m.location;
        if (!wanted) continue;
        rhi::VertexAttribute a;
        a.location = m.location;
        a.binding = 0;
        a.format = static_cast<VkFormat>(m.vk_format);
        a.offset = m.offset;
        out.attributes.push_back(a);
    }
    for (uint32_t loc : locations) {
        bool have = false;
        for (const rhi::VertexAttribute& a : out.attributes) have |= a.location == loc;
        if (!have) {
            log::errorf("render", "vertex layout %s has no attribute at location %u", materials::vertex_layout_name(l), loc);
            return false;
        }
    }
    return true;
}

}  // namespace alamo::render

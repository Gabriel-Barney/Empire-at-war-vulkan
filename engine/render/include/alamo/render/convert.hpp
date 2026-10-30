// alamo_render - integration conversions between the three wave-1 modules (deliberately left to
// the renderer): assets texture formats / swizzles -> rhi, ALO vertex-format names -> materials
// VertexLayout, materials SamplerKey -> rhi SamplerKey, and the Vulkan vertex input of a layout
// built from materials::vertex_layout_desc() and checked against assets' vertex_attributes().
#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "alamo/assets/model.hpp"
#include "alamo/assets/texture.hpp"
#include "alamo/assets/vertex_layout.hpp"
#include "alamo/materials/programs.hpp"
#include "alamo/materials/pso.hpp"
#include "alamo/rhi/format.hpp"
#include "alamo/rhi/pipeline.hpp"
#include "alamo/rhi/sampler.hpp"

namespace alamo::render {

// assets::TextureFormat -> rhi::Format (Undefined for TextureFormat::Unknown).
rhi::Format to_rhi_format(assets::TextureFormat f);

// assets::ComponentMapping -> rhi::Swizzle. Returns false (out = Identity) for a mapping that rhi
// has no view swizzle for. Recognised: identity, L8 (R,R,R,One) -> RRR1, A8 (Zero,Zero,Zero,R) ->
// A8, A8L8 (R,R,R,G) -> RRRG, X8 (R,G,B,One) -> X8.
bool to_rhi_swizzle(const assets::ComponentMapping& m, rhi::Swizzle& out);

// ALO vertex-format name (Submesh::vertex_format, 0x10002) -> materials VertexLayout. Covers the 10
// names of assets model.hpp: exact materials names map 1:1; alD3dVertGrass -> NU2 (static,
// normal + uv0); legacy EaW alD3dVertB4I4NU2 / B4I4NU2U3U3 (loaded rigid by assets) -> RSkinNU2 /
// RSkinNU2U3U3. Anything else falls back on the decoded traits (skinned / tangent frame / colour /
// uv), and Unknown only if `traits.known` is false and nothing usable was decoded.
materials::VertexLayout vertex_layout_for(std::string_view vertex_format, const assets::VertexFormatTraits& traits);
// Same, from the name only (traits inferred like assets does: tokens U2, U3U3, RSkin/B4I4, trailing C).
materials::VertexLayout vertex_layout_for(std::string_view vertex_format);

// The assets layout a materials layout loads into (Static for every non-RSkin mesh layout).
assets::VertexLayout assets_layout_for(materials::VertexLayout l);

// materials::SamplerKey (D3D9-derived, Vk-valued enums) -> rhi::SamplerKey. `max_anisotropy_cap`
// limits an anisotropic key (the engine quality setting / device limit). [port] `anisotropy_override`
// (Renderer::set_anisotropy; 0/1 = off): a key with linear minification samples with at least this
// many taps (max of the key's and the override), still capped; point-minified keys are left alone. Bits 8..15 of
// `anisotropy_override` are the texture reduction: that many top mip levels are skipped (minLod raised to it) for the
// same linearly minified, mip-mapped keys.
rhi::SamplerKey to_rhi_sampler(const materials::SamplerKey& k, uint32_t max_anisotropy_cap = 16, uint32_t anisotropy_override = 0);
// [port] A requested anisotropy level (0/1 = off, else 2..16) clamped to 16 and to `device_cap`; 0 when off
// (or the device caps it at 1).
uint32_t anisotropy_level(uint32_t level, uint32_t device_cap);

// Vulkan vertex input for a materials layout restricted to `locations` (the inputs the shader
// declares; an empty span = every attribute). Asserts that stride and every offset/format agree
// with assets::vertex_attributes() of the matching assets layout; returns false (and logs) on a
// mismatch in release builds too.
struct VertexInputDesc {
    rhi::VertexBinding binding;
    std::vector<rhi::VertexAttribute> attributes;
};
bool build_vertex_input(materials::VertexLayout l, std::span<const uint32_t> locations, VertexInputDesc& out);
// Render-owned streams keyed like mesh layouts in PsoKey::vertex_layout: PrimVert = the engine's 24-byte
// pre-lit prim vertex (float3 position, D3DCOLOR BGRA8 colour, float2 uv), kFullScreenLayout = the
// FLOAT2 full-screen quad. effective_layout_desc() = these, else materials::vertex_layout_desc().
inline constexpr materials::VertexLayout kFullScreenLayout = static_cast<materials::VertexLayout>(0xF0);
// Water patch vertex (terrain::WaterVertex, alD3dVertNC, 32 B): float3 position, float3 normal, COLOR0 and
// COLOR1 as D3DCOLOR BGRA8 (COLOR1 at the translated VS input location 13).
inline constexpr materials::VertexLayout kWaterLayout = static_cast<materials::VertexLayout>(0xF1);
bool render_layout_desc(materials::VertexLayout l, materials::VertexLayoutDesc& out);
materials::VertexLayoutDesc effective_layout_desc(materials::VertexLayout l);
// The check alone: materials table vs assets table for one layout. False on any mismatch.
bool vertex_tables_agree(materials::VertexLayout l);

}  // namespace alamo::render

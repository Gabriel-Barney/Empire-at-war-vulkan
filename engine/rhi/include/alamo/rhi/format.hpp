// rhi::Format: the texel formats the engine uploads or renders to, with size math for mip chains.
// Texture data is expected tightly packed: rows of whole blocks (4x4 for BC), mip 0 first.
//
// D3D9 / DDS mapping (what engine/assets produces from the game's DDS/TGA files):
//   DXT1 -> BC1 (1-bit alpha variant; RGB-only DXT1 decodes the same with alpha = 1)
//   DXT2/DXT3 -> BC2, DXT4/DXT5 -> BC3 (premultiplied variants are only a flag in D3D)
//   A8R8G8B8 / X8R8G8B8 (bytes B,G,R,A in memory) -> BGRA8; A8B8G8R8 -> RGBA8
//   L8 / A8 / P8-expanded -> R8 (swizzle in the view or shader), A8L8 -> R8G8
//   R5G6B5 -> R5G6B5, A1R5G5B5 -> A1R5G5B5, A4R4G4B4 -> A4R4G4B4 (same bit layouts as Vulkan *_PACK16;
//   A4R4G4B4 is optional in Vulkan 1.3, check Context::format_supported or convert to BGRA8).
// Everything is UNORM, never sRGB, for fidelity with the original gamma-space pipeline (PORTING 3.2).
#pragma once
#include <cstdint>

#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

enum class Format : uint8_t {
    Undefined = 0,
    R8,
    R8G8,
    RGBA8,
    BGRA8,
    RGBA8_SRGB,
    BGRA8_SRGB,
    R5G6B5,
    A1R5G5B5,
    A4R4G4B4,
    BC1,
    BC2,
    BC3,
    RGBA16F,
    R32F,
    RG32F,
    RGBA32F,
    B4G4R4A4,  // VK B4G4R4A4_UNORM_PACK16 (mandatory sampled format)
    R4G4B4A4,  // VK R4G4B4A4_UNORM_PACK16
    D16,
    D24S8,
    D32F,
    D32FS8,
    Count
};

struct FormatInfo {
    const char* name;
    VkFormat vk;
    uint8_t block_w, block_h;   // 1x1 for uncompressed, 4x4 for BC
    uint8_t bytes_per_block;    // bytes per texel for uncompressed
    bool compressed;
    bool depth, stencil;
};

const FormatInfo& format_info(Format f);  // Undefined/out of range -> the Undefined row
inline VkFormat to_vk(Format f) { return format_info(f).vk; }
Format from_vk(VkFormat f);  // Undefined if not in the table

// Extent of mip `level` (never 0).
inline constexpr uint32_t mip_extent(uint32_t base, uint32_t level) {
    const uint32_t v = level >= 32 ? 0u : (base >> level);
    return v ? v : 1u;
}
// Full chain length for a w x h image: floor(log2(max(w,h))) + 1.
uint32_t max_mip_count(uint32_t width, uint32_t height);

// Bytes in one tightly packed row of blocks, number of block rows, and bytes of one 2D subresource.
uint64_t row_pitch(Format f, uint32_t width);
uint32_t block_rows(Format f, uint32_t height);
uint64_t subresource_size(Format f, uint32_t width, uint32_t height);
// 3D: bytes of one mip of a volume (slices tightly packed, slice 0 first) and of a full 3D chain.
uint64_t volume_mip_size(Format f, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip);
uint64_t volume_chain_size(Format f, uint32_t width, uint32_t height, uint32_t depth, uint32_t mips);
uint32_t max_mip_count_3d(uint32_t width, uint32_t height, uint32_t depth);
// Bytes of `mips` levels x `layers` layers (layer-major: all mips of layer 0, then layer 1...).
uint64_t mip_chain_size(Format f, uint32_t width, uint32_t height, uint32_t mips, uint32_t layers = 1);

inline VkImageAspectFlags aspect_mask(Format f) {
    const FormatInfo& i = format_info(f);
    if (i.depth || i.stencil) {
        VkImageAspectFlags m = 0;
        if (i.depth) m |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (i.stencil) m |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return m;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

// View swizzles for D3D9 formats that Vulkan stores in fewer channels.
enum class Swizzle : uint8_t {
    Identity,
    RRR1,  // L8 stored as R8            -> (L, L, L, 1)
    RRRG,  // A8L8 stored as R8G8 (R=L, G=A) -> (L, L, L, A)
    A8,    // A8 stored as R8            -> (0, 0, 0, A)
    X8,    // X8R8G8B8 stored as BGRA8   -> (R, G, B, 1)
    // D3D A4R4G4B4 bits (A15:12 R11:8 G7:4 B3:0) stored unchanged in a B4G4R4A4 image, whose
    // channels then hold B=A, G=R, R=G, A=B: the view maps them back -> (G, R, A, B).
    ARGB4_IN_BGRA4,
};
VkComponentMapping component_mapping(Swizzle s);

// D3D9 depth bias -> vkCmdSetDepthBias factors. D3DRS_DEPTHBIAS is an offset in normalized depth
// units; Vulkan's constant factor is in units of the format's minimum resolvable difference r:
//   UNORM D16 / D24: r = 2^-16 / 2^-24 (exact).
//   Float D32: r = 2^(e - 23) with e the exponent of the primitive's max depth. We use the value for
//   depths in [0.5, 1) (r = 2^-24), where nearly all of a perspective depth range lies; nearer
//   geometry gets a proportionally smaller offset. (VK_EXT_depth_bias_control could make it exact.)
// SLOPESCALEDEPTHBIAS maps 1:1 to the slope factor.
struct DepthBias {
    float constant_factor = 0.0f;
    float slope_factor = 0.0f;
    float clamp = 0.0f;
};
DepthBias depth_bias_from_d3d9(float d3d_bias, float d3d_slope_scale, Format depth_format);

}  // namespace alamo::rhi

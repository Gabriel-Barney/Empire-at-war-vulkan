#include "alamo/rhi/format.hpp"

namespace alamo::rhi {

namespace {

constexpr FormatInfo kFormats[] = {
    {"Undefined", VK_FORMAT_UNDEFINED, 1, 1, 0, false, false, false},
    {"R8", VK_FORMAT_R8_UNORM, 1, 1, 1, false, false, false},
    {"R8G8", VK_FORMAT_R8G8_UNORM, 1, 1, 2, false, false, false},
    {"RGBA8", VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, false, false, false},
    {"BGRA8", VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 4, false, false, false},
    {"RGBA8_SRGB", VK_FORMAT_R8G8B8A8_SRGB, 1, 1, 4, false, false, false},
    {"BGRA8_SRGB", VK_FORMAT_B8G8R8A8_SRGB, 1, 1, 4, false, false, false},
    {"R5G6B5", VK_FORMAT_R5G6B5_UNORM_PACK16, 1, 1, 2, false, false, false},
    {"A1R5G5B5", VK_FORMAT_A1R5G5B5_UNORM_PACK16, 1, 1, 2, false, false, false},
    {"A4R4G4B4", VK_FORMAT_A4R4G4B4_UNORM_PACK16, 1, 1, 2, false, false, false},
    {"BC1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8, true, false, false},
    {"BC2", VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16, true, false, false},
    {"BC3", VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16, true, false, false},
    {"RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8, false, false, false},
    {"R32F", VK_FORMAT_R32_SFLOAT, 1, 1, 4, false, false, false},
    {"RG32F", VK_FORMAT_R32G32_SFLOAT, 1, 1, 8, false, false, false},
    {"RGBA32F", VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 16, false, false, false},
    {"B4G4R4A4", VK_FORMAT_B4G4R4A4_UNORM_PACK16, 1, 1, 2, false, false, false},
    {"R4G4B4A4", VK_FORMAT_R4G4B4A4_UNORM_PACK16, 1, 1, 2, false, false, false},
    {"D16", VK_FORMAT_D16_UNORM, 1, 1, 2, false, true, false},
    {"D24S8", VK_FORMAT_D24_UNORM_S8_UINT, 1, 1, 4, false, true, true},
    {"D32F", VK_FORMAT_D32_SFLOAT, 1, 1, 4, false, true, false},
    {"D32FS8", VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 1, 8, false, true, true},
};
static_assert(sizeof(kFormats) / sizeof(kFormats[0]) == static_cast<size_t>(Format::Count),
              "format table out of sync with rhi::Format");

}  // namespace

const FormatInfo& format_info(Format f) {
    const auto i = static_cast<size_t>(f);
    return i < static_cast<size_t>(Format::Count) ? kFormats[i] : kFormats[0];
}

Format from_vk(VkFormat f) {
    for (size_t i = 1; i < static_cast<size_t>(Format::Count); ++i)
        if (kFormats[i].vk == f) return static_cast<Format>(i);
    return Format::Undefined;
}

uint32_t max_mip_count(uint32_t width, uint32_t height) {
    uint32_t m = width > height ? width : height;
    uint32_t n = 1;
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

uint64_t row_pitch(Format f, uint32_t width) {
    const FormatInfo& i = format_info(f);
    const uint64_t blocks = (static_cast<uint64_t>(width) + i.block_w - 1) / i.block_w;
    return blocks * i.bytes_per_block;
}

uint32_t block_rows(Format f, uint32_t height) {
    const FormatInfo& i = format_info(f);
    return (height + i.block_h - 1u) / i.block_h;
}

uint64_t subresource_size(Format f, uint32_t width, uint32_t height) {
    return row_pitch(f, width) * block_rows(f, height);
}

uint64_t mip_chain_size(Format f, uint32_t width, uint32_t height, uint32_t mips, uint32_t layers) {
    uint64_t one = 0;
    for (uint32_t m = 0; m < mips; ++m) one += subresource_size(f, mip_extent(width, m), mip_extent(height, m));
    return one * layers;
}

uint64_t volume_mip_size(Format f, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip) {
    return subresource_size(f, mip_extent(width, mip), mip_extent(height, mip)) * mip_extent(depth, mip);
}

uint64_t volume_chain_size(Format f, uint32_t width, uint32_t height, uint32_t depth, uint32_t mips) {
    uint64_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) total += volume_mip_size(f, width, height, depth, m);
    return total;
}

uint32_t max_mip_count_3d(uint32_t width, uint32_t height, uint32_t depth) {
    return max_mip_count(width > height ? width : height, depth);
}

VkComponentMapping component_mapping(Swizzle s) {
    constexpr VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_IDENTITY, R = VK_COMPONENT_SWIZZLE_R,
                                 G = VK_COMPONENT_SWIZZLE_G, Z = VK_COMPONENT_SWIZZLE_ZERO,
                                 O = VK_COMPONENT_SWIZZLE_ONE;
    switch (s) {
        case Swizzle::Identity: break;
        case Swizzle::RRR1: return {R, R, R, O};
        case Swizzle::RRRG: return {R, R, R, G};
        case Swizzle::A8: return {Z, Z, Z, R};
        case Swizzle::X8: return {I, I, I, O};
        case Swizzle::ARGB4_IN_BGRA4: return {G, R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B};
    }
    return {I, I, I, I};
}

DepthBias depth_bias_from_d3d9(float d3d_bias, float d3d_slope_scale, Format depth_format) {
    float units = 16777216.0f;  // 2^24: D24 and float depth in [0.5, 1)
    if (depth_format == Format::D16) units = 65536.0f;
    DepthBias b;
    b.constant_factor = d3d_bias * units;
    b.slope_factor = d3d_slope_scale;
    b.clamp = 0.0f;
    return b;
}

}  // namespace alamo::rhi

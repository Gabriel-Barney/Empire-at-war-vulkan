#include "alamo/rhi/sampler.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

namespace {

uint64_t bits(uint64_t v, uint64_t mask, int shift) { return (v & mask) << shift; }
uint32_t field(uint64_t p, uint64_t mask, int shift) { return static_cast<uint32_t>((p >> shift) & mask); }

int32_t quantize(float v, float scale, int32_t lo, int32_t hi) {
    if (!(v == v)) return lo;  // NaN
    const double q = std::floor(static_cast<double>(v) * scale + 0.5);
    if (q < lo) return lo;
    if (q > hi) return hi;
    return static_cast<int32_t>(q);
}

}  // namespace

uint64_t SamplerKey::pack() const {
    uint64_t p = 0;
    p |= bits(min_filter == VK_FILTER_LINEAR ? 1u : 0u, 1, 0);
    p |= bits(mag_filter == VK_FILTER_LINEAR ? 1u : 0u, 1, 1);
    p |= bits(mip_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? 1u : 0u, 1, 2);
    p |= bits(static_cast<uint64_t>(std::min<uint32_t>(static_cast<uint32_t>(address_u), 4u)), 7, 3);
    p |= bits(static_cast<uint64_t>(std::min<uint32_t>(static_cast<uint32_t>(address_v), 4u)), 7, 6);
    p |= bits(static_cast<uint64_t>(std::min<uint32_t>(static_cast<uint32_t>(address_w), 4u)), 7, 9);
    const uint32_t aniso = max_anisotropy <= 1 ? 0u : std::min<uint32_t>(max_anisotropy, 16u);
    p |= bits(aniso, 31, 12);
    p |= bits(compare_enable ? 1u : 0u, 1, 17);
    p |= bits(compare_enable ? static_cast<uint64_t>(compare_op) : 0u, 7, 18);
    p |= bits(static_cast<uint64_t>(std::min<uint32_t>(static_cast<uint32_t>(border), 5u)), 7, 21);
    const int32_t bias = quantize(lod_bias, 256.0f, -32768, 32767);
    p |= bits(static_cast<uint16_t>(static_cast<int16_t>(bias)), 0xFFFF, 24);
    p |= bits(static_cast<uint32_t>(quantize(min_lod, 16.0f, 0, 255)), 0xFF, 40);
    const uint32_t maxl = max_lod >= 16.0f ? 255u : static_cast<uint32_t>(quantize(max_lod, 16.0f, 0, 254));
    p |= bits(maxl, 0xFF, 48);
    return p;
}

SamplerKey SamplerKey::unpack(uint64_t p) {
    SamplerKey k;
    k.min_filter = field(p, 1, 0) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    k.mag_filter = field(p, 1, 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    k.mip_mode = field(p, 1, 2) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    k.address_u = static_cast<VkSamplerAddressMode>(field(p, 7, 3));
    k.address_v = static_cast<VkSamplerAddressMode>(field(p, 7, 6));
    k.address_w = static_cast<VkSamplerAddressMode>(field(p, 7, 9));
    k.max_anisotropy = static_cast<uint8_t>(field(p, 31, 12));
    k.compare_enable = field(p, 1, 17) != 0;
    k.compare_op = k.compare_enable ? static_cast<VkCompareOp>(field(p, 7, 18)) : VK_COMPARE_OP_LESS_OR_EQUAL;
    k.border = static_cast<VkBorderColor>(field(p, 7, 21));
    k.lod_bias = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(field(p, 0xFFFF, 24)))) / 256.0f;
    k.min_lod = static_cast<float>(field(p, 0xFF, 40)) / 16.0f;
    const uint32_t maxl = field(p, 0xFF, 48);
    k.max_lod = maxl == 255 ? VK_LOD_CLAMP_NONE : static_cast<float>(maxl) / 16.0f;
    return k;
}

uint64_t SamplerKey::hash() const {
    uint64_t z = pack() + 0x9e3779b97f4a7c15ull;  // splitmix64 finalizer
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

SamplerKey SamplerKey::linear_clamp() {
    SamplerKey k;
    k.address_u = k.address_v = k.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return k;
}
SamplerKey SamplerKey::point_wrap() {
    SamplerKey k;
    k.min_filter = k.mag_filter = VK_FILTER_NEAREST;
    k.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return k;
}
SamplerKey SamplerKey::point_clamp() {
    SamplerKey k = point_wrap();
    k.address_u = k.address_v = k.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return k;
}

SamplerCache::~SamplerCache() { shutdown(); }

void SamplerCache::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_)
        for (auto& kv : map_) ctx_->vkd().vkDestroySampler(ctx_->device(), kv.second, nullptr);
    map_.clear();
}

size_t SamplerCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

VkSampler SamplerCache::get(const SamplerKey& key) {
    const uint64_t packed = key.pack();
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = map_.find(packed); it != map_.end()) return it->second;
    const SamplerKey k = SamplerKey::unpack(packed);  // create from the canonical (quantized) form
    auto ci = vk_struct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    ci.magFilter = k.mag_filter;
    ci.minFilter = k.min_filter;
    ci.mipmapMode = k.mip_mode;
    const DeviceCaps& caps = ctx_->caps();
    // MIRROR_CLAMP_TO_EDGE (D3DTADDRESS_MIRRORONCE) needs samplerMirrorClampToEdge; without it fall
    // back to MIRRORED_REPEAT, which matches it on [-1, 1] (the usual MIRRORONCE use) and only
    // differs by repeating the mirror outside that range.
    auto address = [&](VkSamplerAddressMode m) {
        if (m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !caps.sampler_mirror_clamp_to_edge)
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        return m;
    };
    ci.addressModeU = address(k.address_u);
    ci.addressModeV = address(k.address_v);
    ci.addressModeW = address(k.address_w);
    ci.mipLodBias = std::clamp(k.lod_bias, -caps.max_sampler_lod_bias, caps.max_sampler_lod_bias);
    if (k.max_anisotropy > 1 && caps.sampler_anisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(static_cast<float>(k.max_anisotropy), caps.max_anisotropy);
    }
    ci.compareEnable = k.compare_enable ? VK_TRUE : VK_FALSE;
    ci.compareOp = k.compare_op;
    ci.minLod = k.min_lod;
    ci.maxLod = k.max_lod;
    ci.borderColor = k.border;
    VkSampler s = VK_NULL_HANDLE;
    if (!vk_check(ctx_->vkd().vkCreateSampler(ctx_->device(), &ci, nullptr, &s), "vkCreateSampler")) return VK_NULL_HANDLE;
    map_.emplace(packed, s);
    return s;
}

}  // namespace alamo::rhi

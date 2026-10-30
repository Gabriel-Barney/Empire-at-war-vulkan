// Sampler cache keyed by state (PORTING.md 3.3: filters, addressing, anisotropy, LOD bias). The key
// packs into 64 bits, so equality and hashing are exact and padding-free.
#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Context;

struct SamplerKey {
    VkFilter min_filter = VK_FILTER_LINEAR;
    VkFilter mag_filter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mip_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode address_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    uint8_t max_anisotropy = 0;  // 0/1 = off, else 2..16 (clamped to the device limit)
    bool compare_enable = false;
    VkCompareOp compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    float lod_bias = 0.0f;    // quantized to 1/256
    float min_lod = 0.0f;     // quantized to 1/16, 0..15.9
    float max_lod = VK_LOD_CLAMP_NONE;  // quantized to 1/16; >= 16 means no clamp

    // 64-bit canonical form (quantized). Two keys with equal pack() create identical samplers.
    uint64_t pack() const;
    static SamplerKey unpack(uint64_t packed);
    uint64_t hash() const;  // well-mixed pack()
    bool operator==(const SamplerKey& o) const { return pack() == o.pack(); }

    static SamplerKey linear_wrap() { return {}; }
    static SamplerKey linear_clamp();
    static SamplerKey point_wrap();
    static SamplerKey point_clamp();
};

class SamplerCache {
public:
    SamplerCache() = default;
    ~SamplerCache();
    SamplerCache(const SamplerCache&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;

    void init(Context& ctx) { ctx_ = &ctx; }
    void shutdown();
    VkSampler get(const SamplerKey& key);  // thread-safe; created on first use
    size_t size() const;

private:
    struct Hash {
        size_t operator()(uint64_t v) const { return static_cast<size_t>(v ^ (v >> 29)); }
    };
    Context* ctx_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, VkSampler, Hash> map_;
};

}  // namespace alamo::rhi

// GPU memory: a small allocate/free/map interface plus buffer/image helpers. Backed by VMA
// (toolchain/vma, approved 2026-10-26); VMA types stay out of this header so the backend can be
// swapped without touching callers. Thread-safe (VMA's internal locking).
#pragma once
#include <cstdint>

#include "alamo/rhi/format.hpp"
#include "alamo/rhi/vk.hpp"

struct VmaAllocator_T;   // == VmaAllocator
struct VmaAllocation_T;  // == VmaAllocation

namespace alamo::rhi {

class Context;

enum class MemoryUsage : uint8_t {
    GpuOnly,   // device local, no host access
    Upload,    // host-visible, sequential write, persistently mapped (staging)
    Dynamic,   // host-visible + HOST_COHERENT (guaranteed, no flush needed), device local when available (ReBAR)
    Readback,  // host-visible, cached when available, persistently mapped
};

struct Allocation {
    VmaAllocation_T* handle = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // persistent mapping for host-visible usages
    uint32_t memory_type = 0;
    explicit operator bool() const { return handle != nullptr; }
};

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    Allocation alloc;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    void* mapped() const { return alloc.mapped; }
    explicit operator bool() const { return handle != VK_NULL_HANDLE; }
};

struct ImageDesc {
    Format format = Format::RGBA8;
    uint32_t width = 1, height = 1;
    uint32_t depth = 1;    // > 1: 3D (volume) image with a 3D view; layers must be 1
    uint32_t mips = 1;     // 0 = full chain
    uint32_t layers = 1;   // 6 with cube = true for cube maps
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool cube = false;
    Swizzle swizzle = Swizzle::Identity;  // applied to the default view (L8 -> RRR1 etc.)
    const char* name = nullptr;
};

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // sampling view: whole resource (2D, 2D array, cube or 3D);
                                        // depth-only for depth+stencil formats
    VkImageView attachment_view = VK_NULL_HANDLE;  // attachment images: mip 0 / layer 0, all aspects
                                                   // (== view when that is already the case)
    Allocation alloc;
    Format format = Format::Undefined;
    uint32_t width = 0, height = 0, depth = 1, mips = 0, layers = 0;
    VkImageAspectFlags aspect = 0;
    VkFormat vk_format() const { return to_vk(format); }
    VkImageSubresourceRange range() const { return {aspect, 0, mips, 0, layers}; }
    explicit operator bool() const { return handle != VK_NULL_HANDLE; }
};

struct MemoryStats {
    uint64_t allocation_count = 0;
    uint64_t allocation_bytes = 0;
    uint64_t block_count = 0;
    uint64_t block_bytes = 0;
};

// First of D24S8 / D32FS8 usable as a depth-stencil attachment (and sampled, if asked) on this device.
// AMD RDNA has no D24S8, so expect D32FS8 there. Undefined if neither works.
Format pick_depth_stencil_format(const Context& ctx, bool sampled = false);

class Allocator {
public:
    Allocator() = default;
    ~Allocator();
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    bool init(Context& ctx);
    void shutdown();
    bool valid() const { return vma_ != nullptr; }
    Context& context() const { return *ctx_; }

    // Raw memory for a resource the caller binds itself.
    bool allocate(const VkMemoryRequirements& reqs, MemoryUsage usage, Allocation& out);
    void free(Allocation& a);
    // Persistent mapping (host-visible usages are mapped at allocation already); map() returns it.
    void* map(Allocation& a);
    void unmap(Allocation& a);
    // No-ops on HOST_COHERENT memory.
    bool flush(const Allocation& a, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
    bool invalidate(const Allocation& a, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage mem, Buffer& out,
                       const char* name = nullptr);
    void destroy(Buffer& b);
    bool create_image(const ImageDesc& desc, Image& out);
    void destroy(Image& img);

    MemoryStats stats() const;

private:
    Context* ctx_ = nullptr;
    VmaAllocator_T* vma_ = nullptr;
};

}  // namespace alamo::rhi

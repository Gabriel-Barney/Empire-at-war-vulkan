#include "alamo/rhi/memory.hpp"

#include <vk_mem_alloc.h>

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

namespace {

VmaAllocationCreateInfo auto_info(MemoryUsage usage) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    switch (usage) {
        case MemoryUsage::GpuOnly: ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
        case MemoryUsage::Upload:
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUsage::Dynamic:
            // Written every frame without explicit flushes (frame uniform ring): must be coherent.
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case MemoryUsage::Readback:
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }
    return ai;
}

// vmaAllocateMemory takes no buffer/image info, so AUTO usages are not allowed: use flags.
VmaAllocationCreateInfo raw_info(MemoryUsage usage) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_UNKNOWN;
    switch (usage) {
        case MemoryUsage::GpuOnly: ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; break;
        case MemoryUsage::Upload:
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            ai.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUsage::Dynamic:
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUsage::Readback:
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            ai.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }
    return ai;
}

void fill(Allocation& out, VmaAllocation a, const VmaAllocationInfo& info) {
    out.handle = a;
    out.memory = info.deviceMemory;
    out.offset = info.offset;
    out.size = info.size;
    out.mapped = info.pMappedData;
    out.memory_type = info.memoryType;
}

}  // namespace

Allocator::~Allocator() { shutdown(); }

bool Allocator::init(Context& ctx) {
    shutdown();
    ctx_ = &ctx;
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = ctx.loader().global().vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = ctx.vki().vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci{};
    ci.physicalDevice = ctx.physical_device();
    ci.device = ctx.device();
    ci.instance = ctx.instance();
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &fns;
    VmaAllocator vma = nullptr;
    if (!vk_check(vmaCreateAllocator(&ci, &vma), "vmaCreateAllocator")) return false;
    vma_ = vma;
    return true;
}

void Allocator::shutdown() {
    if (vma_) {
        const MemoryStats s = stats();
        if (s.allocation_count)
            log::warnf("rhi", "Allocator shutdown with %llu live allocations (%llu bytes)",
                       static_cast<unsigned long long>(s.allocation_count),
                       static_cast<unsigned long long>(s.allocation_bytes));
        vmaDestroyAllocator(vma_);
    }
    vma_ = nullptr;
}

bool Allocator::allocate(const VkMemoryRequirements& reqs, MemoryUsage usage, Allocation& out) {
    out = Allocation{};
    const VmaAllocationCreateInfo ai = raw_info(usage);
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    if (!vk_check(vmaAllocateMemory(vma_, &reqs, &ai, &a, &info), "vmaAllocateMemory")) return false;
    fill(out, a, info);
    return true;
}

void Allocator::free(Allocation& a) {
    if (a.handle) vmaFreeMemory(vma_, a.handle);
    a = Allocation{};
}

void* Allocator::map(Allocation& a) {
    void* p = nullptr;
    if (!a.handle || !vk_check(vmaMapMemory(vma_, a.handle, &p), "vmaMapMemory")) return nullptr;
    return p;
}

void Allocator::unmap(Allocation& a) {
    if (a.handle) vmaUnmapMemory(vma_, a.handle);
}

bool Allocator::flush(const Allocation& a, VkDeviceSize offset, VkDeviceSize size) {
    return a.handle && vk_check(vmaFlushAllocation(vma_, a.handle, offset, size), "vmaFlushAllocation");
}

bool Allocator::invalidate(const Allocation& a, VkDeviceSize offset, VkDeviceSize size) {
    return a.handle && vk_check(vmaInvalidateAllocation(vma_, a.handle, offset, size), "vmaInvalidateAllocation");
}

bool Allocator::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage mem, Buffer& out,
                              const char* name) {
    out = Buffer{};
    auto bi = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const VmaAllocationCreateInfo ai = auto_info(mem);
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    if (!vk_check(vmaCreateBuffer(vma_, &bi, &ai, &out.handle, &a, &info), "vmaCreateBuffer")) {
        out.handle = VK_NULL_HANDLE;
        return false;
    }
    fill(out.alloc, a, info);
    out.size = size;
    out.usage = usage;
    if (name) {
        vmaSetAllocationName(vma_, a, name);
        ctx_->set_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(out.handle), name);
    }
    return true;
}

void Allocator::destroy(Buffer& b) {
    if (b.handle) vmaDestroyBuffer(vma_, b.handle, b.alloc.handle);
    b = Buffer{};
}

bool Allocator::create_image(const ImageDesc& d, Image& out) {
    out = Image{};
    const FormatInfo& fi = format_info(d.format);
    if (fi.vk == VK_FORMAT_UNDEFINED || !d.width || !d.height) {
        log::errorf("rhi", "create_image: bad format/extent");
        return false;
    }
    const uint32_t depth = d.depth ? d.depth : 1;
    const bool volume = depth > 1;
    const uint32_t mips = d.mips ? d.mips : max_mip_count_3d(d.width, d.height, depth);
    const uint32_t layers = d.layers ? d.layers : 1;
    if (volume && (layers != 1 || d.cube || fi.depth || fi.stencil)) {
        log::errorf("rhi", "create_image: 3D images have one layer, no cube flag and a colour format");
        return false;
    }
    if (d.cube && (layers % 6 != 0 || d.width != d.height)) {
        log::errorf("rhi", "create_image: cube images need square faces and a multiple of 6 layers");
        return false;
    }
    if (d.cube && layers > 6 && !ctx_->caps().image_cube_array) {
        log::errorf("rhi", "create_image: cube arrays (%u layers) need the imageCubeArray feature", layers);
        return false;
    }
    auto ii = vk_struct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ii.flags = d.cube ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) : 0u;
    ii.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = fi.vk;
    ii.extent = {d.width, d.height, depth};
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    ii.samples = d.samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = d.usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VmaAllocationCreateInfo ai = auto_info(MemoryUsage::GpuOnly);
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    if (!vk_check(vmaCreateImage(vma_, &ii, &ai, &out.handle, &a, &info), "vmaCreateImage")) {
        out.handle = VK_NULL_HANDLE;
        return false;
    }
    fill(out.alloc, a, info);
    out.format = d.format;
    out.width = d.width;
    out.height = d.height;
    out.depth = depth;
    out.mips = mips;
    out.layers = layers;
    out.aspect = aspect_mask(d.format);

    auto vi = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vi.image = out.handle;
    vi.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : d.cube ? (layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
                         : (layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    vi.format = fi.vk;
    vi.components = component_mapping(d.swizzle);
    // Depth+stencil images: the default view is depth-only (sampling); attachments use the image aspect.
    VkImageAspectFlags view_aspect = out.aspect;
    if (fi.depth && fi.stencil) view_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange = {view_aspect, 0, mips, 0, layers};
    const DeviceFns& fn = ctx_->vkd();
    if (!vk_check(fn.vkCreateImageView(ctx_->device(), &vi, nullptr, &out.view), "vkCreateImageView")) {
        destroy(out);
        return false;
    }
    // Attachment view: mip 0 / layer 0, every aspect, identity swizzle (required for attachments).
    if (d.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        const bool reuse = mips == 1 && layers == 1 && !volume && !(fi.depth && fi.stencil) &&
                           d.swizzle == Swizzle::Identity;
        if (reuse) {
            out.attachment_view = out.view;
        } else {
            auto av = vi;
            av.viewType = VK_IMAGE_VIEW_TYPE_2D;
            av.components = {};
            av.subresourceRange = {out.aspect, 0, 1, 0, 1};
            if (!vk_check(fn.vkCreateImageView(ctx_->device(), &av, nullptr, &out.attachment_view), "attachment view")) {
                destroy(out);
                return false;
            }
        }
    }
    if (d.name) {
        vmaSetAllocationName(vma_, a, d.name);
        ctx_->set_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(out.handle), d.name);
    }
    return true;
}

void Allocator::destroy(Image& img) {
    if (img.attachment_view && img.attachment_view != img.view)
        ctx_->vkd().vkDestroyImageView(ctx_->device(), img.attachment_view, nullptr);
    if (img.view) ctx_->vkd().vkDestroyImageView(ctx_->device(), img.view, nullptr);
    if (img.handle) vmaDestroyImage(vma_, img.handle, img.alloc.handle);
    img = Image{};
}

Format pick_depth_stencil_format(const Context& ctx, bool sampled) {
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (sampled) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (Format f : {Format::D24S8, Format::D32FS8})
        if (ctx.format_supported(to_vk(f), need)) return f;
    return Format::Undefined;
}

MemoryStats Allocator::stats() const {
    MemoryStats s;
    if (!vma_) return s;
    VmaTotalStatistics t{};
    vmaCalculateStatistics(vma_, &t);
    s.allocation_count = t.total.statistics.allocationCount;
    s.allocation_bytes = t.total.statistics.allocationBytes;
    s.block_count = t.total.statistics.blockCount;
    s.block_bytes = t.total.statistics.blockBytes;
    return s;
}

}  // namespace alamo::rhi

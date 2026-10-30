#include "alamo/rhi/swapchain.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

Swapchain::~Swapchain() { shutdown(); }

bool Swapchain::init(Context& ctx, const SwapchainDesc& desc) {
    shutdown();
    ctx_ = &ctx;
    desc_ = desc;
    if (!ctx.presentation_enabled() || !ctx.vki().vkGetPhysicalDeviceSurfaceSupportKHR) {
        log::errorf("rhi", "Swapchain: context was created without presentation");
        return false;
    }
    if (!create_surface(ctx, desc.hinstance, desc.hwnd, surface_)) return false;
    VkBool32 supported = VK_FALSE;
    ctx.vki().vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physical_device(), ctx.graphics().family, surface_, &supported);
    if (!supported) {
        log::errorf("rhi", "graphics queue family cannot present to this surface");
        shutdown();
        return false;
    }
    if (!build(desc.width, desc.height)) {
        shutdown();
        return false;
    }
    return true;
}

// Views can go once the device is idle (only GPU work reads them). The render-finished semaphores
// cannot: vkDeviceWaitIdle does not cover the presentation engine's wait on them, and without
// VK_EXT_swapchain_maintenance1 (present fences; not used here) there is no direct signal for it.
// They are retired instead and destroyed once every image of the next swapchain has been acquired
// (image_count() successful acquires), by which point the presents of the old swapchain have been
// consumed. Swapchain::shutdown destroys whatever is still retired after a device wait.
void Swapchain::destroy_images() {
    const DeviceFns& fn = ctx_->vkd();
    for (VkImageView v : views_) fn.vkDestroyImageView(ctx_->device(), v, nullptr);
    // Bound the backlog when recreates repeat without acquires (e.g. a drag-resize storm): the
    // oldest retirees are several swapchains old by then.
    if (retired_semaphores_.size() > 32) destroy_retired();
    retired_semaphores_.insert(retired_semaphores_.end(), render_finished_.begin(), render_finished_.end());
    views_.clear();
    render_finished_.clear();
    images_.clear();
}

void Swapchain::destroy_retired() {
    for (VkSemaphore s : retired_semaphores_) ctx_->vkd().vkDestroySemaphore(ctx_->device(), s, nullptr);
    retired_semaphores_.clear();
    retire_acquires_left_ = 0;
}

void Swapchain::shutdown() {
    if (!ctx_) return;
    ctx_->wait_idle();
    destroy_images();
    destroy_retired();
    if (swapchain_) ctx_->vkd().vkDestroySwapchainKHR(ctx_->device(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (surface_) ctx_->vki().vkDestroySurfaceKHR(ctx_->instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

void Swapchain::set_present_mode(PresentMode mode) { desc_.mode = mode; }

bool Swapchain::supports(PresentMode mode) const {
    const VkPresentModeKHR m = mode == PresentMode::Mailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                             : mode == PresentMode::Immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    return m == VK_PRESENT_MODE_FIFO_KHR || std::find(surface_modes_.begin(), surface_modes_.end(), m) != surface_modes_.end();
}

bool Swapchain::recreate(uint32_t width, uint32_t height) {
    if (!ctx_ || !surface_) return false;
    ctx_->wait_idle();
    return build(width, height);
}

bool Swapchain::build(uint32_t width, uint32_t height) {
    const InstanceFns& vi = ctx_->vki();
    const DeviceFns& fn = ctx_->vkd();
    VkPhysicalDevice pd = ctx_->physical_device();
    VkDevice dev = ctx_->device();

    VkSurfaceCapabilitiesKHR caps{};
    if (!vk_check(vi.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface_, &caps), "surface capabilities"))
        return false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (ext.width == 0 || ext.height == 0) {  // minimized: drop the swapchain until restored
        destroy_images();
        if (swapchain_) fn.vkDestroySwapchainKHR(dev, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        extent_ = {0, 0};
        return true;
    }

    uint32_t n = 0;
    vi.vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vi.vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &n, formats.data());
    formats.resize(n);
    if (formats.empty()) return false;
    // UNORM back buffer: the original renders gamma-space with no sRGB write (PORTING 3.2).
    VkSurfaceFormatKHR chosen = formats[0];
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
            return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end()) {
            chosen = *it;
            break;
        }
    }

    n = 0;
    vi.vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vi.vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &n, modes.data());
    modes.resize(n);
    surface_modes_ = modes;
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (desc_.mode == PresentMode::Immediate && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (desc_.mode != PresentMode::Fifo && has(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;

    uint32_t count = std::max(desc_.min_images, caps.minImageCount);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // TRANSFER_SRC: StretchRect-style copies of the back buffer into SceneTexture etc.
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    else log::warnf("rhi", "swapchain images cannot be a transfer source; back-buffer copies unavailable");
    usage_ = usage;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
            if (caps.supportedCompositeAlpha & b) {
                alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(b);
                break;
            }
    }

    auto ci = vk_struct<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    const VkResult r = fn.vkCreateSwapchainKHR(dev, &ci, nullptr, &fresh);
    destroy_images();
    if (swapchain_) fn.vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (!vk_check(r, "vkCreateSwapchainKHR")) {
        extent_ = {0, 0};
        return false;
    }

    // Build the per-image objects aside and commit only when all exist, so a failure never leaves a
    // valid swapchain with short views_/render_finished_ (image(i)/view(i)/present index them).
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> sems;
    bool ok = vk_check(fn.vkGetSwapchainImagesKHR(dev, fresh, &n, nullptr), "vkGetSwapchainImagesKHR") && n > 0;
    if (ok) {
        images.resize(n);
        ok = vk_check(fn.vkGetSwapchainImagesKHR(dev, fresh, &n, images.data()), "vkGetSwapchainImagesKHR");
        images.resize(n);
    }
    for (uint32_t i = 0; ok && i < n; ++i) {
        auto vci = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = chosen.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        ok = vk_check(fn.vkCreateImageView(dev, &vci, nullptr, &v), "swapchain view");
        if (!ok) break;
        views.push_back(v);
        auto sci = vk_struct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        VkSemaphore s = VK_NULL_HANDLE;
        ok = vk_check(fn.vkCreateSemaphore(dev, &sci, nullptr, &s), "render-finished semaphore");
        if (ok) sems.push_back(s);
    }
    if (!ok) {  // roll back: nothing of the new swapchain was presented, so all of it can go now
        for (VkImageView v : views) fn.vkDestroyImageView(dev, v, nullptr);
        for (VkSemaphore s : sems) fn.vkDestroySemaphore(dev, s, nullptr);
        fn.vkDestroySwapchainKHR(dev, fresh, nullptr);
        extent_ = {0, 0};
        return false;
    }
    swapchain_ = fresh;
    format_ = chosen.format;
    extent_ = ext;
    present_mode_ = mode;
    images_ = std::move(images);
    views_ = std::move(views);
    render_finished_ = std::move(sems);
    if (!retired_semaphores_.empty()) retire_acquires_left_ = n;
    log::debugf("rhi", "swapchain %ux%u, %u images, present mode %d", ext.width, ext.height, n, static_cast<int>(mode));
    return true;
}

SwapchainStatus Swapchain::acquire(VkSemaphore signal, uint32_t& image_index, uint64_t timeout_ns) {
    if (!swapchain_) return SwapchainStatus::Minimized;
    const VkResult r =
        ctx_->vkd().vkAcquireNextImageKHR(ctx_->device(), swapchain_, timeout_ns, signal, VK_NULL_HANDLE, &image_index);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && retire_acquires_left_ && --retire_acquires_left_ == 0)
        destroy_retired();  // every image of this swapchain was acquired since the old one retired
    switch (r) {
        case VK_SUCCESS: return SwapchainStatus::Ok;
        case VK_SUBOPTIMAL_KHR: return SwapchainStatus::Suboptimal;
        case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainStatus::OutOfDate;
        default: vk_check(r, "vkAcquireNextImageKHR"); return SwapchainStatus::Error;
    }
}

SwapchainStatus Swapchain::present(uint32_t image_index) {
    if (!swapchain_) return SwapchainStatus::Minimized;
    auto pi = vk_struct<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;
    VkResult r;
    {
        const Queue& q = ctx_->graphics();
        std::lock_guard<std::mutex> lock(*q.mutex);
        r = ctx_->vkd().vkQueuePresentKHR(q.handle, &pi);
    }
    switch (r) {
        case VK_SUCCESS: return SwapchainStatus::Ok;
        case VK_SUBOPTIMAL_KHR: return SwapchainStatus::Suboptimal;
        case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainStatus::OutOfDate;
        default: vk_check(r, "vkQueuePresentKHR"); return SwapchainStatus::Error;
    }
}

}  // namespace alamo::rhi

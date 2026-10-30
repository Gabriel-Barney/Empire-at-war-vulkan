// Win32 surface + swapchain with resize / out-of-date handling. Requires a Context created with
// ContextDesc::presentation = true. One render-finished semaphore per swapchain image (indexed by
// the acquired image) so a semaphore is never re-signalled while a present may still wait on it.
// On recreate the old semaphores are not destroyed right away (the presentation engine may still
// wait on them after vkDeviceWaitIdle): they are retired until every new image has been acquired
// once. A failed build leaves no swapchain (valid() false), never a partial one.
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/rhi/surface.hpp"
#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Context;

enum class PresentMode : uint8_t {
    Fifo,       // vsync (always available)
    Mailbox,    // low-latency vsync; falls back to Fifo
    Immediate,  // tearing; falls back to Mailbox then Fifo
};

struct SwapchainDesc {
    void* hinstance = nullptr;  // HINSTANCE
    void* hwnd = nullptr;       // HWND
    uint32_t width = 0, height = 0;  // used only when the surface does not dictate the extent
    PresentMode mode = PresentMode::Fifo;
    uint32_t min_images = 3;
};

enum class SwapchainStatus : uint8_t { Ok, Suboptimal, OutOfDate, Minimized, Error };

class Swapchain {
public:
    Swapchain() = default;
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    bool init(Context& ctx, const SwapchainDesc& desc);
    void shutdown();
    // Rebuilds for the new size (waits for the device to idle). A 0x0 size (minimized) leaves the
    // swapchain empty; acquire() then reports Minimized until a later recreate succeeds.
    bool recreate(uint32_t width, uint32_t height);
    void set_present_mode(PresentMode mode);  // applied on the next recreate

    // signal: binary semaphore signalled when the image is ready to be rendered. Suboptimal still
    // acquired the image and signals signal: render and present it (the submit waits on the
    // semaphore), then recreate. Never recreate between a Suboptimal acquire and that submit, or the
    // semaphore stays signalled with no waiter. OutOfDate/Minimized/Error signal nothing.
    SwapchainStatus acquire(VkSemaphore signal, uint32_t& image_index, uint64_t timeout_ns = UINT64_MAX);
    // Presents image_index waiting on render_finished(image_index).
    SwapchainStatus present(uint32_t image_index);

    bool valid() const { return swapchain_ != VK_NULL_HANDLE; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    uint32_t image_count() const { return static_cast<uint32_t>(images_.size()); }
    VkImage image(uint32_t i) const { return images_[i]; }
    VkImageView view(uint32_t i) const { return views_[i]; }
    VkSemaphore render_finished(uint32_t i) const { return render_finished_[i]; }
    VkPresentModeKHR present_mode() const { return present_mode_; }
    // True when the surface offers `mode` (as of the last build; Fifo always). Mailbox / Immediate fall back otherwise.
    bool supports(PresentMode mode) const;
    VkImageUsageFlags usage() const { return usage_; }
    bool supports_transfer_src() const { return (usage_ & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0; }

private:
    bool build(uint32_t width, uint32_t height);
    void destroy_images();   // views destroyed, render-finished semaphores retired
    void destroy_retired();

    Context* ctx_ = nullptr;
    SwapchainDesc desc_{};
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> surface_modes_;  // the surface's present modes (last build)
    VkImageUsageFlags usage_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> render_finished_;
    // Semaphores of replaced swapchains, destroyed after retire_acquires_left_ more acquires
    // (one per image of the current swapchain) or at shutdown (see swapchain.cpp).
    std::vector<VkSemaphore> retired_semaphores_;
    uint32_t retire_acquires_left_ = 0;
};

}  // namespace alamo::rhi

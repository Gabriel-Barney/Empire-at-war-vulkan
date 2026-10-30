#include "alamo/rhi/surface.hpp"

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

#include <windows.h>
#include <vulkan/vulkan_win32.h>

namespace alamo::rhi {

namespace {
constexpr const char* kWin32Ext = "VK_KHR_win32_surface";
}  // namespace

const char* platform_surface_extension() { return kWin32Ext; }

bool create_surface(const Context& ctx, void* hinstance, void* hwnd, VkSurfaceKHR& out) {
    out = VK_NULL_HANDLE;
    if (!ctx.instance_extension_enabled(kWin32Ext)) {
        log::errorf("rhi", "create_surface: %s is not enabled", kWin32Ext);
        return false;
    }
    auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(ctx.vki().vkCreateWin32SurfaceKHR);
    if (!create) {
        log::errorf("rhi", "vkCreateWin32SurfaceKHR unavailable");
        return false;
    }
    auto si = vk_struct<VkWin32SurfaceCreateInfoKHR>(VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR);
    si.hinstance = static_cast<HINSTANCE>(hinstance);
    si.hwnd = static_cast<HWND>(hwnd);
    if (!vk_check(create(ctx.instance(), &si, nullptr, &out), "vkCreateWin32SurfaceKHR")) {
        out = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

}  // namespace alamo::rhi

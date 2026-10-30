// Win32 window surface (VkSurfaceKHR) behind one small interface, so Swapchain and Context stay
// free of window-system headers. Win32 is the only supported platform.
#pragma once

#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Context;

// Instance extension that carries the Win32 surface (VK_KHR_surface is not included).
// Context enables it when ContextDesc::presentation is set.
const char* platform_surface_extension();

// Creates the surface for a window (logged on failure). hinstance = HINSTANCE, hwnd = HWND.
// The caller destroys it with vkDestroySurfaceKHR.
bool create_surface(const Context& ctx, void* hinstance, void* hwnd, VkSurfaceKHR& out);

}  // namespace alamo::rhi

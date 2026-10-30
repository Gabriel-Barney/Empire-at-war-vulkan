// Vulkan include + hand-written function-pointer tables (no volk). The engine is built with
// VK_NO_PROTOTYPES (target alamo_vulkan_headers), so every call goes through one of these tables:
//   GlobalFns   - loaded with vkGetInstanceProcAddr(nullptr, ...)
//   InstanceFns - loaded with vkGetInstanceProcAddr(instance, ...)
//   DeviceFns   - loaded with vkGetDeviceProcAddr(device, ...)
// Members keep the Vulkan names (fn.vkCmdDraw(...)) so code reads like plain Vulkan.
// Add an entry to the X-list when a new function is needed. Platform (Win32) entry points are
// stored as PFN_vkVoidFunction and cast in the platform .cpp so this header stays platform-free.
#pragma once
#include <vulkan/vulkan_core.h>

#include <cstdint>

// Always present in a 1.3 loader.
#define ALAMO_VK_GLOBAL_FUNCS(X)              \
    X(vkCreateInstance)                       \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)     \
    X(vkEnumerateInstanceVersion)

#define ALAMO_VK_INSTANCE_FUNCS(X)                  \
    X(vkDestroyInstance)                            \
    X(vkEnumeratePhysicalDevices)                   \
    X(vkGetPhysicalDeviceProperties)                \
    X(vkGetPhysicalDeviceProperties2)               \
    X(vkGetPhysicalDeviceFeatures2)                 \
    X(vkGetPhysicalDeviceQueueFamilyProperties)     \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceFormatProperties)          \
    X(vkEnumerateDeviceExtensionProperties)         \
    X(vkCreateDevice)                               \
    X(vkGetDeviceProcAddr)

// Extension entry points: may be null (surface/debug-utils not enabled).
#define ALAMO_VK_INSTANCE_OPT_FUNCS(X)              \
    X(vkDestroySurfaceKHR)                          \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)         \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)    \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)         \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)    \
    X(vkCreateDebugUtilsMessengerEXT)               \
    X(vkDestroyDebugUtilsMessengerEXT)              \
    X(vkSetDebugUtilsObjectNameEXT)                 \
    X(vkCmdBeginDebugUtilsLabelEXT)                 \
    X(vkCmdEndDebugUtilsLabelEXT)

#define ALAMO_VK_DEVICE_FUNCS(X)          \
    X(vkDestroyDevice)                    \
    X(vkGetDeviceQueue)                   \
    X(vkDeviceWaitIdle)                   \
    X(vkQueueSubmit2)                     \
    X(vkQueueWaitIdle)                    \
    X(vkCreateBuffer)                     \
    X(vkGetBufferMemoryRequirements)      \
    X(vkBindBufferMemory)                 \
    X(vkGetImageMemoryRequirements)       \
    X(vkBindImageMemory)                  \
    X(vkDestroyBuffer)                    \
    X(vkCreateImage)                      \
    X(vkDestroyImage)                     \
    X(vkCreateImageView)                  \
    X(vkDestroyImageView)                 \
    X(vkCreateSampler)                    \
    X(vkDestroySampler)                   \
    X(vkCreateShaderModule)               \
    X(vkDestroyShaderModule)              \
    X(vkCreatePipelineCache)              \
    X(vkDestroyPipelineCache)             \
    X(vkGetPipelineCacheData)             \
    X(vkCreateGraphicsPipelines)          \
    X(vkCreateComputePipelines)           \
    X(vkDestroyPipeline)                  \
    X(vkCreatePipelineLayout)             \
    X(vkDestroyPipelineLayout)            \
    X(vkCreateDescriptorSetLayout)        \
    X(vkDestroyDescriptorSetLayout)       \
    X(vkCreateDescriptorPool)             \
    X(vkDestroyDescriptorPool)            \
    X(vkResetDescriptorPool)              \
    X(vkAllocateDescriptorSets)           \
    X(vkUpdateDescriptorSets)             \
    X(vkCreateCommandPool)                \
    X(vkDestroyCommandPool)               \
    X(vkResetCommandPool)                 \
    X(vkAllocateCommandBuffers)           \
    X(vkFreeCommandBuffers)               \
    X(vkBeginCommandBuffer)               \
    X(vkEndCommandBuffer)                 \
    X(vkCreateSemaphore)                  \
    X(vkDestroySemaphore)                 \
    X(vkWaitSemaphores)                   \
    X(vkGetSemaphoreCounterValue)         \
    X(vkSignalSemaphore)                  \
    X(vkCreateFence)                      \
    X(vkDestroyFence)                     \
    X(vkWaitForFences)                    \
    X(vkResetFences)                      \
    X(vkCmdPipelineBarrier2)              \
    X(vkCmdBeginRendering)                \
    X(vkCmdEndRendering)                  \
    X(vkCmdBindPipeline)                  \
    X(vkCmdBindDescriptorSets)            \
    X(vkCmdBindVertexBuffers)             \
    X(vkCmdBindIndexBuffer)               \
    X(vkCmdPushConstants)                 \
    X(vkCmdSetViewport)                   \
    X(vkCmdSetScissor)                    \
    X(vkCmdSetStencilReference)           \
    X(vkCmdSetStencilCompareMask)         \
    X(vkCmdSetStencilWriteMask)           \
    X(vkCmdSetBlendConstants)             \
    X(vkCmdSetDepthBias)                  \
    X(vkCmdDraw)                          \
    X(vkCmdDrawIndexed)                   \
    X(vkCmdDispatch)                      \
    X(vkCmdExecuteCommands)               \
    X(vkCmdCopyBuffer)                    \
    X(vkCmdCopyBufferToImage)             \
    X(vkCmdCopyImageToBuffer)             \
    X(vkCmdClearColorImage)               \
    X(vkCmdBlitImage)                     \
    X(vkCmdBlitImage2)                    \
    X(vkCmdCopyImage2)                    \
    X(vkCmdSetStencilOp)                  \
    X(vkCreateQueryPool)                  \
    X(vkDestroyQueryPool)                 \
    X(vkCmdResetQueryPool)                \
    X(vkResetQueryPool)                   \
    X(vkCmdWriteTimestamp2)               \
    X(vkGetQueryPoolResults)

// VK_KHR_swapchain: null when the device was created without presentation.
#define ALAMO_VK_DEVICE_OPT_FUNCS(X) \
    X(vkCreateSwapchainKHR)          \
    X(vkDestroySwapchainKHR)         \
    X(vkGetSwapchainImagesKHR)       \
    X(vkAcquireNextImageKHR)         \
    X(vkQueuePresentKHR)

namespace alamo::rhi {

#define ALAMO_VK_DECLARE_MEMBER(name) PFN_##name name = nullptr;

struct GlobalFns {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    ALAMO_VK_GLOBAL_FUNCS(ALAMO_VK_DECLARE_MEMBER)
};

struct InstanceFns {
    ALAMO_VK_INSTANCE_FUNCS(ALAMO_VK_DECLARE_MEMBER)
    ALAMO_VK_INSTANCE_OPT_FUNCS(ALAMO_VK_DECLARE_MEMBER)
    // Win32 (cast to the real PFN in the platform code). Null elsewhere / when not enabled.
    PFN_vkVoidFunction vkCreateWin32SurfaceKHR = nullptr;
    PFN_vkVoidFunction vkGetPhysicalDeviceWin32PresentationSupportKHR = nullptr;
};

struct DeviceFns {
    ALAMO_VK_DEVICE_FUNCS(ALAMO_VK_DECLARE_MEMBER)
    ALAMO_VK_DEVICE_OPT_FUNCS(ALAMO_VK_DECLARE_MEMBER)
};

#undef ALAMO_VK_DECLARE_MEMBER

// "VK_ERROR_OUT_OF_DATE_KHR" etc. Unknown codes return "VkResult(<n>)" from a static buffer.
const char* vk_result_name(VkResult r);

// Logs "<what> failed: <result>" on channel "rhi" when r < 0. Returns r >= 0.
bool vk_check(VkResult r, const char* what);

// Zero-initialised Vulkan struct with sType set (avoids partial aggregate init, which trips
// -Wmissing-field-initializers on GCC/Clang).
template <class T>
inline T vk_struct(VkStructureType type) {
    T v{};
    v.sType = type;
    return v;
}

inline constexpr uint64_t align_up(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

}  // namespace alamo::rhi

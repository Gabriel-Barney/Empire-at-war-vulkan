#include "alamo/rhi/loader.hpp"

#include <cstdio>

#include "alamo/core/log.hpp"

#include <windows.h>

namespace alamo::rhi {

namespace {

void* open_library() {
    // LOAD_LIBRARY_SEARCH_SYSTEM32: only the system loader, never a vulkan-1.dll next to the exe/cwd.
    HMODULE h = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) h = LoadLibraryW(L"vulkan-1.dll");
    return reinterpret_cast<void*>(h);
}

void close_library(void* lib) {
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
}

PFN_vkGetInstanceProcAddr get_gipa(void* lib) {
    FARPROC p = GetProcAddress(reinterpret_cast<HMODULE>(lib), "vkGetInstanceProcAddr");
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void (*)()>(p));
}

}  // namespace

const char* vk_result_name(VkResult r) {
    switch (r) {
#define R(x) \
    case x: return #x;
        R(VK_SUCCESS) R(VK_NOT_READY) R(VK_TIMEOUT) R(VK_EVENT_SET) R(VK_EVENT_RESET) R(VK_INCOMPLETE)
        R(VK_ERROR_OUT_OF_HOST_MEMORY) R(VK_ERROR_OUT_OF_DEVICE_MEMORY) R(VK_ERROR_INITIALIZATION_FAILED)
        R(VK_ERROR_DEVICE_LOST) R(VK_ERROR_MEMORY_MAP_FAILED) R(VK_ERROR_LAYER_NOT_PRESENT)
        R(VK_ERROR_EXTENSION_NOT_PRESENT) R(VK_ERROR_FEATURE_NOT_PRESENT) R(VK_ERROR_INCOMPATIBLE_DRIVER)
        R(VK_ERROR_TOO_MANY_OBJECTS) R(VK_ERROR_FORMAT_NOT_SUPPORTED) R(VK_ERROR_FRAGMENTED_POOL)
        R(VK_ERROR_UNKNOWN) R(VK_ERROR_OUT_OF_POOL_MEMORY) R(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        R(VK_ERROR_FRAGMENTATION) R(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS) R(VK_PIPELINE_COMPILE_REQUIRED)
        R(VK_ERROR_SURFACE_LOST_KHR) R(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) R(VK_SUBOPTIMAL_KHR)
        R(VK_ERROR_OUT_OF_DATE_KHR) R(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR) R(VK_ERROR_VALIDATION_FAILED_EXT)
#undef R
        default: break;
    }
    static thread_local char buf[32];
    std::snprintf(buf, sizeof(buf), "VkResult(%d)", static_cast<int>(r));
    return buf;
}

bool vk_check(VkResult r, const char* what) {
    if (r >= 0) return true;
    log::errorf("rhi", "%s failed: %s", what, vk_result_name(r));
    return false;
}

Loader::~Loader() { close(); }

bool Loader::open() {
    if (lib_) return true;
    lib_ = open_library();
    if (!lib_) {
        log::infof("rhi", "Vulkan loader library not found");
        return false;
    }
    global_.vkGetInstanceProcAddr = get_gipa(lib_);
    if (!global_.vkGetInstanceProcAddr) {
        log::errorf("rhi", "vkGetInstanceProcAddr missing from the Vulkan loader");
        close();
        return false;
    }
#define X(name) global_.name = reinterpret_cast<PFN_##name>(global_.vkGetInstanceProcAddr(nullptr, #name));
    ALAMO_VK_GLOBAL_FUNCS(X)
#undef X
    if (!global_.vkCreateInstance || !global_.vkEnumerateInstanceExtensionProperties ||
        !global_.vkEnumerateInstanceLayerProperties) {
        log::errorf("rhi", "Vulkan loader is missing global entry points");
        close();
        return false;
    }
    return true;
}

void Loader::close() {
    if (lib_) close_library(lib_);
    lib_ = nullptr;
    global_ = GlobalFns{};
}

uint32_t Loader::instance_version() const {
    uint32_t v = VK_API_VERSION_1_0;
    if (global_.vkEnumerateInstanceVersion) global_.vkEnumerateInstanceVersion(&v);
    return v;
}

bool Loader::load_instance(VkInstance instance, InstanceFns& out) const {
    out = InstanceFns{};
    bool ok = true;
    const PFN_vkGetInstanceProcAddr gipa = global_.vkGetInstanceProcAddr;
#define X(name)                                                                 \
    out.name = reinterpret_cast<PFN_##name>(gipa(instance, #name));             \
    if (!out.name) {                                                            \
        log::errorf("rhi", "missing instance entry point %s", #name);           \
        ok = false;                                                             \
    }
    ALAMO_VK_INSTANCE_FUNCS(X)
#undef X
#define X(name) out.name = reinterpret_cast<PFN_##name>(gipa(instance, #name));
    ALAMO_VK_INSTANCE_OPT_FUNCS(X)
#undef X
    out.vkCreateWin32SurfaceKHR = gipa(instance, "vkCreateWin32SurfaceKHR");
    out.vkGetPhysicalDeviceWin32PresentationSupportKHR =
        gipa(instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    return ok;
}

bool Loader::load_device(const InstanceFns& inst, VkDevice device, DeviceFns& out) {
    out = DeviceFns{};
    bool ok = true;
    const PFN_vkGetDeviceProcAddr gdpa = inst.vkGetDeviceProcAddr;
#define X(name)                                                       \
    out.name = reinterpret_cast<PFN_##name>(gdpa(device, #name));     \
    if (!out.name) {                                                  \
        log::errorf("rhi", "missing device entry point %s", #name);   \
        ok = false;                                                   \
    }
    ALAMO_VK_DEVICE_FUNCS(X)
#undef X
#define X(name) out.name = reinterpret_cast<PFN_##name>(gdpa(device, #name));
    ALAMO_VK_DEVICE_OPT_FUNCS(X)
#undef X
    return ok;
}

}  // namespace alamo::rhi

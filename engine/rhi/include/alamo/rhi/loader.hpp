// Runtime loader for the Vulkan loader library (vulkan-1.dll / libvulkan.so.1). Nothing links
// against Vulkan at build time; a machine without a driver simply fails Loader::open().
#pragma once
#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Loader {
public:
    Loader() = default;
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // Loads the library and the global table. False (and logs at Info) if Vulkan is unavailable.
    bool open();
    void close();
    bool is_open() const { return lib_ != nullptr; }

    const GlobalFns& global() const { return global_; }
    // Highest instance version the loader supports (VK_API_VERSION_1_0 if vkEnumerateInstanceVersion is missing).
    uint32_t instance_version() const;

    // Fill tables. Return false if a required (non-OPT) entry point is missing; logs the names.
    bool load_instance(VkInstance instance, InstanceFns& out) const;
    static bool load_device(const InstanceFns& inst, VkDevice device, DeviceFns& out);

private:
    void* lib_ = nullptr;
    GlobalFns global_{};
};

}  // namespace alamo::rhi

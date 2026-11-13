// Vulkan 1.3 instance + device + queues.
//
// Requirements (a device missing any of them is skipped): API 1.3, dynamicRendering,
// synchronization2, shaderDemoteToHelperInvocation (HLSL discard), timelineSemaphore, descriptorIndexing (runtimeDescriptorArray,
// descriptorBindingPartiallyBound, shaderSampledImageArrayNonUniformIndexing,
// descriptorBindingSampledImageUpdateAfterBind, descriptorBindingUpdateUnusedWhilePending), a graphics queue
// (that can present when ContextDesc::presentation is set). Discrete GPUs are preferred, then
// integrated, then the largest device-local heap. ALAMO_VK_DEVICE=<index> overrides the choice.
//
// Queues: graphics (+present) and transfer. Transfer uses a dedicated family when the device has
// one (AMD: SDMA) and otherwise aliases the graphics queue; the accessors always return a usable
// Queue. (Compute work runs on the graphics queue: no dedicated async-compute queue is created.) Queue::submit() locks a per-VkQueue mutex, so several
// threads (frame loop, upload workers) may submit to the same queue.
//
// Validation: VK_LAYER_KHRONOS_validation is enabled only when the loader lists it (the loader
// also honours VK_LAYER_PATH). Messages go to alamo::log channel "vk"; errors/warnings are counted.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "alamo/rhi/loader.hpp"
#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

enum class Validation : uint8_t { Off, IfAvailable, Required };

struct ContextDesc {
    const char* app_name = "alamo";
    Validation validation = Validation::IfAvailable;
    bool presentation = false;   // VK_KHR_surface (+ win32) and VK_KHR_swapchain; graphics queue must present
    bool use_transfer_queue = true;  // use a dedicated transfer family when present
    int device_index = -1;           // -1: automatic (env ALAMO_VK_DEVICE overrides)
    // [port] FSR upscaling (render/src/fsr.cpp): also enable every optional feature the device supports in the core
    // 1.0-1.3 feature structs (except robustBufferAccess / robustImageAccess) and, when present, VK_KHR_cooperative_matrix,
    // VK_KHR_shader_bfloat16 and VK_EXT_shader_float8 with their features - what AMD's FSR 4 ML upscaler may use
    // (fp16 / int8 / dot product / wave matrix). Rendering itself never relies on them. DeviceCaps::upscaler_features.
    bool upscaler_features = false;
};

struct Queue {
    VkQueue handle = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    // Thread-safe (locks the mutex shared by every alias of this VkQueue).
    VkResult submit(std::span<const VkSubmitInfo2> submits, VkFence fence = VK_NULL_HANDLE) const;
    VkResult wait_idle() const;
    std::mutex* mutex = nullptr;
    const DeviceFns* fn = nullptr;
};

// Pure queue-family choice, exposed for tests. present_ok may be empty (= every family presents).
struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;
    uint32_t transfer = UINT32_MAX;  // == graphics when no usable dedicated transfer family
    bool valid() const { return graphics != UINT32_MAX; }
};
QueueFamilies select_queue_families(std::span<const VkQueueFamilyProperties> families,
                                    std::span<const bool> present_ok, bool want_transfer = true);

struct DeviceCaps {
    std::string name;
    uint32_t vendor_id = 0, device_id = 0;
    uint32_t api_version = 0, driver_version = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint64_t device_local_bytes = 0;   // the largest device-local memory heap (video memory; shared memory on an integrated GPU)
    bool texture_bc = false;
    bool sampler_anisotropy = false;
    float max_anisotropy = 1.0f;
    bool fill_mode_non_solid = false;  // LINE/POINT polygon modes (else pipelines fall back to FILL)
    bool depth_clamp = false;
    bool image_cube_array = false;     // CUBE_ARRAY views (cube images with more than 6 layers)
    float max_sampler_lod_bias = 0.0f; // |mipLodBias| limit (SamplerCache clamps to it)
    bool update_after_bind_sampled_image = false;
    bool variable_descriptor_count = false;
    bool scalar_block_layout = false;
    bool shader_draw_parameters = false;
    bool sampler_mirror_clamp_to_edge = false;
    uint32_t max_bindless_sampled_images = 0;  // update-after-bind sampled images per stage/set
    VkDeviceSize min_uniform_buffer_offset_alignment = 256;
    VkDeviceSize min_storage_buffer_offset_alignment = 256;
    VkDeviceSize non_coherent_atom_size = 256;
    uint32_t max_push_constants_size = 128;
    float timestamp_period_ns = 1.0f;   // ns per timestamp tick
    uint32_t timestamp_valid_bits = 0;  // graphics queue family; 0 = no timestamps
    bool host_query_reset = false;
    bool upscaler_features = false;      // ContextDesc::upscaler_features took effect
    bool cooperative_matrix = false;     // VK_KHR_cooperative_matrix enabled (with upscaler_features)
    bool shader_float8 = false;          // VK_EXT_shader_float8 enabled (with upscaler_features)
    uint8_t pipeline_cache_uuid[VK_UUID_SIZE] = {};
};

class Context {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // False when no Vulkan loader / no suitable device (logged). Safe to call shutdown() after.
    bool init(const ContextDesc& desc = {});
    void shutdown();
    bool valid() const { return device_ != VK_NULL_HANDLE; }

    const Loader& loader() const { return loader_; }
    const InstanceFns& vki() const { return vki_; }
    const DeviceFns& vkd() const { return vkd_; }
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_; }
    VkDevice device() const { return device_; }
    const DeviceCaps& caps() const { return caps_; }
    const VkPhysicalDeviceMemoryProperties& memory_properties() const { return mem_props_; }

    const Queue& graphics() const { return queues_[0]; }
    const Queue& transfer() const { return queues_[slot_transfer_]; }
    bool has_transfer_queue() const { return slot_transfer_ != 0; }
    bool presentation_enabled() const { return presentation_; }
    bool instance_extension_enabled(const char* name) const;

    bool validation_enabled() const { return validation_; }
    uint32_t validation_errors() const { return errors_.load(); }
    uint32_t validation_warnings() const { return warnings_.load(); }
    // [port] FSR: messages raised on this thread while a ThirdPartyValidation scope is open - inside a closed third-party
    // runtime's own Vulkan calls (AMD's FSR DLL: its ML shaders use SPIR-V the pinned layer cannot judge) - are counted
    // here instead of in errors / warnings, and the first few are logged. Our own calls never run inside such a scope.
    uint32_t validation_third_party() const { return third_party_.load(); }
    void reset_validation_counts() { errors_ = 0; warnings_ = 0; third_party_ = 0; }
    class ThirdPartyValidation {
    public:
        ThirdPartyValidation();
        ~ThirdPartyValidation();
        ThirdPartyValidation(const ThirdPartyValidation&) = delete;
        ThirdPartyValidation& operator=(const ThirdPartyValidation&) = delete;
    };

    // Optimal-tiling (or linear) format feature check.
    bool format_supported(VkFormat format, VkFormatFeatureFlags features, bool optimal = true) const;

    // Records with `record` into a one-off graphics command buffer, submits and waits. Thread-safe.
    bool immediate_submit(const std::function<void(VkCommandBuffer)>& record);

    // VK_EXT_debug_utils object name (no-op when unavailable).
    void set_name(VkObjectType type, uint64_t handle, const char* name) const;

    VkResult wait_idle() const;  // vkDeviceWaitIdle with every queue locked

    // Debug-messenger entry (public for the C callback).
    void on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                          const char* message);

private:
    bool create_instance(const ContextDesc& desc);
    bool pick_device(const ContextDesc& desc);
    bool create_device(const ContextDesc& desc);

    Loader loader_;
    InstanceFns vki_{};
    DeviceFns vkd_{};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    DeviceCaps caps_{};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    QueueFamilies families_{};

    Queue queues_[2];  // [0] graphics, then the dedicated transfer queue when present
    std::unique_ptr<std::mutex> queue_mutex_[2];
    uint32_t slot_transfer_ = 0, queue_count_ = 1;

    bool presentation_ = false;
    bool validation_ = false;
    bool debug_utils_ = false;
    std::atomic<uint32_t> errors_{0}, warnings_{0}, third_party_{0};
    std::vector<std::string> instance_extensions_;

    std::mutex immediate_mutex_;
    VkCommandPool immediate_pool_ = VK_NULL_HANDLE;
    VkFence immediate_fence_ = VK_NULL_HANDLE;

    // Device feature support captured during selection, used to enable optional features.
    struct Support;
    std::unique_ptr<Support> support_;
};

}  // namespace alamo::rhi

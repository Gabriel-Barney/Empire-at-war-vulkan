#include "alamo/rhi/context.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "alamo/core/log.hpp"
#include "alamo/rhi/surface.hpp"

namespace alamo::rhi {

namespace {

// Matches PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR without pulling in <windows.h>.
using PfnWin32PresentSupport = VkBool32(VKAPI_PTR*)(VkPhysicalDevice, uint32_t);

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT types,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user) {
    if (user && data && data->pMessage) static_cast<Context*>(user)->on_debug_message(severity, types, data->pMessage);
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT messenger_info(Context* ctx) {
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_callback;
    ci.pUserData = ctx;
    return ci;
}

thread_local int t_third_party_depth = 0;  // open Context::ThirdPartyValidation scopes on this thread

}  // namespace

Context::ThirdPartyValidation::ThirdPartyValidation() { ++t_third_party_depth; }
Context::ThirdPartyValidation::~ThirdPartyValidation() { --t_third_party_depth; }

// [port] FSR: optional extension feature structs for ContextDesc::upscaler_features, handled generically (sType, pNext,
// then only VkBool32 members; `bools` of them). Raw values so the pinned Vulkan-Headers need not know the newer
// extensions (VK_EXT_shader_float8 / VK_KHR_shader_bfloat16 came after header 296).
struct ExtFeatureSpec {
    const char* name;
    VkStructureType stype;
    uint32_t bools;
};
constexpr ExtFeatureSpec kUpscalerExt[] = {
    {"VK_KHR_cooperative_matrix", static_cast<VkStructureType>(1000506000), 2},  // cooperativeMatrix, ...RobustBufferAccess
    {"VK_KHR_shader_bfloat16", static_cast<VkStructureType>(1000141000), 3},     // type, dot product, cooperative matrix
    {"VK_EXT_shader_float8", static_cast<VkStructureType>(1000567000), 2},       // shaderFloat8, ...CooperativeMatrix
};
constexpr uint32_t kUpscalerExtCount = static_cast<uint32_t>(std::size(kUpscalerExt));
struct ExtFeatures {
    VkStructureType sType;
    void* pNext;
    VkBool32 b[8];
};

struct Context::Support {
    VkPhysicalDeviceFeatures2 f2{};
    VkPhysicalDeviceVulkan11Features f11{};
    VkPhysicalDeviceVulkan12Features f12{};
    VkPhysicalDeviceVulkan13Features f13{};
    std::vector<VkExtensionProperties> extensions;
    ExtFeatures ext[kUpscalerExtCount]{};  // queried only for extensions the device lists (else sType 0)

    void query(const InstanceFns& vki, VkPhysicalDevice pd) {
        f2 = {};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f11 = {};
        f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        f12 = {};
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        f13 = {};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        uint32_t n = 0;
        vki.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
        extensions.resize(n);
        vki.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, extensions.data());
        extensions.resize(n);
        void** tail = &f13.pNext;
        for (uint32_t i = 0; i < kUpscalerExtCount; ++i) {
            ext[i] = {};
            if (!has_extension(extensions, kUpscalerExt[i].name)) continue;
            ext[i].sType = kUpscalerExt[i].stype;
            *tail = &ext[i];
            tail = &ext[i].pNext;
        }
        vki.vkGetPhysicalDeviceFeatures2(pd, &f2);
        f13.pNext = nullptr;  // the copies below must not carry this chain
        for (ExtFeatures& e : ext) e.pNext = nullptr;
    }
    bool ext_supported(uint32_t i) const {
        if (ext[i].sType == 0) return false;
        for (uint32_t k = 0; k < kUpscalerExt[i].bools; ++k)
            if (ext[i].b[k]) return true;
        return false;
    }
    // Empty string = OK, else the first missing requirement.
    const char* missing_requirement() const {
        if (!f13.dynamicRendering) return "dynamicRendering";
        if (!f13.synchronization2) return "synchronization2";
        // DXC (vulkan1.3 target) lowers HLSL discard to OpDemoteToHelperInvocation.
        if (!f13.shaderDemoteToHelperInvocation) return "shaderDemoteToHelperInvocation";
        if (!f12.timelineSemaphore) return "timelineSemaphore";
        if (!f12.descriptorIndexing) return "descriptorIndexing";
        if (!f12.runtimeDescriptorArray) return "runtimeDescriptorArray";
        if (!f12.descriptorBindingPartiallyBound) return "descriptorBindingPartiallyBound";
        if (!f12.shaderSampledImageArrayNonUniformIndexing) return "shaderSampledImageArrayNonUniformIndexing";
        // Bindless texture table (materials hot set): stream textures into the bound array.
        if (!f12.descriptorBindingSampledImageUpdateAfterBind) return "descriptorBindingSampledImageUpdateAfterBind";
        if (!f12.descriptorBindingUpdateUnusedWhilePending) return "descriptorBindingUpdateUnusedWhilePending";
        return "";
    }
};

// ---------------------------------------------------------------------------------------------

VkResult Queue::submit(std::span<const VkSubmitInfo2> submits, VkFence fence) const {
    std::lock_guard<std::mutex> lock(*mutex);
    return fn->vkQueueSubmit2(handle, static_cast<uint32_t>(submits.size()), submits.data(), fence);
}

VkResult Queue::wait_idle() const {
    std::lock_guard<std::mutex> lock(*mutex);
    return fn->vkQueueWaitIdle(handle);
}

QueueFamilies select_queue_families(std::span<const VkQueueFamilyProperties> families,
                                    std::span<const bool> present_ok, bool want_transfer) {
    QueueFamilies q;
    const auto n = static_cast<uint32_t>(families.size());
    for (uint32_t i = 0; i < n; ++i) {
        const auto& f = families[i];
        if (!f.queueCount || !(f.queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (!present_ok.empty() && (i >= present_ok.size() || !present_ok[i])) continue;
        // Prefer a family that also has compute (universal queue).
        if (q.graphics == UINT32_MAX || ((f.queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                                         !(families[q.graphics].queueFlags & VK_QUEUE_COMPUTE_BIT)))
            q.graphics = i;
    }
    if (!q.valid()) return q;
    q.transfer = q.graphics;
    if (want_transfer) {
        for (uint32_t i = 0; i < n; ++i) {
            const auto& f = families[i];
            const bool transfer_only = (f.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                                       !(f.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));
            // Granularity (0,0,0) means whole mips only and anything > 1 restricts partial copies;
            // the uploader copies arbitrary small mips, so require (1,1,1).
            const VkExtent3D& g = f.minImageTransferGranularity;
            if (f.queueCount && transfer_only && g.width == 1 && g.height == 1 && g.depth == 1) {
                q.transfer = i;
                break;
            }
        }
    }
    return q;
}

// ---------------------------------------------------------------------------------------------

Context::Context() = default;
Context::~Context() { shutdown(); }

void Context::on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                               const char* message) {
    // GENERAL-only messages come from the loader (e.g. "layer forced disabled by VK_LOADER_LAYERS_DISABLE");
    // they are logged but not counted.
    if (!(types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT))) {
        log::debugf("vk", "%s", message);
        return;
    }
    if (t_third_party_depth > 0) {  // inside a third-party runtime's own calls (ThirdPartyValidation)
        if (++third_party_ <= 3) log::infof("vk", "third-party runtime: %s", message);
        return;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++errors_;
        log::errorf("vk", "%s", message);
    } else {
        ++warnings_;
        log::warnf("vk", "%s", message);
    }
}

bool Context::init(const ContextDesc& desc) {
    shutdown();
    if (!loader_.open()) return false;
    if (!create_instance(desc) || !pick_device(desc) || !create_device(desc)) {
        shutdown();
        return false;
    }
    log::infof("rhi", "device: %s (api %u.%u.%u)%s%s", caps_.name.c_str(), VK_API_VERSION_MAJOR(caps_.api_version),
               VK_API_VERSION_MINOR(caps_.api_version), VK_API_VERSION_PATCH(caps_.api_version),
               has_transfer_queue() ? ", transfer queue" : "",
               validation_ ? ", validation" : "");
    return true;
}

bool Context::create_instance(const ContextDesc& desc) {
    const GlobalFns& g = loader_.global();
    const uint32_t ver = loader_.instance_version();
    if (ver < VK_API_VERSION_1_3) {
        log::infof("rhi", "Vulkan loader is %u.%u, need 1.3", VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver));
        return false;
    }

    uint32_t n = 0;
    g.vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    g.vkEnumerateInstanceLayerProperties(&n, layers.data());
    layers.resize(n);
    bool have_validation = false;
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, kValidationLayer) == 0) have_validation = true;
    validation_ = desc.validation != Validation::Off && have_validation;
    if (desc.validation == Validation::Required && !have_validation) {
        log::errorf("rhi", "%s required but not available (VK_LAYER_PATH=%s)", kValidationLayer,
                    std::getenv("VK_LAYER_PATH") ? std::getenv("VK_LAYER_PATH") : "");
        return false;
    }

    n = 0;
    g.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    g.vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    exts.resize(n);
    if (validation_) {  // layer-provided extensions (debug utils lives in the layer when the driver lacks it)
        uint32_t m = 0;
        g.vkEnumerateInstanceExtensionProperties(kValidationLayer, &m, nullptr);
        std::vector<VkExtensionProperties> le(m);
        g.vkEnumerateInstanceExtensionProperties(kValidationLayer, &m, le.data());
        le.resize(m);
        exts.insert(exts.end(), le.begin(), le.end());
    }

    std::vector<const char*> enabled;
    if (desc.presentation) {
        const char* needed[] = {VK_KHR_SURFACE_EXTENSION_NAME, platform_surface_extension()};
        for (const char* e : needed) {
            if (!has_extension(exts, e)) {
                log::errorf("rhi", "instance extension %s missing", e);
                return false;
            }
            enabled.push_back(e);
        }
        presentation_ = true;
    }
    debug_utils_ = has_extension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils_) enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instance_extensions_.assign(enabled.begin(), enabled.end());

    auto app = vk_struct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = desc.app_name;
    app.applicationVersion = 1;
    app.pEngineName = "alamo";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT mi = messenger_info(this);
    auto ci = vk_struct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ci.ppEnabledExtensionNames = enabled.data();
    const char* layer_names[] = {kValidationLayer};
    if (validation_) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layer_names;
    }
    if (debug_utils_ && validation_) ci.pNext = &mi;
    if (!vk_check(g.vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance")) {
        instance_ = VK_NULL_HANDLE;
        return false;
    }
    if (!loader_.load_instance(instance_, vki_)) return false;
    if (debug_utils_ && validation_ && vki_.vkCreateDebugUtilsMessengerEXT)
        vk_check(vki_.vkCreateDebugUtilsMessengerEXT(instance_, &mi, nullptr, &messenger_),
                 "vkCreateDebugUtilsMessengerEXT");
    return true;
}

bool Context::pick_device(const ContextDesc& desc) {
    uint32_t n = 0;
    vki_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vki_.vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    devices.resize(n);
    if (devices.empty()) {
        log::infof("rhi", "no Vulkan physical devices");
        return false;
    }

    int forced = desc.device_index;
    if (const char* env = std::getenv("ALAMO_VK_DEVICE")) forced = std::atoi(env);

    auto present_ok = [&](VkPhysicalDevice pd, uint32_t families) {
        std::vector<bool> ok(families, true);
        if (desc.presentation && vki_.vkGetPhysicalDeviceWin32PresentationSupportKHR) {
            auto fn = reinterpret_cast<PfnWin32PresentSupport>(vki_.vkGetPhysicalDeviceWin32PresentationSupportKHR);
            for (uint32_t i = 0; i < families; ++i) ok[i] = fn(pd, i) == VK_TRUE;
        }
        return ok;
    };

    uint64_t best_score = 0;
    for (uint32_t i = 0; i < devices.size(); ++i) {
        VkPhysicalDevice pd = devices[i];
        VkPhysicalDeviceProperties props{};
        vki_.vkGetPhysicalDeviceProperties(pd, &props);
        if (forced >= 0 && static_cast<uint32_t>(forced) != i) continue;
        if (props.apiVersion < VK_API_VERSION_1_3) {
            log::infof("rhi", "skip %s: api < 1.3", props.deviceName);
            continue;
        }
        auto sup = std::make_unique<Support>();
        sup->query(vki_, pd);
        if (const char* miss = sup->missing_requirement(); *miss) {
            log::infof("rhi", "skip %s: no %s", props.deviceName, miss);
            continue;
        }
        if (desc.presentation && !has_extension(sup->extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            log::infof("rhi", "skip %s: no VK_KHR_swapchain", props.deviceName);
            continue;
        }
        uint32_t qn = 0;
        vki_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> fams(qn);
        vki_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, fams.data());
        fams.resize(qn);
        const std::vector<bool> pok = present_ok(pd, qn);
        // std::vector<bool> has no data(); copy into a plain array for the span.
        std::unique_ptr<bool[]> pok_arr(new bool[qn ? qn : 1]);
        for (uint32_t k = 0; k < qn; ++k) pok_arr[k] = pok[k];
        const QueueFamilies qf = select_queue_families(fams, std::span<const bool>(pok_arr.get(), qn),
                                                       desc.use_transfer_queue);
        if (!qf.valid()) {
            log::infof("rhi", "skip %s: no graphics%s queue", props.deviceName, desc.presentation ? "+present" : "");
            continue;
        }
        VkPhysicalDeviceMemoryProperties mp{};
        vki_.vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        uint64_t local = 0;
        for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local = std::max<uint64_t>(local, mp.memoryHeaps[h].size);
        uint64_t rank = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) rank = 4;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) rank = 3;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) rank = 2;
        const uint64_t score = (rank << 48) + (local >> 20) + 1;
        if (score > best_score) {
            best_score = score;
            physical_ = pd;
            families_ = qf;
            support_ = std::move(sup);
            mem_props_ = mp;
            caps_ = DeviceCaps{};
            caps_.name = props.deviceName;
            caps_.vendor_id = props.vendorID;
            caps_.device_id = props.deviceID;
            caps_.api_version = props.apiVersion;
            caps_.driver_version = props.driverVersion;
            caps_.type = props.deviceType;
            caps_.device_local_bytes = local;
            caps_.max_anisotropy = props.limits.maxSamplerAnisotropy;
            caps_.max_sampler_lod_bias = props.limits.maxSamplerLodBias;
            caps_.min_uniform_buffer_offset_alignment = props.limits.minUniformBufferOffsetAlignment;
            caps_.min_storage_buffer_offset_alignment = props.limits.minStorageBufferOffsetAlignment;
            caps_.non_coherent_atom_size = props.limits.nonCoherentAtomSize;
            caps_.max_push_constants_size = props.limits.maxPushConstantsSize;
            caps_.timestamp_period_ns = props.limits.timestampPeriod;
            caps_.framebuffer_samples = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                                        props.limits.framebufferStencilSampleCounts;
            std::memcpy(caps_.pipeline_cache_uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
            auto p12 = vk_struct<VkPhysicalDeviceVulkan12Properties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
            auto p2 = vk_struct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
            p2.pNext = &p12;
            vki_.vkGetPhysicalDeviceProperties2(pd, &p2);
            caps_.max_bindless_sampled_images = std::min(p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                                         p12.maxDescriptorSetUpdateAfterBindSampledImages);
        }
    }
    if (!physical_) {
        log::infof("rhi", "no suitable Vulkan 1.3 device");
        return false;
    }
    return true;
}

bool Context::create_device(const ContextDesc& desc) {
    const Support& s = *support_;
    float priority = 1.0f;
    uint32_t fams[2] = {families_.graphics, families_.transfer};
    std::vector<VkDeviceQueueCreateInfo> qci;
    for (uint32_t i = 0; i < 2; ++i) {
        bool dup = false;
        for (uint32_t j = 0; j < i; ++j) dup |= fams[j] == fams[i];
        if (dup) continue;
        auto q = vk_struct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        q.queueFamilyIndex = fams[i];
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        qci.push_back(q);
    }

    auto f2 = vk_struct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    auto f11 = vk_struct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    auto f12 = vk_struct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    auto f13 = vk_struct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    f2.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    const VkPhysicalDeviceFeatures& a = s.f2.features;
    f2.features.samplerAnisotropy = a.samplerAnisotropy;
    f2.features.fillModeNonSolid = a.fillModeNonSolid;
    f2.features.textureCompressionBC = a.textureCompressionBC;
    f2.features.depthClamp = a.depthClamp;
    f2.features.imageCubeArray = a.imageCubeArray;  // CUBE_ARRAY views (Allocator::create_image)
    f2.features.depthBiasClamp = a.depthBiasClamp;
    f2.features.independentBlend = a.independentBlend;
    f2.features.shaderClipDistance = a.shaderClipDistance;
    f11.shaderDrawParameters = s.f11.shaderDrawParameters;
    f12.timelineSemaphore = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = s.f12.descriptorBindingVariableDescriptorCount;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.samplerMirrorClampToEdge = s.f12.samplerMirrorClampToEdge;  // D3DTADDRESS_MIRRORONCE
    f12.scalarBlockLayout = s.f12.scalarBlockLayout;
    f12.uniformBufferStandardLayout = s.f12.uniformBufferStandardLayout;
    f12.hostQueryReset = s.f12.hostQueryReset;  // vkResetQueryPool (optional; GpuTimer resets in the command buffer)
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    f13.maintenance4 = s.f13.maintenance4;

    std::vector<const char*> exts;
    if (desc.presentation) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (has_extension(s.extensions, "VK_KHR_portability_subset")) exts.push_back("VK_KHR_portability_subset");

    // [port] FSR (ContextDesc::upscaler_features): every supported core feature (enabling a supported feature is always
    // valid) except the robustness ones, which only cost time, plus the extension features the ML upscaler may use.
    ExtFeatures ext[kUpscalerExtCount]{};
    if (desc.upscaler_features) {
        f2.features = a;
        f2.features.robustBufferAccess = VK_FALSE;
        auto copy_bools = [](auto& dst, const auto& src) {  // keep dst's sType / pNext, take src's VkBool32 members
            void* next = dst.pNext;
            const VkStructureType st = dst.sType;
            dst = src;
            dst.sType = st;
            dst.pNext = next;
        };
        copy_bools(f11, s.f11);
        copy_bools(f12, s.f12);
        copy_bools(f13, s.f13);
        f13.robustImageAccess = VK_FALSE;
        void** tail = &f13.pNext;
        for (uint32_t i = 0; i < kUpscalerExtCount; ++i) {
            if (!s.ext_supported(i)) continue;
            // The pinned validation layer (SDK 1.3.296) does not know the structs added after header 296 and reports them as
            // invalid pNext entries; with validation on, those two extensions stay off (the runtime then uses another model).
            if (validation_ && i > 0) continue;
            ext[i] = s.ext[i];
            ext[i].pNext = nullptr;
            *tail = &ext[i];
            tail = &ext[i].pNext;
            exts.push_back(kUpscalerExt[i].name);
        }
    }

    auto ci = vk_struct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    ci.pNext = &f2;
    ci.queueCreateInfoCount = static_cast<uint32_t>(qci.size());
    ci.pQueueCreateInfos = qci.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    if (!vk_check(vki_.vkCreateDevice(physical_, &ci, nullptr, &device_), "vkCreateDevice")) {
        device_ = VK_NULL_HANDLE;
        return false;
    }
    if (!Loader::load_device(vki_, device_, vkd_)) return false;

    caps_.texture_bc = a.textureCompressionBC == VK_TRUE;
    caps_.sampler_anisotropy = a.samplerAnisotropy == VK_TRUE;
    caps_.fill_mode_non_solid = a.fillModeNonSolid == VK_TRUE;
    caps_.depth_clamp = a.depthClamp == VK_TRUE;
    caps_.image_cube_array = a.imageCubeArray == VK_TRUE;
    caps_.update_after_bind_sampled_image = s.f12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
    caps_.variable_descriptor_count = s.f12.descriptorBindingVariableDescriptorCount == VK_TRUE;
    caps_.scalar_block_layout = s.f12.scalarBlockLayout == VK_TRUE;
    caps_.shader_draw_parameters = s.f11.shaderDrawParameters == VK_TRUE;
    caps_.sampler_mirror_clamp_to_edge = s.f12.samplerMirrorClampToEdge == VK_TRUE;
    caps_.host_query_reset = s.f12.hostQueryReset == VK_TRUE;
    caps_.upscaler_features = desc.upscaler_features;
    caps_.cooperative_matrix = desc.upscaler_features && s.ext_supported(0);
    caps_.shader_float8 = desc.upscaler_features && s.ext_supported(2) && !validation_;
    {
        uint32_t qn = 0;
        vki_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qn);
        vki_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qprops.data());
        if (families_.graphics < qn) caps_.timestamp_valid_bits = qprops[families_.graphics].timestampValidBits;
    }

    // Queues: slot 0 graphics; transfer gets its own slot only for a distinct family.
    auto make_queue = [&](uint32_t slot, uint32_t family) {
        queue_mutex_[slot] = std::make_unique<std::mutex>();
        Queue& q = queues_[slot];
        q.family = family;
        q.mutex = queue_mutex_[slot].get();
        q.fn = &vkd_;
        vkd_.vkGetDeviceQueue(device_, family, 0, &q.handle);
    };
    make_queue(0, families_.graphics);
    queue_count_ = 1;
    slot_transfer_ = 0;
    if (families_.transfer != families_.graphics) {
        slot_transfer_ = queue_count_;
        make_queue(queue_count_++, families_.transfer);
    }

    auto pci = vk_struct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = families_.graphics;
    if (!vk_check(vkd_.vkCreateCommandPool(device_, &pci, nullptr, &immediate_pool_), "vkCreateCommandPool"))
        return false;
    auto fci = vk_struct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    if (!vk_check(vkd_.vkCreateFence(device_, &fci, nullptr, &immediate_fence_), "vkCreateFence")) return false;
    return true;
}

void Context::shutdown() {
    if (device_) {
        wait_idle();
        if (immediate_pool_) vkd_.vkDestroyCommandPool(device_, immediate_pool_, nullptr);
        if (immediate_fence_) vkd_.vkDestroyFence(device_, immediate_fence_, nullptr);
        vkd_.vkDestroyDevice(device_, nullptr);
    }
    immediate_pool_ = VK_NULL_HANDLE;
    immediate_fence_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    if (messenger_ && vki_.vkDestroyDebugUtilsMessengerEXT) vki_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    messenger_ = VK_NULL_HANDLE;
    if (instance_) vki_.vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    for (auto& q : queues_) q = Queue{};
    for (auto& m : queue_mutex_) m.reset();
    slot_transfer_ = 0;
    queue_count_ = 1;
    support_.reset();
    vki_ = InstanceFns{};
    vkd_ = DeviceFns{};
    presentation_ = validation_ = debug_utils_ = false;
    instance_extensions_.clear();
    loader_.close();
}

bool Context::instance_extension_enabled(const char* name) const {
    for (const std::string& e : instance_extensions_)
        if (e == name) return true;
    return false;
}

bool Context::format_supported(VkFormat format, VkFormatFeatureFlags features, bool optimal) const {
    VkFormatProperties p{};
    vki_.vkGetPhysicalDeviceFormatProperties(physical_, format, &p);
    const VkFormatFeatureFlags have = optimal ? p.optimalTilingFeatures : p.linearTilingFeatures;
    return (have & features) == features;
}

bool Context::immediate_submit(const std::function<void(VkCommandBuffer)>& record) {
    std::lock_guard<std::mutex> lock(immediate_mutex_);
    auto ai = vk_struct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    ai.commandPool = immediate_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!vk_check(vkd_.vkAllocateCommandBuffers(device_, &ai, &cmd), "vkAllocateCommandBuffers")) return false;
    auto bi = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkd_.vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    bool ok = vk_check(vkd_.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    if (ok) {
        auto cbi = vk_struct<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
        cbi.commandBuffer = cmd;
        auto si = vk_struct<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        vkd_.vkResetFences(device_, 1, &immediate_fence_);
        ok = vk_check(graphics().submit({&si, 1}, immediate_fence_), "immediate submit") &&
             vk_check(vkd_.vkWaitForFences(device_, 1, &immediate_fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }
    vkd_.vkFreeCommandBuffers(device_, immediate_pool_, 1, &cmd);
    return ok;
}

void Context::set_name(VkObjectType type, uint64_t handle, const char* name) const {
    if (!debug_utils_ || !vki_.vkSetDebugUtilsObjectNameEXT || !device_) return;
    auto ni = vk_struct<VkDebugUtilsObjectNameInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT);
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vki_.vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

VkResult Context::wait_idle() const {
    if (!device_) return VK_SUCCESS;
    std::unique_lock<std::mutex> l0, l1;
    if (queue_count_ > 0 && queue_mutex_[0]) l0 = std::unique_lock<std::mutex>(*queue_mutex_[0]);
    if (queue_count_ > 1 && queue_mutex_[1]) l1 = std::unique_lock<std::mutex>(*queue_mutex_[1]);
    return vkd_.vkDeviceWaitIdle(device_);
}

}  // namespace alamo::rhi

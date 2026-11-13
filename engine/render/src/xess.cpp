// alamo_render XeSS upscaling on the GPU [port: Gabriel 2026-11-13] (src/xess.hpp, alamo/render/upscale.hpp).
// The Intel XeSS SDK 3.0.2 C API (toolchain\13_xess.cmd) through libxess.dll, loaded at run time.
#include "xess.hpp"

#include <cstring>
#include <string>

#include "alamo/core/log.hpp"
#include "render_internal.hpp"
#include "taa.hpp"

#if defined(ALAMO_RENDER_HAVE_XESS) && defined(_WIN32)
#define ALAMO_XESS_RUNTIME 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <xess/xess.h>
#include <xess/xess_vk.h>
#endif

namespace alamo::render {

#ifdef ALAMO_XESS_RUNTIME

namespace {
constexpr VkFormat kOutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // storage-capable everywhere (the 8-bit BGRA is not)
constexpr const wchar_t* kDll = L"libxess.dll";

const char* xess_code(xess_result_t r) {
    switch (r) {
        case XESS_RESULT_SUCCESS: return "ok";
        case XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "warning: folder does not exist";
        case XESS_RESULT_WARNING_OLD_DRIVER: return "warning: old driver";
        case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device";
        case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
        case XESS_RESULT_ERROR_UNINITIALIZED: return "not initialised";
        case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of device memory";
        case XESS_RESULT_ERROR_DEVICE: return "device error";
        case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
        case XESS_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
        case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "operation in progress";
        case XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
        case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "cannot load library";
        case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong call order";
        default: return "unknown error";
    }
}
bool ok(xess_result_t r) { return r >= XESS_RESULT_SUCCESS; }  // warnings are > 0

xess_quality_settings_t xess_quality(UpscaleQuality q) {
    switch (q) {
        case UpscaleQuality::NativeAA: return XESS_QUALITY_SETTING_AA;
        case UpscaleQuality::Quality: return XESS_QUALITY_SETTING_QUALITY;
        case UpscaleQuality::Balanced: return XESS_QUALITY_SETTING_BALANCED;
        case UpscaleQuality::Performance: return XESS_QUALITY_SETTING_PERFORMANCE;
        case UpscaleQuality::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    }
    return XESS_QUALITY_SETTING_QUALITY;
}

// The runtime's messages (warnings / errors) go to the log; may be called from any thread.
void xess_message(const char* message, xess_logging_level_t level) {
    if (level >= XESS_LOGGING_LEVEL_ERROR) {
        log::errorf("render", "XeSS: %s", message ? message : "");
    } else {
        log::infof("render", "XeSS: %s", message ? message : "");
    }
}

xess_vk_image_view_info view_info(const rhi::Image& img, VkExtent2D ext) {
    xess_vk_image_view_info v{};
    v.imageView = img.view;
    v.image = img.handle;
    v.subresourceRange = img.range();
    v.format = img.vk_format();
    v.width = ext.width;
    v.height = ext.height;
    return v;
}

template <class F>
F proc(HMODULE dll, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}
}  // namespace

struct XessPass::Runtime {
    HMODULE dll = nullptr;
    decltype(&xessGetVersion) get_version = nullptr;
    decltype(&xessVKGetRequiredInstanceExtensions) instance_extensions = nullptr;
    decltype(&xessVKGetRequiredDeviceExtensions) device_extensions = nullptr;
    decltype(&xessVKCreateContext) create = nullptr;
    decltype(&xessVKInit) init = nullptr;
    decltype(&xessVKExecute) execute = nullptr;
    decltype(&xessDestroyContext) destroy = nullptr;
    decltype(&xessGetOptimalInputResolution) optimal_input = nullptr;
    decltype(&xessSetVelocityScale) velocity_scale = nullptr;
    decltype(&xessSetLoggingCallback) logging = nullptr;
    xess_context_handle_t ctx = nullptr;
    ~Runtime() {
        if (dll) FreeLibrary(dll);
    }
};

XessPass::XessPass() = default;
XessPass::~XessPass() { shutdown(); }

bool XessPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    wchar_t path[MAX_PATH];  // the executable's own folder only (no current-directory or PATH search)
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
    auto rt = std::make_unique<Runtime>();
    rt->dll = LoadLibraryExW((dir + kDll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!rt->dll) {
        status_ = "XeSS runtime not found (libxess.dll beside the executable)";
        log::infof("render", "%s", status_.c_str());
        return false;
    }
    rt->get_version = proc<decltype(rt->get_version)>(rt->dll, "xessGetVersion");
    rt->instance_extensions = proc<decltype(rt->instance_extensions)>(rt->dll, "xessVKGetRequiredInstanceExtensions");
    rt->device_extensions = proc<decltype(rt->device_extensions)>(rt->dll, "xessVKGetRequiredDeviceExtensions");
    rt->create = proc<decltype(rt->create)>(rt->dll, "xessVKCreateContext");
    rt->init = proc<decltype(rt->init)>(rt->dll, "xessVKInit");
    rt->execute = proc<decltype(rt->execute)>(rt->dll, "xessVKExecute");
    rt->destroy = proc<decltype(rt->destroy)>(rt->dll, "xessDestroyContext");
    rt->optimal_input = proc<decltype(rt->optimal_input)>(rt->dll, "xessGetOptimalInputResolution");
    rt->velocity_scale = proc<decltype(rt->velocity_scale)>(rt->dll, "xessSetVelocityScale");
    rt->logging = proc<decltype(rt->logging)>(rt->dll, "xessSetLoggingCallback");
    if (!rt->get_version || !rt->instance_extensions || !rt->device_extensions || !rt->create || !rt->init || !rt->execute ||
        !rt->destroy || !rt->optimal_input || !rt->velocity_scale || !rt->logging) {
        status_ = "XeSS runtime incomplete (libxess.dll lacks the xessVK* entry points)";
        log::errorf("render", "%s", status_.c_str());
        return false;
    }
    xess_version_t ver{};
    std::string name = "XeSS";
    if (ok(rt->get_version(&ver))) name += " " + std::to_string(ver.major) + "." + std::to_string(ver.minor) + "." + std::to_string(ver.patch);
    // What XeSS needs from the instance and device; the context was created before the runtime was loaded, so anything
    // missing turns XeSS off (rhi::ContextDesc::upscaler_features enables what it asks for on current drivers).
    if (!ctx.caps().upscaler_features) {
        status_ = name + " off: the device was created without the upscaler features";
        log::infof("render", "%s", status_.c_str());
        return false;
    }
    uint32_t count = 0, min_api = 0;
    const char* const* names = nullptr;
    xess_result_t r = rt->instance_extensions(&count, &names, &min_api);
    if (!ok(r)) {
        status_ = name + " off: " + xess_code(r) + " (instance requirements)";
        log::infof("render", "%s", status_.c_str());
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (names && names[i] && !ctx.instance_extension_enabled(names[i])) {
            status_ = name + " off: instance extension " + names[i] + " not enabled";
            log::infof("render", "%s", status_.c_str());
            return false;
        }
    }
    if (min_api > VK_API_VERSION_1_3) {
        status_ = name + " off: needs a newer Vulkan API version than 1.3";
        log::infof("render", "%s", status_.c_str());
        return false;
    }
    count = 0;
    names = nullptr;
    r = rt->device_extensions(ctx.instance(), ctx.physical_device(), &count, &names);
    if (!ok(r)) {
        status_ = name + " off: " + xess_code(r) + " (device requirements)";
        log::infof("render", "%s", status_.c_str());
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (names && names[i] && !ctx.device_extension_enabled(names[i])) {
            status_ = name + " off: device extension " + names[i] + " not enabled";
            log::infof("render", "%s", status_.c_str());
            return false;
        }
    }
    rt_ = std::move(rt);
    available_ = true;
    status_ = name;
    return true;
}

void XessPass::destroy_context() {
    if (rt_ && rt_->ctx) {
        const rhi::Context::ThirdPartyValidation tp;  // the runtime's own Vulkan calls (rhi/context.hpp)
        rt_->destroy(rt_->ctx);
        rt_->ctx = nullptr;
    }
    display_ = {0, 0};
    render_ = {0, 0};
}

void XessPass::shutdown() {
    if (ctx_ && (rt_ && rt_->ctx)) ctx_->wait_idle();
    destroy_context();
    output_.shutdown();
    rt_.reset();
    available_ = false;
    ctx_ = nullptr;
    alloc_ = nullptr;
}

bool XessPass::prepare(VkExtent2D display, UpscaleQuality quality) {
    if (!available_) return false;
    if (rt_->ctx && display_.width == display.width && display_.height == display.height && quality_ == quality &&
        output_.has_color())
        return true;
    const auto fail = [&](const std::string& why) {
        status_ = why;
        log::errorf("render", "%s", status_.c_str());
        available_ = false;  // the device / driver will not do better on the next frame
        destroy_context();
        output_.shutdown();
        return false;
    };
    if (rt_->ctx || output_.has_color()) ctx_->wait_idle();  // re-init: the old output / XeSS work may be in flight
    if (!rt_->ctx) {
        xess_result_t r;
        {
            const rhi::Context::ThirdPartyValidation tp;
            r = rt_->create(ctx_->instance(), ctx_->physical_device(), ctx_->device(), &rt_->ctx);
        }
        if (!ok(r) || !rt_->ctx) {
            rt_->ctx = nullptr;
            return fail(std::string("XeSS context creation failed: ") + xess_code(r));
        }
        rt_->logging(rt_->ctx, XESS_LOGGING_LEVEL_WARNING, xess_message);
    }
    if (display_.width != display.width || display_.height != display.height || !output_.has_color()) {
        output_.shutdown();
        rhi::RenderTargetDesc d;
        d.color = rhi::from_vk(kOutputFormat);
        d.extra_color_usage = VK_IMAGE_USAGE_STORAGE_BIT;
        d.name = "render.xess_output";
        if (!output_.init(*alloc_, d, display)) {
            log::errorf("render", "XeSS output target could not be created (%ux%u)", display.width, display.height);
            return fail("XeSS output target could not be created");
        }
    }
    const xess_2d_t out{display.width, display.height};
    xess_2d_t optimal{}, lo{}, hi{};
    xess_result_t r = rt_->optimal_input(rt_->ctx, &out, xess_quality(quality), &optimal, &lo, &hi);
    if (!ok(r) || optimal.x == 0 || optimal.y == 0) return fail(std::string("XeSS input resolution query failed: ") + xess_code(r));
    xess_vk_init_params_t ip{};
    ip.outputResolution = out;
    ip.qualitySetting = xess_quality(quality);
    ip.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR;  // display-referred 8-bit colour; low-res motion vectors + depth
    {
        const rhi::Context::ThirdPartyValidation tp;  // its kernels / weights upload
        r = rt_->init(rt_->ctx, &ip);
    }
    if (!ok(r)) return fail(std::string("XeSS initialisation failed: ") + xess_code(r));
    display_ = display;
    quality_ = quality;
    render_ = {optimal.x, optimal.y};
    log::infof("render", "XeSS upscaling: %s, display %ux%u, render %ux%u", status_.c_str(), display.width, display.height,
               render_.width, render_.height);
    return true;
}

bool XessPass::dispatch(VkCommandBuffer cmd, rhi::RenderTarget& scene, TaaPass& taa, bool reset) {
    if (!available_ || !rt_->ctx || !output_.has_color()) return false;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkExtent2D render = scene.extent();
    scene.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    output_.transition_color(fn, cmd, rhi::ImageState::General);
    rhi::RenderTarget& mv = taa.fsr_motion();
    rhi::RenderTarget& depth = taa.fsr_depth();
    // Velocity in uv units -> pixels of the input (the scale is a context setting; render size can change per frame).
    rt_->velocity_scale(rt_->ctx, static_cast<float>(render.width), static_cast<float>(render.height));
    xess_vk_execute_params_t ep{};
    ep.colorTexture = view_info(scene.color(), render);
    ep.velocityTexture = view_info(mv.color(), render);
    ep.depthTexture = view_info(depth.color(), render);
    ep.outputTexture = view_info(output_.color(), display_);
    // Render pixels, +y down: the same convention as FSR's jitterOffset (checked 2026-11-13 on Endor: the three flipped
    // signs blur the picture and keep a still camera moving, alamo_render_xess_tests' settle check fails).
    ep.jitterOffsetX = taa.fsr_jitter_x();
    ep.jitterOffsetY = taa.fsr_jitter_y();
    ep.exposureScale = 1.0f;
    ep.resetHistory = reset ? 1u : 0u;
    ep.inputWidth = render.width;
    ep.inputHeight = render.height;
    xess_result_t r;
    {
        // Only the commands the runtime records itself; our images' layouts are still checked at submit.
        const rhi::Context::ThirdPartyValidation tp;
        r = rt_->execute(rt_->ctx, cmd, &ep);
    }
    if (!ok(r)) {
        status_ = std::string("XeSS dispatch failed: ") + xess_code(r);
        log::errorf("render", "%s", status_.c_str());
        available_ = false;  // later frames fall back to TAA at full size
        return false;
    }
    // XeSS does no synchronisation and changes no layouts: the inputs stay ShaderRead, the output General. The next
    // transition (the copy out) orders after its compute writes.
    return true;
}

#else  // built without the XeSS SDK headers

struct XessPass::Runtime {};

XessPass::XessPass() = default;
XessPass::~XessPass() { shutdown(); }

bool XessPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    ctx_ = &ctx;
    alloc_ = &alloc;
    status_ = "XeSS not built (toolchain\\13_xess.cmd was not run before configuring)";
    return false;
}
void XessPass::destroy_context() {}
void XessPass::shutdown() {
    output_.shutdown();
    available_ = false;
    ctx_ = nullptr;
    alloc_ = nullptr;
}
bool XessPass::prepare(VkExtent2D, UpscaleQuality) { return false; }
bool XessPass::dispatch(VkCommandBuffer, rhi::RenderTarget&, TaaPass&, bool) { return false; }

#endif

}  // namespace alamo::render

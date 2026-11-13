// alamo_render FSR upscaling on the GPU [port: Gabriel 2026-11-13] (src/fsr.hpp, alamo/render/upscale.hpp).
// The AMD FidelityFX SDK 2.4.0 C API (toolchain\11_fidelityfx.cmd) through its loader DLL, loaded at run time.
#include "fsr.hpp"

#include <cstring>
#include <string>

#include "alamo/core/log.hpp"
#include "render_internal.hpp"
#include "taa.hpp"

#if defined(ALAMO_RENDER_HAVE_FFX) && defined(_WIN32)
#define ALAMO_FSR_RUNTIME 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>
#endif

namespace alamo::render {

#ifdef ALAMO_FSR_RUNTIME

namespace {
constexpr VkFormat kOutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // storage-capable everywhere (the 8-bit BGRA is not)
constexpr const wchar_t* kLoaderDll = L"amd_fidelityfx_loader_vk.dll";

const char* ffx_code(ffxReturnCode_t c) {
    switch (c) {
        case FFX_API_RETURN_OK: return "ok";
        case FFX_API_RETURN_ERROR: return "error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown desc type";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
        case FFX_API_RETURN_NO_PROVIDER: return "no provider (GPU / driver without FSR on Vulkan)";
        case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER: return "invalid parameter";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "provider too old";
        default: return "unknown return code";
    }
}

// The runtime's messages (warnings / errors from debug checking) go to the log.
void ffx_message(uint32_t type, const wchar_t* message) {
    std::string m;
    for (const wchar_t* p = message; p && *p; ++p) m.push_back(*p < 128 ? static_cast<char>(*p) : '?');
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        log::errorf("render", "FSR: %s", m.c_str());
    } else {
        log::infof("render", "FSR: %s", m.c_str());
    }
}

FfxApiResource image_resource(VkImage image, VkFormat format, VkExtent2D ext, uint32_t usage, uint32_t state) {
    FfxApiResource r{};
    r.resource = reinterpret_cast<void*>(image);
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(format);
    r.description.width = ext.width;
    r.description.height = ext.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    return r;
}
}  // namespace

struct FsrPass::Runtime {
    HMODULE dll = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    ffxContext ctx = nullptr;
    ~Runtime() {
        if (dll) FreeLibrary(dll);
    }
};

FsrPass::FsrPass() = default;
FsrPass::~FsrPass() { shutdown(); }

bool FsrPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    // The executable's own folder only (no current-directory or PATH search for the loader); the loader finds the
    // effect DLL next to itself.
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
    auto rt = std::make_unique<Runtime>();
    rt->dll = LoadLibraryExW((dir + kLoaderDll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!rt->dll) {
        status_ = "FSR runtime not found (amd_fidelityfx_loader_vk.dll beside the executable)";
        log::infof("render", "%s", status_.c_str());
        return false;
    }
    rt->create = reinterpret_cast<PfnFfxCreateContext>(reinterpret_cast<void*>(GetProcAddress(rt->dll, "ffxCreateContext")));
    rt->destroy = reinterpret_cast<PfnFfxDestroyContext>(reinterpret_cast<void*>(GetProcAddress(rt->dll, "ffxDestroyContext")));
    rt->configure = reinterpret_cast<PfnFfxConfigure>(reinterpret_cast<void*>(GetProcAddress(rt->dll, "ffxConfigure")));
    rt->query = reinterpret_cast<PfnFfxQuery>(reinterpret_cast<void*>(GetProcAddress(rt->dll, "ffxQuery")));
    rt->dispatch = reinterpret_cast<PfnFfxDispatch>(reinterpret_cast<void*>(GetProcAddress(rt->dll, "ffxDispatch")));
    if (!rt->create || !rt->destroy || !rt->configure || !rt->query || !rt->dispatch) {
        status_ = "FSR runtime incomplete (amd_fidelityfx_loader_vk.dll lacks the ffx* entry points)";
        log::errorf("render", "%s", status_.c_str());
        return false;
    }
    rt_ = std::move(rt);
    available_ = true;
    status_ = "FSR runtime loaded";
    return true;
}

void FsrPass::destroy_context() {
    if (rt_ && rt_->ctx) {
        const rhi::Context::ThirdPartyValidation tp;  // the runtime's own Vulkan calls (rhi/context.hpp)
        rt_->destroy(&rt_->ctx, nullptr);
        rt_->ctx = nullptr;
    }
    display_ = {0, 0};
}

void FsrPass::shutdown() {
    if (ctx_ && (rt_ && rt_->ctx)) ctx_->wait_idle();
    destroy_context();
    output_.shutdown();
    rt_.reset();
    available_ = false;
    ctx_ = nullptr;
    alloc_ = nullptr;
}

bool FsrPass::prepare(VkExtent2D display) {
    if (!available_) return false;
    if (rt_->ctx && display_.width == display.width && display_.height == display.height && output_.has_color()) return true;
    if (rt_->ctx || output_.has_color()) ctx_->wait_idle();  // display resize: the old context / output may be in flight
    destroy_context();
    output_.shutdown();
    rhi::RenderTargetDesc d;
    d.color = rhi::from_vk(kOutputFormat);
    d.extra_color_usage = VK_IMAGE_USAGE_STORAGE_BIT;
    d.name = "render.fsr_output";
    if (!output_.init(*alloc_, d, display)) {
        status_ = "FSR output target could not be created";
        log::errorf("render", "%s (%ux%u)", status_.c_str(), display.width, display.height);
        return false;
    }
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = ctx_->device();
    backend.deviceContext.vkPhysicalDevice = ctx_->physical_device();
    backend.deviceContext.vkDeviceProcAddr = ctx_->vki().vkGetDeviceProcAddr;
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    cd.maxRenderSize = {display.width, display.height};  // any quality mode without a new context
    cd.maxUpscaleSize = {display.width, display.height};
    cd.fpMessage = ffx_message;
    cd.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    ffxReturnCode_t rc;
    {
        const rhi::Context::ThirdPartyValidation tp;  // its ML shaders use SPIR-V the pinned layer cannot judge
        rc = rt_->create(&rt_->ctx, &cd.header, nullptr);
    }
    if (rc != FFX_API_RETURN_OK || !rt_->ctx) {
        rt_->ctx = nullptr;
        status_ = std::string("FSR context creation failed: ") + ffx_code(rc);
        log::errorf("render", "%s", status_.c_str());
        available_ = false;  // the device / driver will not do better on the next frame
        output_.shutdown();
        return false;
    }
    display_ = display;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (rt_->query(&rt_->ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) {
        status_ = pv.versionName;  // copied: the runtime may reuse the string (ffx-api.md "Version selection")
    } else {
        status_ = "FSR";
    }
    log::infof("render", "FSR upscaling: %s, display %ux%u", status_.c_str(), display.width, display.height);
    return true;
}

bool FsrPass::dispatch(VkCommandBuffer cmd, rhi::RenderTarget& scene, TaaPass& taa, float sharpness, float frame_ms, bool reset) {
    if (!available_ || !rt_->ctx || !output_.has_color()) return false;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkExtent2D render = scene.extent();
    scene.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    output_.transition_color(fn, cmd, rhi::ImageState::General);
    rhi::RenderTarget& mv = taa.fsr_motion();
    rhi::RenderTarget& depth = taa.fsr_depth();
    const uint32_t read = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;  // SHADER_READ_ONLY_OPTIMAL (the inputs' ShaderRead)
    ffxDispatchDescUpscale dd{};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = reinterpret_cast<void*>(cmd);
    dd.color = image_resource(scene.color().handle, scene.color().vk_format(), render, FFX_API_RESOURCE_USAGE_READ_ONLY, read);
    dd.depth = image_resource(depth.color().handle, depth.color().vk_format(), render, FFX_API_RESOURCE_USAGE_READ_ONLY, read);
    dd.motionVectors = image_resource(mv.color().handle, mv.color().vk_format(), render, FFX_API_RESOURCE_USAGE_READ_ONLY, read);
    dd.output = image_resource(output_.color().handle, output_.color().vk_format(), display_, FFX_API_RESOURCE_USAGE_UAV,
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.jitterOffset = {taa.fsr_jitter_x(), taa.fsr_jitter_y()};
    dd.motionVectorScale = {static_cast<float>(render.width), static_cast<float>(render.height)};  // uv -> pixels
    dd.renderSize = {render.width, render.height};
    dd.upscaleSize = {display_.width, display_.height};
    dd.enableSharpening = sharpness > 0.0f;
    dd.sharpness = sharpness < 0.0f ? 0.0f : (sharpness > 1.0f ? 1.0f : sharpness);
    dd.frameTimeDelta = frame_ms > 0.0f ? frame_ms : 16.6f;
    dd.preExposure = 1.0f;
    dd.reset = reset;
    const UpscaleCamera cam = upscale_camera(taa.camera());
    dd.cameraNear = cam.near_plane;
    dd.cameraFar = cam.far_plane;
    dd.cameraFovAngleVertical = cam.fov_y;
    dd.viewSpaceToMetersFactor = 1.0f;
    dd.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    ffxReturnCode_t rc;
    {
        // Only the commands the runtime records itself; our images' layouts are still checked at submit.
        const rhi::Context::ThirdPartyValidation tp;
        rc = rt_->dispatch(&rt_->ctx, &dd.header);
    }
    if (rc != FFX_API_RETURN_OK) {
        status_ = std::string("FSR dispatch failed: ") + ffx_code(rc);
        log::errorf("render", "%s", status_.c_str());
        available_ = false;  // later frames fall back to TAA at full size
        return false;
    }
    // The runtime returns every resource it was given to the state it was given in (General / ShaderRead).
    return true;
}

#else  // built without the FidelityFX SDK headers

struct FsrPass::Runtime {};

FsrPass::FsrPass() = default;
FsrPass::~FsrPass() { shutdown(); }

bool FsrPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    ctx_ = &ctx;
    alloc_ = &alloc;
    status_ = "FSR not built (toolchain\\11_fidelityfx.cmd was not run before configuring)";
    return false;
}
void FsrPass::destroy_context() {}
void FsrPass::shutdown() {
    output_.shutdown();
    available_ = false;
    ctx_ = nullptr;
    alloc_ = nullptr;
}
bool FsrPass::prepare(VkExtent2D) { return false; }
bool FsrPass::dispatch(VkCommandBuffer, rhi::RenderTarget&, TaaPass&, float, float, bool) { return false; }

#endif

}  // namespace alamo::render

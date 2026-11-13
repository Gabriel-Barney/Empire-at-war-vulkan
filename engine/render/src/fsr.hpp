// alamo_render - FSR upscaling on the GPU [port: Gabriel 2026-11-13] (internal; rules in alamo/render/upscale.hpp).
// Owned by the Renderer (created on the first FSR frame). The AMD runtime (amd_fidelityfx_loader_vk.dll, which loads
// amd_fidelityfx_upscaler_vk.dll) is loaded from the executable's folder at run time; built without the SDK headers
// (ALAMO_RENDER_HAVE_FFX unset) or without the DLLs, available() stays false and render() draws with TAA instead.
//   prepare():  the FSR context for this display size (created / recreated when the display size changes; the maximum
//               render size is the display size, so the quality mode can change without a new context);
//   dispatch(): scene colour (render size, any tracked state) + TaaPass::fsr_motion() / fsr_depth() -> output()
//               (display size, RGBA16F storage image, left in General), with the frame's jitter and camera.
// Colour: the frame graph's 8-bit back-buffer colour is display-referred (gamma), so the context is created with
// FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE and dispatched with FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB; auto exposure on.
// Depth: 0..1, not inverted, finite far plane.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "alamo/render/upscale.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo::render {

class TaaPass;

class FsrPass {
public:
    FsrPass();
    ~FsrPass();
    FsrPass(const FsrPass&) = delete;
    FsrPass& operator=(const FsrPass&) = delete;

    // Loads the runtime once; false (and status() says why) when FSR cannot run here.
    bool init(rhi::Context& ctx, rhi::Allocator& alloc);
    void shutdown();  // GPU idle
    bool available() const { return available_; }
    // "FSR 4.1.2" style name of the provider the context uses once one exists, else why FSR is off.
    const std::string& status() const { return status_; }
    // Context + output target for `display`; false when they cannot be created (FSR then stays off for the frame).
    bool prepare(VkExtent2D display);
    // Records the upscale. `reset`: camera cut / first frame (the TAA pass's history_reset()). frame_ms: time since the
    // last frame. False when the runtime rejected the dispatch (output() is then undefined for this frame).
    bool dispatch(VkCommandBuffer cmd, rhi::RenderTarget& scene, TaaPass& taa, float sharpness, float frame_ms, bool reset);
    rhi::RenderTarget& output() { return output_; }

private:
    struct Runtime;  // fsr.cpp: DLL handle, entry points, context
    void destroy_context();

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    std::unique_ptr<Runtime> rt_;
    bool available_ = false;
    std::string status_ = "FSR not initialised";
    VkExtent2D display_{0, 0};  // the context's display size
    rhi::RenderTarget output_;
};

}  // namespace alamo::render

// alamo_render - XeSS upscaling on the GPU [port: Gabriel 2026-11-13] (internal; rules in alamo/render/upscale.hpp).
// Owned by the Renderer (created on the first XeSS frame). Intel's runtime (libxess.dll, XeSS SDK 3.0.2) is loaded from
// the executable's folder at run time; built without the SDK headers (ALAMO_RENDER_HAVE_XESS unset) or without the DLL,
// available() stays false and render() draws with TAA instead.
//   init():      loads the DLL and checks that the device has what XeSS asks for (its instance / device extensions -
//                rhi::ContextDesc::upscaler_features enables VK_EXT_mutable_descriptor_type and the core features);
//   prepare():   the XeSS context for this display size and quality preset (created once, xessVKInit again when the
//                display size or preset changes) and the render size XeSS wants (xessGetOptimalInputResolution);
//   dispatch():  scene colour (render size) + TaaPass::fsr_motion() / fsr_depth() -> output() (display size, RGBA16F
//                storage image, left in General), with the frame's jitter - the same inputs as FSR.
// Colour: the frame graph's 8-bit back-buffer colour is display-referred (gamma), so XeSS is told it is LDR
// (XESS_INIT_FLAG_LDR_INPUT_COLOR, exposure 1, no auto exposure) - XeSS's documented "after tone mapping" mode.
// Motion vectors: uv units, current -> previous (xessSetVelocityScale turns them into pixels). Depth: 0..1, not inverted.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "alamo/render/upscale.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo::render {

class TaaPass;

class XessPass {
public:
    XessPass();
    ~XessPass();
    XessPass(const XessPass&) = delete;
    XessPass& operator=(const XessPass&) = delete;

    // Loads the runtime once; false (and status() says why) when XeSS cannot run here.
    bool init(rhi::Context& ctx, rhi::Allocator& alloc);
    void shutdown();  // GPU idle
    bool available() const { return available_; }
    // "XeSS 2.0.2" style name of the runtime once a context exists, else why XeSS is off.
    const std::string& status() const { return status_; }
    // Context + output target for `display` and `quality`; false when they cannot be created (XeSS then stays off for
    // the frame). render_size(): the input size XeSS wants for them (valid after a successful prepare()).
    bool prepare(VkExtent2D display, UpscaleQuality quality);
    VkExtent2D render_size() const { return render_; }
    // Records the upscale. `reset`: camera cut / first frame (the TAA pass's history_reset()). False when the runtime
    // rejected the dispatch (output() is then undefined for this frame).
    bool dispatch(VkCommandBuffer cmd, rhi::RenderTarget& scene, TaaPass& taa, bool reset);
    rhi::RenderTarget& output() { return output_; }

private:
    struct Runtime;  // xess.cpp: DLL handle, entry points, context
    void destroy_context();

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    std::unique_ptr<Runtime> rt_;
    bool available_ = false;
    std::string status_ = "XeSS not initialised";
    VkExtent2D display_{0, 0};  // the initialised display size (0: not initialised)
    UpscaleQuality quality_ = UpscaleQuality::Quality;
    VkExtent2D render_{0, 0};
    rhi::RenderTarget output_;
};

}  // namespace alamo::render

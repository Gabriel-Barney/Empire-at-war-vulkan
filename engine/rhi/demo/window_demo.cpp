// alamo_rhi_window_demo: opens a window and clears the swapchain to a cycling colour.
// Esc or closing the window quits. Options: --frames N (exit after N frames), --mailbox, --immediate.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "alamo/core/log.hpp"
#include "alamo/rhi/rhi.hpp"

using namespace alamo::rhi;

int main(int argc, char** argv) {
    long max_frames = -1;
    PresentMode mode = PresentMode::Fifo;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--mailbox")) mode = PresentMode::Mailbox;
        else if (!std::strcmp(argv[i], "--immediate")) mode = PresentMode::Immediate;
    }

    Window win;
    if (!win.create({"alamo rhi window demo (Esc quits)", 960, 540, true, true})) return 1;

    Context ctx;
    ContextDesc cd;
    cd.app_name = "alamo_rhi_window_demo";
    cd.presentation = true;
    if (!ctx.init(cd)) {
        std::printf("no suitable Vulkan 1.3 device\n");
        return 1;
    }
    Allocator alloc;
    if (!alloc.init(ctx)) return 1;
    Swapchain sc;
    SwapchainDesc sd;
    sd.hinstance = win.native_instance();
    sd.hwnd = win.native_handle();
    sd.width = win.width();
    sd.height = win.height();
    sd.mode = mode;
    if (!sc.init(ctx, sd)) return 1;
    FrameRing ring;
    FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 0;
    if (!ring.init(ctx, alloc, fd)) return 1;

    std::printf("device %s, swapchain %ux%u, %u images, present mode %d\n", ctx.caps().name.c_str(), sc.extent().width,
                sc.extent().height, sc.image_count(), static_cast<int>(sc.present_mode()));
    const auto t0 = std::chrono::steady_clock::now();
    long frames = 0;
    bool need_recreate = false;
    while (win.pump()) {
        if (win.key_down(0x1B)) break;  // VK_ESCAPE
        if (max_frames >= 0 && frames >= max_frames) break;
        if (win.take_resized()) need_recreate = true;
        if (need_recreate) {
            sc.recreate(win.width(), win.height());
            need_recreate = false;
        }
        if (!sc.valid()) {  // minimized
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        Frame* f = ring.begin_frame();
        if (!f) break;
        uint32_t image = 0;
        const SwapchainStatus as = sc.acquire(f->acquire_semaphore(), image);
        if (as == SwapchainStatus::OutOfDate || as == SwapchainStatus::Minimized || as == SwapchainStatus::Error) {
            ring.end_frame(*f);  // empty submit keeps the timeline pacing consistent
            need_recreate = true;
            if (as == SwapchainStatus::Error) break;
            continue;
        }

        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        const float r = 0.5f + 0.5f * std::sin(t * 1.3f);
        const float g = 0.5f + 0.5f * std::sin(t * 1.7f + 2.0f);
        const float b = 0.5f + 0.5f * std::sin(t * 2.3f + 4.0f);

        const DeviceFns& fn = ctx.vkd();
        VkCommandBuffer cmd = f->cmd();
        cmd_barrier(fn, cmd, transition(sc.image(image), ImageState::Undefined, ImageState::ColorAttachment));
        auto ca = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
        ca.imageView = sc.view(image);
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{r, g, b, 1.0f}};
        auto ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
        ri.renderArea = {{0, 0}, sc.extent()};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        fn.vkCmdBeginRendering(cmd, &ri);
        fn.vkCmdEndRendering(cmd);
        cmd_barrier(fn, cmd, transition(sc.image(image), ImageState::ColorAttachment, ImageState::Present));

        const VkSemaphoreSubmitInfo wait =
            semaphore_info(f->acquire_semaphore(), 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        const VkSemaphoreSubmitInfo signal =
            semaphore_info(sc.render_finished(image), 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        FrameSubmit fs;
        fs.waits = {&wait, 1};
        fs.signals = {&signal, 1};
        if (!ring.end_frame(*f, fs)) break;
        const SwapchainStatus ps = sc.present(image);
        if (ps == SwapchainStatus::OutOfDate || ps == SwapchainStatus::Suboptimal || as == SwapchainStatus::Suboptimal)
            need_recreate = true;
        else if (ps == SwapchainStatus::Error)
            break;
        ++frames;
    }
    const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%ld frames in %.2f s (%.1f fps), validation errors %u\n", frames, secs,
                secs > 0 ? static_cast<float>(frames) / secs : 0.0f, ctx.validation_errors());
    ring.shutdown();
    sc.shutdown();
    alloc.shutdown();
    const bool clean = ctx.validation_errors() == 0;
    ctx.shutdown();
    win.destroy();
    return clean ? 0 : 1;
}

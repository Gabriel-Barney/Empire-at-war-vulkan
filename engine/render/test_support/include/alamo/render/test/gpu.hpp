// Shared GPU scaffolding of the render-side test exes (CU4 t02): engine/render/tests, render/galactic/tests,
// render/particles/tests and the ui draw tests. Header-only (INTERFACE target alamo_render_test_support), so exes built
// from several TUs define nothing twice. With alamo/test/support.hpp (CHECK, env_or_empty, data roots).
#pragma once
#include <cstdint>

#include "alamo/render/offscreen.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/test/support.hpp"

namespace alamo::render::test {

// The device objects every GPU test owns. A test that also needs a renderer / HUD / GPU timer derives
// (`struct Gpu : render::test::GpuCore { render::Renderer renderer; };`): derived members are destroyed first, before
// the ring, uploader, allocator and context, as when they were declared last in one struct.
struct GpuCore {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
};

// render::Offscreen with the tests' call shape, o.create(g, w, h) / o.destroy(g) for any Gpu with an `alloc`: RGBA8
// colour, plus a D32F depth attachment when `Depth`.
template <bool Depth>
struct GpuOffscreen : render::Offscreen {
    template <class G>
    bool create(G& g, uint32_t width, uint32_t height) {
        OffscreenDesc d;
        d.depth = Depth;
        return render::Offscreen::create(g.alloc, width, height, d);
    }
    template <class G>
    void destroy(G& g) {
        render::Offscreen::destroy(g.alloc);
    }
};
using OffscreenWithDepth = GpuOffscreen<true>;
using OffscreenColorOnly = GpuOffscreen<false>;

}  // namespace alamo::render::test

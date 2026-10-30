// YUV(A) 4:2:0 -> RGBA8, BT.601, integer (16.16 fixed point) so the output is identical everywhere.
#include <algorithm>
#include <cstdint>

#include "alamo/video/video.hpp"

namespace alamo::video {
namespace {

inline uint8_t clamp8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

}  // namespace

void yuv420_to_rgba(const uint8_t* y, int y_stride, const uint8_t* u, int u_stride, const uint8_t* v, int v_stride,
                    const uint8_t* a, int a_stride, int width, int height, bool full_range, uint8_t* rgba,
                    int rgba_stride) {
    // BT.601 coefficients in 16.16 fixed point.
    //   limited: R = 1.164(Y-16) + 1.596(V-128); G = 1.164(Y-16) - 0.392(U-128) - 0.813(V-128); B = 1.164(Y-16) + 2.017(U-128)
    //   full:    R = Y + 1.402(V-128);           G = Y - 0.344(U-128) - 0.714(V-128);           B = Y + 1.772(U-128)
    const int ky = full_range ? 65536 : 76309;
    const int y0 = full_range ? 0 : 16;
    const int krv = full_range ? 91881 : 104597;
    const int kgu = full_range ? 22554 : 25675;
    const int kgv = full_range ? 46802 : 53279;
    const int kbu = full_range ? 116130 : 132201;
    for (int row = 0; row < height; ++row) {
        const uint8_t* yr = y + static_cast<std::ptrdiff_t>(row) * y_stride;
        const uint8_t* ur = u + static_cast<std::ptrdiff_t>(row / 2) * u_stride;
        const uint8_t* vr = v + static_cast<std::ptrdiff_t>(row / 2) * v_stride;
        const uint8_t* ar = a ? a + static_cast<std::ptrdiff_t>(row) * a_stride : nullptr;
        uint8_t* out = rgba + static_cast<std::ptrdiff_t>(row) * rgba_stride;
        for (int x = 0; x < width; ++x) {
            const int yy = (yr[x] - y0) * ky + 32768;
            const int cu = ur[x / 2] - 128;
            const int cv = vr[x / 2] - 128;
            out[4 * x + 0] = clamp8((yy + krv * cv) >> 16);
            out[4 * x + 1] = clamp8((yy - kgu * cu - kgv * cv) >> 16);
            out[4 * x + 2] = clamp8((yy + kbu * cu) >> 16);
            out[4 * x + 3] = ar ? ar[x] : uint8_t{255};
        }
    }
}

}  // namespace alamo::video

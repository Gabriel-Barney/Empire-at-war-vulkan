#include "alamo/tactical/movement/corridor.hpp"

#include <algorithm>

#include "alamo/tactical/movement/land_path.hpp"

namespace alamo::tactical::movement {

float passability_ray(const PassabilityGrid& grid, V2 from, V2 dir, float len, uint32_t mask) {
    if (!(len > 0.0f)) return 0.0f;
    const V2 to = from + dir * len;
    float hit = len;
    walk_cells(from, to, [&](int32_t cx, int32_t cy) {
        if (grid.passable_cell(cx, cy, mask)) return true;
        // Entry distance into the blocked cell's box (slab test); the start cell gives 0.
        const float x0 = static_cast<float>(cx) * kCellSize - kHalfCell, x1 = x0 + kCellSize;
        const float y0 = static_cast<float>(cy) * kCellSize - kHalfCell, y1 = y0 + kCellSize;
        float t_enter = 0.0f;
        if (dir.x != 0.0f) {
            const float ta = (x0 - from.x) / dir.x, tb = (x1 - from.x) / dir.x;
            t_enter = std::max(t_enter, std::min(ta, tb));
        }
        if (dir.y != 0.0f) {
            const float ta = (y0 - from.y) / dir.y, tb = (y1 - from.y) / dir.y;
            t_enter = std::max(t_enter, std::min(ta, tb));
        }
        hit = std::clamp(t_enter, 0.0f, len);
        return false;
    });
    return hit;
}

namespace {

struct PathCursor {
    std::span<const V2> p;
    std::vector<float> cum;  // cumulative arc length at each point
    explicit PathCursor(std::span<const V2> path) : p(path), cum(path.size(), 0.0f) {
        for (size_t i = 1; i < p.size(); ++i) cum[i] = cum[i - 1] + distance(p[i - 1], p[i]);
    }
    float total() const { return cum.back(); }
    // Segment (index of its start point) holding arc length a; the last segment at / past the end.
    size_t seg(float a) const {
        size_t j = 0;
        while (j + 2 < p.size() && cum[j + 1] <= a) ++j;
        return j;
    }
    V2 at(float a, size_t j) const {
        const float l = cum[j + 1] - cum[j];
        const float t = l > 0.0f ? std::clamp((a - cum[j]) / l, 0.0f, 1.0f) : 0.0f;
        return lerp(p[j], p[j + 1], t);
    }
    V2 dir(size_t j) const { return normalize_retail(p[j + 1] - p[j]); }
};

V2 rotate(V2 v, float rad) {
    float s = 0.0f, c = 0.0f;
    dmath::sincos(rad, s, c);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

}  // namespace

CorridorSqueeze squeeze_corridor(const PassabilityGrid& grid, std::span<const V2> path, const CorridorParams& p) {
    CorridorSqueeze out;
    const float W = p.width;
    if (!(W > 0.0f) || path.size() < 2 || (p.type != kCorridorProbe && p.type != kCorridorFixed)) return out;
    const float s = kCorridorSampleStep;
    const float h = W * 0.5f;
    const PathCursor pc(path);
    const float total = pc.total();
    const size_t n = static_cast<size_t>(std::floor(total / s)) + 1;
    std::vector<float> L(n, h), R(n, h);
    std::vector<size_t> segs(n, 0);
    out.samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float a = std::min(static_cast<float>(i) * s, total);
        segs[i] = pc.seg(a);
        out.samples[i] = pc.at(a, segs[i]);
    }
    if (p.type == kCorridorProbe && !p.fixed_flag) {
        auto probe = [&](float a, bool left) {
            a = std::clamp(a, 0.0f, total);
            const size_t j = pc.seg(a);
            const V2 nl = perp_left(pc.dir(j));
            return passability_ray(grid, pc.at(a, j), left ? nl : nl * -1.0f, h, p.mask);
        };
        for (size_t i = 0; i < n; ++i) {
            const float a = std::min(static_cast<float>(i) * s, total);
            for (int side = 0; side < 2; ++side) {
                const bool left = side == 0;
                float v = probe(a, left);
                if (v < h) {  // refine: cursor back by s, 4 probes s/2 apart
                    for (int k = 0; k < 4; ++k) v = std::min(v, probe(a - s + static_cast<float>(k) * (s * 0.5f), left));
                }
                (left ? L : R)[i] = v;
            }
            if (i > 0 && segs[i] != segs[i - 1]) {
                // Vertices passed since the previous sample: fan of rays around each.
                for (size_t v = segs[i - 1] + 1; v <= segs[i]; ++v) {
                    const V2 din = pc.dir(v - 1), dout = pc.dir(v);
                    const float d = std::clamp(dot(din, dout), -1.0f, 1.0f);
                    const float alpha = dmath::acos(d);
                    const float sign = cross(din, dout) < 0.0f ? -1.0f : 1.0f;
                    const auto rays = static_cast<int32_t>(alpha * h / (s * 0.25f) + 1.0f);
                    const V2 nl = perp_left(din);
                    for (int32_t k = 0; k <= rays; ++k) {
                        const float th = sign * static_cast<float>(k) * alpha / static_cast<float>(rays);
                        L[i] = std::min(L[i], passability_ray(grid, path[v], rotate(nl, th), h, p.mask));
                        R[i] = std::min(R[i], passability_ray(grid, path[v], rotate(nl * -1.0f, th), h, p.mask));
                    }
                }
                L[i] = std::min(L[i], L[i - 1]);
                R[i] = std::min(R[i], R[i - 1]);
            }
        }
    }
    // Look-ahead window (in place, front to back: entries ahead of i are still raw).
    const auto K = static_cast<size_t>(std::max<int32_t>(1, static_cast<int32_t>(W / (2.0f * s) + 2.0f) - 1));
    for (size_t i = 0; i < n; ++i) {
        const size_t e = std::min(i + K, n);
        for (size_t j = i + 1; j < e; ++j) {
            L[i] = std::min(L[i], L[j]);
            R[i] = std::min(R[i], R[j]);
        }
    }
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i) w[i] = L[i] + R[i];
    // Smoothing, front to back.
    const auto max_gap = static_cast<int32_t>(2.5f * p.depth_cells);
    for (size_t i = 0; i < n; ++i) {
        if (!(w[i] < W)) continue;
        const float wi = w[i];
        for (size_t j = i; j-- > 0;) {  // back-fill
            if (i - j > 2 && w[j] >= W) break;
            w[j] = wi;
        }
        size_t last = i;  // forward fill
        int32_t gap = 0;
        for (size_t k = i + 1; k < n && w[k] >= wi; ++k) {
            if (w[k] < W) {
                for (size_t m = last + 1; m <= k; ++m) w[m] = wi;
                last = k;
                gap = 0;
            } else if (++gap >= max_gap) {
                break;
            }
        }
    }
    // Outputs.
    out.table.push_back(W / kCellSize);
    out.entry.resize(n);
    out.shift.resize(n);
    for (size_t i = 0; i < n; ++i) {
        if (w[i] >= W) {
            out.entry[i] = 0;
        } else {
            const float cells = w[i] / kCellSize;
            if (out.table.back() != cells) out.table.push_back(cells);
            out.entry[i] = static_cast<uint32_t>(out.table.size() - 1);
        }
        const float hi = std::min(w[i], W) * 0.5f;
        float sh = 0.0f;
        if (L[i] < hi || R[i] < hi) sh = R[i] <= L[i] ? hi - R[i] : L[i] - hi;
        out.shift[i] = sh;
    }
    out.left = std::move(L);
    out.right = std::move(R);
    out.width = std::move(w);
    return out;
}

}  // namespace alamo::tactical::movement

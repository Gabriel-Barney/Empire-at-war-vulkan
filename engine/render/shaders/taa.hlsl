// alamo_render temporal anti-aliasing [port] (the original has none; render/src/taa.cpp, alamo/render/taa.hpp).
// One full-screen triangle (no vertex buffer), pixel coordinates from SV_Position.
// RESOLVE: scene colour (jittered) + scene depth + history + previous linear depth (+ velocity) -> new history (RGBA16F):
//   reprojection of the 3x3 nearest depth through Reproj (current jittered NDC -> previous un-jittered clip; camera
//   motion), or - where the nearest-depth pixel belongs to an instance that moved (taa_velocity.hlsl) - the velocity
//   target's previous position of that surface point; 5-tap Catmull-Rom history fetch, then three decisions about
//   how far to trust it:
//     * disocclusion: the depth the reprojection expects vs the depth stored at that spot last frame (closest of the
//       2x2 footprint, so slopes and silhouettes do not trip it). Things that moved without a vector (no motion id,
//       animation) leave exactly this signature, so their history is dropped instead of smeared;
//     * clip: the history is clipped to the current 3x3 neighbourhood (YCoCg variance box intersected with min / max);
//       the box is wider where the history sat still (more accumulation) and tighter where it moved;
//     * blend: Params.x of the current frame, raised with how far the history had to be clipped and to 1 on
//       disocclusion (luma-weighted against flicker).
// DEPTH: scene depth -> linear view distance (R32F), kept for the next frame's disocclusion test.
// SHARPEN: history -> scene colour, unsharp mask of the 4 neighbours by Params.z, clamped to their min / max.

struct TaaPush {
    float4 Reproj[4];   // rows (column-vector convention): float4(ndc.xy, depth, 1) -> previous clip
    float4 Texel;       // 1 / width, 1 / height, width, height
    float4 Params;      // x current weight, y reset (> 0.5: current only), z sharpen amount, w 1 = velocity target valid
    float4 DepthCur;    // x proj m[2][2], y proj m[2][3], z 1 = perspective / 0 = orthographic (this frame)
    float4 DepthPrev;   // the same for the previous frame's camera
};
[[vk::push_constant]] TaaPush PC;

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

// View distance of a 0..1 NDC depth (CPU twin: taa_linear_depth in taa.cpp).
float LinearDepth(float4 dp, float ndc) {
    return dp.z > 0.5 ? dp.y / (ndc + dp.x) : (dp.y - ndc) / dp.x;
}

#ifdef RESOLVE
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> CurTex;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState CurSmp;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float> DepthTex;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState DepthSmp;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] Texture2D<float4> HistTex;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] SamplerState HistSmp;
[[vk::combinedImageSampler]][[vk::binding(3, 0)]] Texture2D<float> PrevDepthTex;
[[vk::combinedImageSampler]][[vk::binding(3, 0)]] SamplerState PrevDepthSmp;
[[vk::combinedImageSampler]][[vk::binding(4, 0)]] Texture2D<float4> VelTex;  // previous uv, previous NDC depth, 1 = written
[[vk::combinedImageSampler]][[vk::binding(4, 0)]] SamplerState VelSmp;

// Relative depth error where the history starts to be dropped / is dropped completely.
static const float kDisoccLo = 0.03;
static const float kDisoccHi = 0.10;
// Variance-box size in sigmas: still history / history that moved by a pixel or more.
static const float kGammaStill = 1.25;
static const float kGammaMoving = 1.0;
// Current-frame weight the blend rises to when the history had to be clipped hard.
static const float kClipWeight = 0.4;

float3 ToYCoCg(float3 c) {
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}
float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Clip q towards the box centre onto the box (Playdead's clip_aabb; keeps the history's hue better than a clamp).
float3 ClipBox(float3 mn, float3 mx, float3 q) {
    const float3 c = 0.5 * (mx + mn);
    const float3 e = 0.5 * (mx - mn) + 1e-5;
    const float3 v = q - c;
    const float3 u = abs(v / e);
    const float m = max(u.x, max(u.y, u.z));
    return m > 1.0 ? c + v / m : q;
}

// Bicubic Catmull-Rom from 5 bilinear taps (corners dropped): less blur than one bilinear fetch when the camera moves.
float4 HistoryCatmullRom(float2 uv) {
    const float2 sp = uv * PC.Texel.zw;
    const float2 t1 = floor(sp - 0.5) + 0.5;
    const float2 f = sp - t1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 p0 = (t1 - 1.0) * PC.Texel.xy;
    const float2 p3 = (t1 + 2.0) * PC.Texel.xy;
    const float2 p12 = (t1 + w2 / w12) * PC.Texel.xy;
    float4 r = HistTex.SampleLevel(HistSmp, float2(p12.x, p0.y), 0) * (w12.x * w0.y);
    r += HistTex.SampleLevel(HistSmp, float2(p0.x, p12.y), 0) * (w0.x * w12.y);
    r += HistTex.SampleLevel(HistSmp, p12, 0) * (w12.x * w12.y);
    r += HistTex.SampleLevel(HistSmp, float2(p3.x, p12.y), 0) * (w3.x * w12.y);
    r += HistTex.SampleLevel(HistSmp, float2(p12.x, p3.y), 0) * (w12.x * w3.y);
    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, 0.0);
}

// 0 = the history's depth agrees with where the reprojection says it should be, 1 = it does not (disoccluded).
// `expected` is the previous-frame NDC depth of the reprojected point, `puv` its previous-frame position.
float Disocclusion(float expected, float2 puv, int2 last) {
    const float dExp = LinearDepth(PC.DepthPrev, expected);
    const float2 sp = puv * PC.Texel.zw - 0.5;
    const int2 b = int2(floor(sp));
    float best = 1e9;
    [unroll] for (int i = 0; i < 4; ++i) {
        const int2 q = clamp(b + int2(i & 1, i >> 1), int2(0, 0), last);
        best = min(best, abs(PrevDepthTex.Load(int3(q, 0)) - dExp));
    }
    const float rel = best / max(abs(dExp), 1e-3);
    return saturate((rel - kDisoccLo) / (kDisoccHi - kDisoccLo));
}

float4 PSResolve(float4 pos : SV_Position) : SV_Target0 {
    const int2 p = int2(pos.xy);
    const int2 last = int2(PC.Texel.zw) - 1;
    const float4 cur = CurTex.Load(int3(p, 0));
    if (PC.Params.y > 0.5) return cur;
    float3 m1 = 0.0, m2 = 0.0, mn = 1e9, mx = -1e9;
    float nearest = 1.0;
    int2 nearest_px = p;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            const int2 q = clamp(p + int2(dx, dy), int2(0, 0), last);
            const float3 c = ToYCoCg(CurTex.Load(int3(q, 0)).rgb);
            m1 += c;
            m2 += c * c;
            mn = min(mn, c);
            mx = max(mx, c);
            const float d = DepthTex.Load(int3(q, 0));
            if (d < nearest) {
                nearest = d;
                nearest_px = q;
            }
        }
    }
    // Pixel centre with the nearest depth around it (moving silhouettes reproject with the foreground's motion).
    const float2 uv = (float2(p) + 0.5) * PC.Texel.xy;
    const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, nearest, 1.0);  // render(): viewport Y flipped
    // An instance that moved: where its surface point stood last frame (own motion + camera), and its depth there.
    const float4 vel = PC.Params.w > 0.5 ? VelTex.Load(int3(nearest_px, 0)) : float4(0.0, 0.0, 0.0, 0.0);
    float2 puv;
    float expected;  // previous-frame NDC depth of the reprojected point
    if (vel.w > 0.5) {
        puv = vel.xy;
        expected = vel.z;
    } else {
        const float4 clip = float4(dot(PC.Reproj[0], ndc), dot(PC.Reproj[1], ndc), dot(PC.Reproj[2], ndc), dot(PC.Reproj[3], ndc));
        if (clip.w <= 1e-6) return cur;
        const float2 pn = clip.xy / clip.w;
        puv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
        expected = clip.z / clip.w;
    }
    if (any(puv < 0.0) || any(puv > 1.0)) return cur;  // came from off screen: nothing to accumulate
    // Sky / far plane carries no usable depth: nothing to test there.
    const float disocc = nearest < 0.99999 ? Disocclusion(expected, puv, last) : 0.0;
    if (disocc >= 1.0) return cur;
    float4 hist = HistoryCatmullRom(puv);
    const float moved = length((puv - uv) * PC.Texel.zw);  // pixels the history shifted by
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float gamma = lerp(kGammaStill, kGammaMoving, saturate(moved));
    const float3 bmin = max(mean - gamma * sigma, mn), bmax = min(mean + gamma * sigma, mx);
    const float3 hy = ToYCoCg(hist.rgb);
    const float3 cy = ClipBox(bmin, bmax, hy);
    hist.rgb = FromYCoCg(cy);
    // How far the history was pulled in, against the box size: a trail of something that moved is far outside it.
    const float clipped = saturate(length(hy - cy) / (length(bmax - bmin) + 1e-3));
    float a = PC.Params.x;
    a = lerp(a, max(a, kClipWeight), saturate(clipped * 4.0));
    a = lerp(a, 1.0, disocc);
    // Luma-weighted blend (bright sub-pixel sparks do not dominate the average).
    const float wc = a / (1.0 + ToYCoCg(cur.rgb).x), wh = (1.0 - a) / (1.0 + ToYCoCg(hist.rgb).x);
    float4 r;
    r.rgb = (cur.rgb * wc + hist.rgb * wh) / (wc + wh);
    r.a = lerp(hist.a, cur.a, a);
    return r;
}
#endif

#ifdef DEPTH
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float> SceneDepthTex;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState SceneDepthSmp;

float PSDepth(float4 pos : SV_Position) : SV_Target0 {
    return LinearDepth(PC.DepthCur, SceneDepthTex.Load(int3(int2(pos.xy), 0)));
}
#endif

#ifdef SHARPEN
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> SrcTex;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState SrcSmp;

float4 PSSharpen(float4 pos : SV_Position) : SV_Target0 {
    const int2 p = int2(pos.xy);
    const int2 last = int2(PC.Texel.zw) - 1;
    const float4 c = SrcTex.Load(int3(p, 0));
    if (PC.Params.z <= 0.0) return c;
    const float3 n = SrcTex.Load(int3(clamp(p + int2(0, -1), int2(0, 0), last), 0)).rgb;
    const float3 s = SrcTex.Load(int3(clamp(p + int2(0, 1), int2(0, 0), last), 0)).rgb;
    const float3 e = SrcTex.Load(int3(clamp(p + int2(1, 0), int2(0, 0), last), 0)).rgb;
    const float3 w = SrcTex.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), last), 0)).rgb;
    const float3 mn = min(c.rgb, min(min(n, s), min(e, w))), mx = max(c.rgb, max(max(n, s), max(e, w)));
    const float3 sharp = c.rgb + PC.Params.z * (4.0 * c.rgb - n - s - e - w);
    return float4(clamp(sharp, mn, mx), c.a);
}
#endif

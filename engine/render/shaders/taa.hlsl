// alamo_render temporal anti-aliasing [port] (the original has none; render/src/taa.cpp, alamo/render/taa.hpp).
// One full-screen triangle (no vertex buffer), pixel coordinates from SV_Position.
// RESOLVE: scene colour (jittered) + scene depth + history -> new history (RGBA16F):
//   reprojection of the 3x3 nearest depth through Reproj (current jittered NDC -> previous un-jittered clip; camera
//   motion only), 5-tap Catmull-Rom history fetch, clip to the current 3x3 neighbourhood (YCoCg variance box
//   intersected with min / max), blend Params.x of the current frame (luma-weighted against flicker).
// SHARPEN: history -> scene colour, unsharp mask of the 4 neighbours by Params.z, clamped to their min / max.

struct TaaPush {
    float4 Reproj[4];  // rows (column-vector convention): float4(ndc.xy, depth, 1) -> previous clip
    float4 Texel;      // 1 / width, 1 / height, width, height
    float4 Params;     // x current weight, y reset (> 0.5: current only), z sharpen amount, w unused
};
[[vk::push_constant]] TaaPush PC;

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

#ifdef RESOLVE
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> CurTex;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState CurSmp;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float> DepthTex;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState DepthSmp;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] Texture2D<float4> HistTex;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] SamplerState HistSmp;

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

float4 PSResolve(float4 pos : SV_Position) : SV_Target0 {
    const int2 p = int2(pos.xy);
    const int2 last = int2(PC.Texel.zw) - 1;
    const float4 cur = CurTex.Load(int3(p, 0));
    if (PC.Params.y > 0.5) return cur;
    float3 m1 = 0.0, m2 = 0.0, mn = 1e9, mx = -1e9;
    float nearest = 1.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            const int2 q = clamp(p + int2(dx, dy), int2(0, 0), last);
            const float3 c = ToYCoCg(CurTex.Load(int3(q, 0)).rgb);
            m1 += c;
            m2 += c * c;
            mn = min(mn, c);
            mx = max(mx, c);
            nearest = min(nearest, DepthTex.Load(int3(q, 0)));
        }
    }
    // Pixel centre with the nearest depth around it (moving silhouettes reproject with the foreground's motion).
    const float2 uv = (float2(p) + 0.5) * PC.Texel.xy;
    const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, nearest, 1.0);  // render(): viewport Y flipped
    const float4 clip = float4(dot(PC.Reproj[0], ndc), dot(PC.Reproj[1], ndc), dot(PC.Reproj[2], ndc), dot(PC.Reproj[3], ndc));
    if (clip.w <= 1e-6) return cur;
    const float2 pn = clip.xy / clip.w;
    const float2 puv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
    if (any(puv < 0.0) || any(puv > 1.0)) return cur;  // came from off screen: nothing to accumulate
    float4 hist = HistoryCatmullRom(puv);
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 bmin = max(mean - sigma, mn), bmax = min(mean + sigma, mx);
    hist.rgb = FromYCoCg(ClipBox(bmin, bmax, ToYCoCg(hist.rgb)));
    // Luma-weighted blend (bright sub-pixel sparks do not dominate the average).
    const float a = PC.Params.x;
    const float wc = a / (1.0 + ToYCoCg(cur.rgb).x), wh = (1.0 - a) / (1.0 + ToYCoCg(hist.rgb).x);
    float4 r;
    r.rgb = (cur.rgb * wc + hist.rgb * wh) / (wc + wh);
    r.a = lerp(hist.a, cur.a, a);
    return r;
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

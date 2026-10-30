// [port] Screen-space ambient occlusion (render/src/ssao.cpp). The original game has no AO; this is a port
// option, off by default (RecordOptions::ambient_occlusion). Depth only (no normals buffer: the materials are
// the translated original shaders): view-space positions and face normals are rebuilt from the scene depth.
//   PSAo    : half-res AO from the full-res scene depth. A 12-tap spiral in a view-space disc of Radius world
//             units around the pixel; each tap occludes by the cosine between the tap direction and the
//             rebuilt normal (minus a bias) with a quadratic distance falloff (taps beyond Radius count 0, so a
//             foreground object leaves no halo on the far background). Out: (visibility, view distance; 0 = sky).
//   PSBlur  : separable 9-tap Gaussian, weighted by view-distance similarity (edges stay sharp).
//   PSApply : depth-aware bilinear upsample to full res; 1 - Strength x (1 - visibility), written to the scene
//             colour with a ZERO / SRC_COLOR blend (RGB only).
// Pixel coordinates come from SV_Position: row 0 = top of the screen = NDC y +1 (the scene's Y-flipped viewport).

struct AoPush {
    float4 Proj0;   // p00, p11, p02, p12 of the projection (column-vector rows)
    float4 Proj1;   // p03, p13, p22, p23
    float4 Proj2;   // p32, p33, depth of empty pixels (the clear value), unused
    float4 Params;  // Radius (world units), Intensity, Bias (cosine), Strength
    float4 Size;    // depth texture width, height; AO target width, height
    float4 Blur;    // blur direction x, y (AO texels); depth sharpness; max radius (depth-texture pixels)
};
[[vk::push_constant]] AoPush PC;

[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float> DepthTex;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState DepthSmp;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float4> AoTex;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState AoSmp;

static const uint kTaps = 12;
static const float kTurns = 7.0;  // spiral turns over the taps (coprime with kTaps)
static const float kTwoPi = 6.28318530718;

struct VSOut {
    float4 pos : SV_Position;
};

// One triangle covering the target.
VSOut VSFull(uint id : SV_VertexID) {
    VSOut o;
    const float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// View-space position of screen uv (0,0 top-left) at depth d: z from the projection's z / w rows (normal or
// reversed depth, perspective or orthographic), x / y from its x / y rows (sub-pixel offsets in p02 / p12 kept).
float3 ViewPos(float2 uv, float d) {
    const float z = (PC.Proj1.w - d * PC.Proj2.y) / (d * PC.Proj2.x - PC.Proj1.z);
    const float w = PC.Proj2.x * z + PC.Proj2.y;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float x = (ndc.x * w - PC.Proj0.z * z - PC.Proj1.x) / PC.Proj0.x;
    const float y = (ndc.y * w - PC.Proj0.w * z - PC.Proj1.y) / PC.Proj0.y;
    return float3(x, y, z);
}

float DepthAt(float2 uv) { return DepthTex.SampleLevel(DepthSmp, uv, 0.0); }
bool IsEmpty(float d) { return d == PC.Proj2.z; }

// Interleaved gradient noise (Jimenez 2014): per-pixel spiral rotation, removed by the blur.
float Noise(float2 pix) { return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715)))); }

float4 PSAo(VSOut i) : SV_Target0 {
    const float2 uv = i.pos.xy / PC.Size.zw;
    const float2 texel = 1.0 / PC.Size.xy;  // one depth-texture pixel in uv
    // Snap to the depth texel under this AO pixel so the centre and its neighbours are exact samples.
    const float2 cuv = (floor(uv * PC.Size.xy) + 0.5) * texel;
    const float d = DepthAt(cuv);
    if (IsEmpty(d)) return float4(1.0, 0.0, 0.0, 0.0);
    const float3 P = ViewPos(cuv, d);
    const float dist = -P.z;
    // Face normal from the nearer neighbour on each axis (no smearing across silhouettes).
    const float3 pr = ViewPos(cuv + float2(texel.x, 0.0), DepthAt(cuv + float2(texel.x, 0.0)));
    const float3 pl = ViewPos(cuv - float2(texel.x, 0.0), DepthAt(cuv - float2(texel.x, 0.0)));
    const float3 pd = ViewPos(cuv + float2(0.0, texel.y), DepthAt(cuv + float2(0.0, texel.y)));
    const float3 pu = ViewPos(cuv - float2(0.0, texel.y), DepthAt(cuv - float2(0.0, texel.y)));
    const float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    const float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, P) > 0.0) N = -N;  // towards the camera
    // Disc radius in uv, clamped to Blur.w depth pixels (a close-up keeps a bounded cost and look).
    float R = PC.Params.x;
    float2 ruv = R * 0.5 * float2(PC.Proj0.x, PC.Proj0.y) / dist;
    const float rpx = ruv.y * PC.Size.y;
    if (rpx < 1.0) return float4(1.0, dist, 0.0, 0.0);
    if (rpx > PC.Blur.w) {
        const float s = PC.Blur.w / rpx;
        ruv *= s;
        R *= s;
    }
    const float r2 = R * R;
    const float rot = Noise(i.pos.xy) * kTwoPi;
    float sum = 0.0;
    [unroll] for (uint k = 0; k < kTaps; ++k) {
        const float a = (float(k) + 0.5) / float(kTaps);
        const float ang = a * kTurns * kTwoPi + rot;
        // Clamped on screen: a tap past the edge would read the edge depth at the wrong position (false occlusion).
        const float2 suv = clamp(cuv + float2(cos(ang), sin(ang)) * ruv * a, 0.5 * texel, 1.0 - 0.5 * texel);
        const float sd = DepthAt(suv);
        if (IsEmpty(sd)) continue;
        const float3 v = ViewPos(suv, sd) - P;
        const float vv = dot(v, v);
        const float falloff = max(1.0 - vv / r2, 0.0);
        sum += falloff * max(dot(v, N) * rsqrt(vv + 1e-4 * r2) - PC.Params.z, 0.0);
    }
    const float vis = saturate(1.0 - PC.Params.y * sum / float(kTaps));
    return float4(vis, dist, 0.0, 0.0);
}

float4 PSBlur(VSOut i) : SV_Target0 {
    const float2 inv = 1.0 / PC.Size.zw;
    const float2 uv = i.pos.xy * inv;
    const float2 c = AoTex.SampleLevel(AoSmp, uv, 0.0).xy;
    if (c.y <= 0.0) return float4(1.0, 0.0, 0.0, 0.0);
    static const float kW[5] = {0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};
    float s = c.x * kW[0], wsum = kW[0];
    const float sharp = PC.Blur.z / c.y;
    [unroll] for (int k = 1; k <= 4; ++k) {
        [unroll] for (int side = -1; side <= 1; side += 2) {
            const float2 t = AoTex.SampleLevel(AoSmp, uv + PC.Blur.xy * inv * float(k * side), 0.0).xy;
            const float w = t.y > 0.0 ? kW[k] * max(1.0 - abs(t.y - c.y) * sharp, 0.0) : 0.0;
            s += t.x * w;
            wsum += w;
        }
    }
    return float4(s / wsum, c.y, 0.0, 0.0);
}

float4 PSApply(VSOut i) : SV_Target0 {
    const float2 uv = i.pos.xy / PC.Size.xy;
    const float d = DepthAt(uv);
    if (IsEmpty(d)) return float4(1.0, 1.0, 1.0, 1.0);
    const float dist = -ViewPos(uv, d).z;
    // The four AO texels around this pixel: bilinear weights x view-distance similarity.
    const float2 hp = uv * PC.Size.zw - 0.5;
    const float2 base = floor(hp);
    const float2 f = hp - base;
    const float2 inv = 1.0 / PC.Size.zw;
    float s = 0.0, wsum = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
        const float2 o = float2(k & 1, k >> 1);
        const float2 t = AoTex.SampleLevel(AoSmp, (base + o + 0.5) * inv, 0.0).xy;
        const float bw = (o.x > 0.0 ? f.x : 1.0 - f.x) * (o.y > 0.0 ? f.y : 1.0 - f.y);
        const float w = t.y > 0.0 ? (bw + 1e-3) / (1e-3 + abs(t.y - dist) * PC.Blur.z / dist) : 0.0;
        s += t.x * w;
        wsum += w;
    }
    const float vis = wsum > 0.0 ? s / wsum : 1.0;
    const float m = 1.0 - PC.Params.w * (1.0 - vis);
    return float4(m, m, m, 1.0);
}

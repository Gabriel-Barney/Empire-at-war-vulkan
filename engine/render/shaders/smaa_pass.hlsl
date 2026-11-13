// [port] SMAA 1x (render/src/smaa.cpp, shaders/smaa_pass.hlsl): Subpixel Morphological Anti-Aliasing, the reference implementation by Jimenez,
// Echevarria, Masia, Navarro and Gutierrez (third_party/smaa/SMAA.hlsl, MIT licence) with one fixed quality level
// (SMAA_PRESET_HIGH: threshold 0.1, 16 search steps, diagonal and corner detection on). Three full-screen passes:
//   PSEdge    : luma edge detection of the finished frame        -> edges   (RG8)
//   PSWeights : blending weights (search + area lookup textures) -> weights (RGBA8)
//   PSBlend   : neighbourhood blending of the frame copy         -> the scene colour
// The pixel position comes from SV_Position (row 0 = top row of the image) so the passes do not depend on the viewport
// flip the scene pass uses; the reference vertex-shader offsets are computed in the pixel shaders from it.
struct SmaaPush {
    float4 Metrics;  // 1 / width, 1 / height, width, height
};
[[vk::push_constant]] SmaaPush PC;

#define SMAA_RT_METRICS PC.Metrics
// The reference's HLSL 4.1 porting layer (SMAA.hlsl, "Porting Functions") with the two samplers bound as Vulkan samplers
// instead of its effect-syntax state blocks.
#define SMAA_CUSTOM_SL
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_PRESET_HIGH

[[vk::binding(0, 0)]] Texture2D Tex0;
[[vk::binding(1, 0)]] Texture2D Tex1;
[[vk::binding(2, 0)]] Texture2D Tex2;
[[vk::binding(3, 0)]] SamplerState LinearSampler;
[[vk::binding(4, 0)]] SamplerState PointSampler;

#include "SMAA.hlsl"

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

float2 Uv(float4 pos) { return pos.xy * SMAA_RT_METRICS.xy; }

// Tex0 = the frame.
float2 PSEdge(float4 pos : SV_Position) : SV_Target0 {
    const float2 uv = Uv(pos);
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
    return SMAALumaEdgeDetectionPS(uv, offset, Tex0);
}

// Tex0 = edges, Tex1 = area lookup, Tex2 = search lookup.
float4 PSWeights(float4 pos : SV_Position) : SV_Target0 {
    const float2 uv = Uv(pos);
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(uv, pixcoord, offset, Tex0, Tex1, Tex2, float4(0.0, 0.0, 0.0, 0.0));
}

// Tex0 = the frame copy, Tex1 = weights.
float4 PSBlend(float4 pos : SV_Position) : SV_Target0 {
    const float2 uv = Uv(pos);
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    return SMAANeighborhoodBlendingPS(uv, offset, Tex0, Tex1);
}

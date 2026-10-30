// Volume texture sampling test: uv from the full-screen triangle, w = pc.color.x, explicit LOD.
struct PushData {
    float4 color;  // x = w coordinate
    float lod;
};
[[vk::push_constant]] PushData pc;
[[vk::combinedImageSampler]][[vk::binding(0, 1)]] Texture3D vol0;
[[vk::combinedImageSampler]][[vk::binding(0, 1)]] SamplerState smp0;

struct FsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 vol_ps(FsOut i) : SV_Target0 { return vol0.SampleLevel(smp0, float3(i.uv, pc.color.x), pc.lod); }

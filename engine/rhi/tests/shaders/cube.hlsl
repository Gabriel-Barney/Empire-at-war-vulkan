// Cube-map sampling test: direction from the push block.
struct PushData {
    float4 color;  // xyz = direction
    float lod;
};
[[vk::push_constant]] PushData pc;
[[vk::combinedImageSampler]][[vk::binding(0, 1)]] TextureCube cube0;
[[vk::combinedImageSampler]][[vk::binding(0, 1)]] SamplerState smp0;

float4 cube_ps(float4 pos : SV_Position) : SV_Target0 { return cube0.SampleLevel(smp0, pc.color.xyz, pc.lod); }

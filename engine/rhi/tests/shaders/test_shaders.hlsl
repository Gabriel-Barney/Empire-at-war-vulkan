// Test shaders for alamo_rhi_tests (compiled to SPIR-V headers by DXC at build time).
// Interface follows docs/shaders/PORTING.md section 1: set 1 = combined image samplers, push constants.

struct PushData {
    float4 color;
    float lod;
};
[[vk::push_constant]] PushData pc;

// --- Hard-coded triangle: no vertex buffers. Vulkan clip space has +Y down.
float4 tri_vs(uint id : SV_VertexID) : SV_Position {
    const float2 p[3] = {float2(0.0, -0.5), float2(0.5, 0.5), float2(-0.5, 0.5)};
    return float4(p[id], 0.0, 1.0);
}

float4 tri_ps() : SV_Target0 { return pc.color; }

// --- Full-screen triangle sampling s0 at an explicit LOD.
struct FsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

FsOut fs_vs(uint id : SV_VertexID) {
    FsOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[[vk::combinedImageSampler]][[vk::binding(0, 1)]] Texture2D tex0;
[[vk::combinedImageSampler]][[vk::binding(0, 1)]] SamplerState smp0;

float4 fs_ps(FsOut i) : SV_Target0 { return tex0.SampleLevel(smp0, i.uv, pc.lod); }

// --- discard (DXC/vulkan1.3 lowers it to OpDemoteToHelperInvocation): left half is discarded.
float4 discard_ps(FsOut i) : SV_Target0 {
    if (i.uv.x < 0.5) discard;
    return pc.color;
}

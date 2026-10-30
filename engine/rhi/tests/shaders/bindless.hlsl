// Hot-set (track A) interface test: mirrors engine/materials/shaders/common.hlsli set 1 + push block.
struct PushData {
    uint DrawIndex;
    uint AlphaFunc;
    uint AlphaRef;
    uint Flags;
};
[[vk::push_constant]] PushData PC;
[[vk::binding(0, 1)]] Texture2D Textures[];
[[vk::binding(1, 1)]] SamplerState Samplers[4];

struct FsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 bl_ps(FsOut i) : SV_Target0 {
    const uint idx = PC.DrawIndex;
    return Textures[NonUniformResourceIndex(idx)].SampleLevel(Samplers[NonUniformResourceIndex(PC.Flags & 3u)], i.uv, 0.0);
}

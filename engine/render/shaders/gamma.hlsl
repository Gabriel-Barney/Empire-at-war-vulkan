// alamo_render output gamma [port] (the original sets the device gamma ramp, TERRAIN_WATER_POST.md 4.6: Options > Gamma;
// render/src/gamma.cpp). One full-screen triangle (no vertex buffer): copy of the finished frame -> frame, each colour
// channel raised to Params.x (= 1 / gamma, > 1 darkens, < 1 brightens), alpha unchanged.
struct GammaPush {
    float4 Texel;   // 1 / width, 1 / height, width, height
    float4 Params;  // x exponent (1 / gamma)
};
[[vk::push_constant]] GammaPush PC;

[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> SrcTex;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState SrcSmp;

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    const float4 c = SrcTex.SampleLevel(SrcSmp, pos.xy * PC.Texel.xy, 0);
    return float4(pow(saturate(c.rgb), PC.Params.x), c.a);
}

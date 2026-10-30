// alamo_render unlit colour pass (Phase 3 C2b): diffuse texture x vertex colour, depth tested.
// Uses the real hot-set interface (engine/materials/shaders/common.hlsli): FrameCB, Draws,
// Materials, Bones, bindless Textures[] + Samplers[], 16-byte push constants.
// Defines: SKINNED (rigid palette from NORMAL.w, Bones[DrawData.BoneBase + index]).
// The PS ends with the shared Epilogue(): the material's alpha test (PC.AlphaFunc / AlphaRef, Always = off)
// and scene fog (PC.Flags bit 0, vertex fog factor from FOG_VALS like the lit passes).
#include "common.hlsli"

struct VSIn {
    [[vk::location(0)]] float3 pos : POSITION;
#ifdef SKINNED
    [[vk::location(1)]] float4 nrm : NORMAL;
#endif
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(5)]] float4 col : COLOR0;
    uint inst : SV_InstanceID;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    nointerpolation uint mat : TEXCOORD1;
    float fog : TEXCOORD2;
};

VSOut VSMain(VSIn i) {
    DrawData d = Draws[PC.DrawIndex + i.inst];
    float4 r0, r1, r2;
#ifdef SKINNED
    WorldRows(d, i.nrm.w, r0, r1, r2);
#else
    WorldRows(d, 0.0, r0, r1, r2);
#endif
    float3 wp = XformPoint(r0, r1, r2, i.pos);
    VSOut o;
    o.pos = Mul4(ViewProj, float4(wp, 1.0));
    o.uv = i.uv;
    o.col = i.col * d.LightScale;
    o.mat = d.MaterialIndex;
    o.fog = FogFactor(o.pos);
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    MeshMaterial m = Materials[i.mat];
    float4 c = SampleTex(m.Tex.x, i.uv) * i.col;
    return Epilogue(c, i.fog);
}

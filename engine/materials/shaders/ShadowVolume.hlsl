// Stencil shadow volumes: MeshShadowVolume (Patch2, vs_1_1 vs_main_bin + preshader) and
// RSkinShadowVolume (SKINNED). Vertices whose normal faces away from the light are pushed away
// from it by SHADOW_EXTRUSION_DISTANCE (FrameCB.ShadowParams.x); the meshes carry pre-built
// degenerate fin quads. Colour writes are off in the shipped pass (COLORWRITEENABLE = 0); the
// debug colour (MeshMaterial.Diffuse = DebugColor) is kept for debug views.
#include "common.hlsli"

struct VSIn {
    [[vk::location(0)]] float3 Pos : POSITION;
    [[vk::location(1)]] float4 Normal : NORMAL;   // .w = bone index for SKINNED
};

struct VSOut {
    float4 Pos : SV_Position;
    float4 Color0 : COLOR0;
};

VSOut VSMain(VSIn v, uint iid : SV_InstanceID) {
    VSOut o;
    DrawData d = Draws[PC.DrawIndex + iid];
    MeshMaterial m = Materials[d.MaterialIndex];
    float4 r0, r1, r2;
    WorldRows(d, v.Normal.w, r0, r1, r2);
    float3 wp = XformPoint(r0, r1, r2, v.Pos);
    float3 wn = XformVector(r0, r1, r2, v.Normal.xyz);
    float3 L = Light0Vector.xyz;
    float back = dot(wn, L) < 0.0 ? 1.0 : 0.0;
    wp -= back * L * ShadowParams.x;
#ifdef SKINNED
    const float darken = 0.8;
#else
    const float darken = 0.7;
#endif
    o.Color0 = m.Diffuse - back * darken * m.Diffuse;
    o.Pos = Mul4(ViewProj, float4(wp, 1.0));
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 { return i.Color0; }

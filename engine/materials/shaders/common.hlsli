// Shared interface of the hand-written (track A) and fixed-function SM6 shaders.
// C++ twin: engine/materials/include/alamo/materials/constants.hpp (static_asserted layouts).
// Compiled with: dxc -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -WX.
//
// Matrices are stored as "register rows": out[i] = dot(R[i], v) (the D3D9 register image of the
// original column_major constants). Only float4/uint4 members are used, so std140/std430/dx
// layouts coincide.
//
// Descriptor interface:
//   set 0 binding 0  FrameCB (uniform buffer)
//   set 0 binding 1  StructuredBuffer<DrawData>      index = PC.DrawIndex + SV_InstanceID
//   set 0 binding 2  StructuredBuffer<MeshMaterial>  index = DrawData.MaterialIndex
//   set 0 binding 3  StructuredBuffer<BoneMatrix>    index = DrawData.BoneBase + (uint)NORMAL.w
//   set 0 binding 4  StructuredBuffer<FfDrawData>    index = DrawData.MaterialIndex (FF only)
//   set 1 binding 0  Texture2D Textures[]            (bindless, runtime array, non-uniform index)
//   set 1 binding 1  SamplerState Samplers[4]        (engine quality samplers, EngineSampler)
//   push constant    { DrawIndex, AlphaFunc, AlphaRef, Flags }  16 bytes, VS+PS
// SV_InstanceID maps to InstanceIndex (includes firstInstance), so firstInstance may carry the
// draw index with PC.DrawIndex = 0.
#ifndef ALAMO_COMMON_HLSLI
#define ALAMO_COMMON_HLSLI

[[vk::binding(0, 0)]] cbuffer FrameCB {
    float4 View[4];
    float4 Proj[4];
    float4 ViewProj[4];
    float4 ViewInv[4];
    float4 EyePos;
    float4 ResolutionConstants;
    float4 FogVals_FadeVals;
    float4 Time_pad;
    float4 LightAmbient;
    float4 Light0Vector;      // world, towards the light
    float4 Light0Diffuse;
    float4 Light0Specular;
    float4 FOWTexU;
    float4 FOWTexV;
    float4 CloudTexU;
    float4 CloudTexV;
    float4 BendVector;
    float4 WindGrassVector;
    float4 FogColor;
    float4 ShadowParams;      // x extrusion distance, y depth scale
    float4 Light1Vector;      // FF fill lights 1, 2 (RENDER.md 2.3), zero in space
    float4 Light1Diffuse;
    float4 Light1Specular;
    float4 Light2Vector;
    float4 Light2Diffuse;
    float4 Light2Specular;
    float4 SphAll[12];        // SPH_LIGHT_ALL / SPH_LIGHT_FILL register images, per frame
    float4 SphFill[12];
};

struct DrawData {
    float4 World[3];
    float4 LightScale;
    uint MaterialIndex;
    uint BoneBase;
    uint Flags;
    uint Pad;
};

struct MeshMaterial {
    float4 Diffuse;
    float4 Emissive;          // .w Shininess / AtmospherePower
    float4 Specular;          // .w BendScale
    float4 Colorization;      // Planet: Atmosphere
    float4 UVOffset_Scroll;   // Planet: CityColor.rgb, CloudScrollRate
    uint4 Tex;                // packed: low 24 bits texture index, high 8 bits sampler slot
};

struct BoneMatrix {
    float4 R0;
    float4 R1;
    float4 R2;
};

[[vk::binding(1, 0)]] StructuredBuffer<DrawData> Draws;
[[vk::binding(2, 0)]] StructuredBuffer<MeshMaterial> Materials;
[[vk::binding(3, 0)]] StructuredBuffer<BoneMatrix> Bones;
[[vk::binding(0, 1)]] Texture2D Textures[];
[[vk::binding(1, 1)]] SamplerState Samplers[4];

struct PushData {
    uint DrawIndex;
    uint AlphaFunc;           // CompareOp (Vk order): 0 never .. 7 always
    uint AlphaRef;            // 0..255
    uint Flags;               // bit0 fog
};
[[vk::push_constant]] PushData PC;

float4 SampleTex(uint packed, float2 uv) {
    uint index = packed & 0x00FFFFFFu;
    uint smp = (packed >> 24) & 3u;
    return Textures[NonUniformResourceIndex(index)].Sample(Samplers[NonUniformResourceIndex(smp)], uv);
}

// normalize() that never produces NaN: a zero (or denormal) vector, e.g. a missing tangent
// basis, yields +Z (which makes a tangent-space vector point along the surface normal).
float3 SafeNormalize(float3 v) {
    float d = dot(v, v);
    return d > 1e-24 ? v * rsqrt(d) : float3(0.0, 0.0, 1.0);
}

float4 Mul4(float4 m[4], float4 v) { return float4(dot(m[0], v), dot(m[1], v), dot(m[2], v), dot(m[3], v)); }

// Per-vertex SH irradiance n^T M n for the three colour channels (register image of float4x4[3]).
float3 SphIrradiance(float4 M[12], float3 n) {
    float4 n1 = float4(n, 1.0);
    float3 r;
    r.x = dot(n1, M[0] * n.x + M[1] * n.y + M[2] * n.z + M[3]);
    r.y = dot(n1, M[4] * n.x + M[5] * n.y + M[6] * n.z + M[7]);
    r.z = dot(n1, M[8] * n.x + M[9] * n.y + M[10] * n.z + M[11]);
    return r;
}

// oFog = saturate(FOG_VALS.x * |clip.xyz| + FOG_VALS.y)
float FogFactor(float4 clip) { return saturate(FogVals_FadeVals.x * length(clip.xyz) + FogVals_FadeVals.y); }

// Object -> world rows for the draw: the world matrix, or the bone picked by NORMAL.w (rigid
// skinning, 1 bone per vertex, index truncated like the vs_1_1 code; see constants.hpp BoneMatrix).
void WorldRows(DrawData d, float boneW, out float4 r0, out float4 r1, out float4 r2) {
#ifdef SKINNED
    uint bone = d.BoneBase + (uint)max(boneW, 0.0);
    BoneMatrix b = Bones[bone];
    r0 = b.R0; r1 = b.R1; r2 = b.R2;
#else
    r0 = d.World[0]; r1 = d.World[1]; r2 = d.World[2];
#endif
}

float3 XformPoint(float4 r0, float4 r1, float4 r2, float3 p) {
    float4 p1 = float4(p, 1.0);
    return float3(dot(r0, p1), dot(r1, p1), dot(r2, p1));
}
float3 XformVector(float4 r0, float4 r1, float4 r2, float3 v) {
    return float3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v));
}

// D3D9 alpha test emulation: compare the 8-bit alpha against ALPHAREF.
void AlphaTest(float alpha) {
    if (PC.AlphaFunc >= 7u) return;
    uint a = (uint)round(saturate(alpha) * 255.0);
    uint r = PC.AlphaRef;
    bool pass;
    switch (PC.AlphaFunc) {
    case 0u: pass = false; break;
    case 1u: pass = a < r; break;
    case 2u: pass = a == r; break;
    case 3u: pass = a <= r; break;
    case 4u: pass = a > r; break;
    case 5u: pass = a != r; break;
    default: pass = a >= r; break;
    }
    if (!pass) discard;
}

// PS epilogue shared by every shader: alpha test, then D3D9 fixed-function fog.
float4 Epilogue(float4 c, float fog) {
    AlphaTest(c.a);
    if ((PC.Flags & 1u) != 0u) c.rgb = lerp(FogColor.rgb, c.rgb, fog);
    return c;
}

#endif

// AlamoMesh ubershader: Mesh* / RSkin* / Tree (PORTING.md section 5). Reference: the lifted
// MESHBUMPCOLORIZE, RSKINBUMPCOLORIZE, MESHGLOSS, MESHGLOSSCOLORIZE, TREE, MESHADDITIVE code
// in docs/shaders/shaders_decompiled.zip (best technique of each effect).
//
// Vertex-shader permutations (VSMain):
//   BUMP      tangent-space light/half vectors, SH fill irradiance (MeshBumpColorize DX9 VS)
//   GLOSS     SH all irradiance + per-vertex specular in COLOR1 (MeshGloss / GlossColorize DX8 VS)
//   BEND      + tree wind bend (Tree DX8 VS)
//   ADDITIVE  unlit LIGHT_SCALE * Color, UV scroll (MeshAdditive DX8 VS)
//   SKINNED   rigid palette skinning, bone index in NORMAL.w (RSkin*)
// Pixel-shader permutations (PSMain):
//   BUMP COLORIZE      MeshBumpColorize ps_2_0 (spec pow 16 masked by normal-map alpha)
//   GLOSS              MeshGloss ps_1_1 (gloss masked by base alpha)
//   GLOSS COLORIZE GLOSSMAP   MeshGlossColorize ps_1_1 (gloss masked by GlossTexture.r)
//   TREE               Tree ps_1_1 (alpha = tex.a * diffuse.a, alpha test via push constants)
//   ADDITIVE           MeshAdditive ps_1_1 (tex * colour)
// Lighting is evaluated in world space for mesh and skinned variants alike (the original mesh
// VS used object space; identical for rigid transforms).
#include "common.hlsli"

struct VSIn {
    [[vk::location(0)]] float3 Pos : POSITION;
#if !defined(ADDITIVE) || defined(SKINNED)
    [[vk::location(1)]] float4 Normal : NORMAL;     // .w = bone index for SKINNED
#endif
    [[vk::location(2)]] float2 UV : TEXCOORD0;
#ifdef BUMP
    [[vk::location(3)]] float3 Tangent : TANGENT;
    [[vk::location(4)]] float3 Binormal : BINORMAL;
#endif
};

struct VSOut {
    float4 Pos : SV_Position;
    float4 Color0 : COLOR0;
    float4 Color1 : COLOR1;
    float4 UV01 : TEXCOORD0;      // uv0, uv1
    float3 LightTS : TEXCOORD2;   // BUMP: tangent-space light vector (unpacked, not *0.5+0.5)
    float3 HalfTS : TEXCOORD3;    // BUMP: tangent-space half vector
    float Fog : TEXCOORD4;
    nointerpolation uint DrawId : TEXCOORD5;
};

VSOut VSMain(VSIn v, uint iid : SV_InstanceID) {
    VSOut o;
    uint drawId = PC.DrawIndex + iid;
    DrawData d = Draws[drawId];
    MeshMaterial m = Materials[d.MaterialIndex];
    float4 r0, r1, r2;
#if !defined(ADDITIVE) || defined(SKINNED)
    WorldRows(d, v.Normal.w, r0, r1, r2);
#else
    WorldRows(d, 0.0, r0, r1, r2);
#endif
    float3 wp = XformPoint(r0, r1, r2, v.Pos);
#ifdef BEND
    // bend = BendVector.xyz * BendScale * (z_obj^2 * BendVector.w), applied in world space
    float h = v.Pos.z * v.Pos.z * BendVector.w;
    wp += BendVector.xyz * m.Specular.w * h;
#endif
    float4 clip = Mul4(ViewProj, float4(wp, 1.0));
    o.Pos = clip;
    o.DrawId = drawId;
    o.LightTS = float3(0, 0, 1);
    o.HalfTS = float3(0, 0, 1);
    o.Color1 = float4(0, 0, 0, 1);

#ifdef ADDITIVE
    o.Color0 = float4(d.LightScale.rgb * m.Diffuse.rgb * d.LightScale.w, 1.0);
    float2 uv = v.UV + Time_pad.x * m.UVOffset_Scroll.zw;
    o.UV01 = float4(uv, uv);
    o.Fog = 1.0;   // the additive VS writes oFog = 1 (no fog)
#else
    float3 wn = SafeNormalize(XformVector(r0, r1, r2, v.Normal.xyz));
    float3 L = Light0Vector.xyz;
    float3 E = SafeNormalize(EyePos.xyz - wp);
    float3 H = SafeNormalize(E + L);
#ifdef BUMP
    float3 T = XformVector(r0, r1, r2, v.Tangent);
    float3 B = XformVector(r0, r1, r2, v.Binormal);
    o.LightTS = SafeNormalize(float3(dot(L, T), dot(L, B), dot(L, wn)));
    o.HalfTS = SafeNormalize(float3(dot(H, T), dot(H, B), dot(H, wn)));
    float3 irr = SphIrradiance(SphFill, wn);
    float2 uv = v.UV + m.UVOffset_Scroll.xy;
#else
    float3 irr = SphIrradiance(SphAll, wn);
    float2 uv = v.UV;
    // per-vertex specular: max(N.H, 0)^16 (fixed exponent; Shininess is unused by the DX8 VS)
    float s = max(dot(wn, H), 0.0);
    s *= s; s *= s; s *= s; s *= s;
    o.Color1 = float4(s * Light0Specular.rgb * m.Specular.rgb, 1.0);
#endif
    o.Color0 = float4(irr * m.Diffuse.rgb * d.LightScale.rgb + m.Emissive.rgb, d.LightScale.w);
    o.UV01 = float4(uv, uv);
    o.Fog = FogFactor(clip);
#endif
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    DrawData d = Draws[i.DrawId];
    MeshMaterial m = Materials[d.MaterialIndex];
    float4 base = SampleTex(m.Tex.x, i.UV01.xy);
    float4 c;
#if defined(ADDITIVE)
    c = base * i.Color0;
#elif defined(BUMP)
    float4 nm = SampleTex(m.Tex.y, i.UV01.zw);
    float3 col = base.rgb;
#ifdef COLORIZE
    col = lerp(base.rgb, base.rgb * m.Colorization.rgb, base.a);
#endif
    float3 n = nm.xyz * 2.0 - 1.0;
    float3 diff = saturate(dot(n, i.LightTS)) * m.Diffuse.rgb * Light0Diffuse.rgb * d.LightScale.rgb + i.Color0.rgb;
    float spec = pow(saturate(dot(n, i.HalfTS)), 16.0);
    float3 rgb = col * diff * 2.0 + spec * Light0Specular.rgb * m.Specular.rgb * nm.a;
    c = float4(rgb, i.Color0.a);
#elif defined(TREE)
    c.rgb = 2.0 * base.rgb * i.Color0.rgb + 2.0 * base.a * i.Color1.rgb;
    c.a = base.a * i.Color0.a;
#elif defined(GLOSS)
    float3 col = base.rgb;
    float mask = base.a;
#ifdef COLORIZE
    col = lerp(base.rgb, base.rgb * m.Colorization.rgb, base.a);
#endif
#ifdef GLOSSMAP
    mask = SampleTex(m.Tex.y, i.UV01.zw).r;
#endif
    c = float4(2.0 * col * i.Color0.rgb + mask * i.Color1.rgb, i.Color0.a);
#else
    c = base * i.Color0;
#endif
    return Epilogue(c, i.Fog);
}

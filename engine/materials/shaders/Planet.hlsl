// Planet (DX9 technique sph_t3: vs_1_1 sph_bump_spec_vs_main_bin + ps_2_0 bump_spec_ps_main_bin):
// bump-mapped surface with specular, atmosphere rim, scrolling clouds with their own normal map,
// and city lights on the night side. MeshMaterial mapping (params.cpp): Diffuse, Emissive.xyz,
// Emissive.w = AtmospherePower, Specular, Colorization = Atmosphere,
// UVOffset_Scroll = (CityColor.rgb, CloudScrollRate); Tex = Base, Normal, Cloud, CloudNormal.
#include "common.hlsli"

struct VSIn {
    [[vk::location(0)]] float3 Pos : POSITION;
    [[vk::location(1)]] float4 Normal : NORMAL;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
    [[vk::location(3)]] float3 Tangent : TANGENT;
    [[vk::location(4)]] float3 Binormal : BINORMAL;
};

struct VSOut {
    float4 Pos : SV_Position;
    float4 Color0 : COLOR0;
    float4 UV01 : TEXCOORD0;      // base/normal uv
    float2 UVCloud : TEXCOORD1;
    float3 LightTS : TEXCOORD2;
    float3 HalfTS : TEXCOORD3;
    float2 AtmoCity : TEXCOORD4;  // x = 2 * atmosphere rim, y = city-light (night side) factor
    float Fog : TEXCOORD5;
    nointerpolation uint DrawId : TEXCOORD6;
};

VSOut VSMain(VSIn v, uint iid : SV_InstanceID) {
    VSOut o;
    uint drawId = PC.DrawIndex + iid;
    DrawData d = Draws[drawId];
    MeshMaterial m = Materials[d.MaterialIndex];
    float4 r0 = d.World[0], r1 = d.World[1], r2 = d.World[2];
    float3 wp = XformPoint(r0, r1, r2, v.Pos);
    float3 wn = SafeNormalize(XformVector(r0, r1, r2, v.Normal.xyz));
    float3 T = XformVector(r0, r1, r2, v.Tangent);
    float3 B = XformVector(r0, r1, r2, v.Binormal);
    float3 L = Light0Vector.xyz;
    float3 E = SafeNormalize(EyePos.xyz - wp);
    float3 H = SafeNormalize(E + L);
    float4 clip = Mul4(ViewProj, float4(wp, 1.0));
    o.Pos = clip;
    o.DrawId = drawId;
    o.LightTS = SafeNormalize(float3(dot(L, T), dot(L, B), dot(L, wn)));
    o.HalfTS = SafeNormalize(float3(dot(H, T), dot(H, B), dot(H, wn)));
    float3 irr = SphIrradiance(SphFill, wn);
    o.Color0 = float4(irr * m.Diffuse.rgb * d.LightScale.rgb + m.Emissive.rgb, d.LightScale.w);
    float rim = exp2(log2(abs(1.0 - dot(wn, E))) * m.Emissive.w) * m.Colorization.w;
    o.AtmoCity = float2(rim + rim, saturate(-32.0 * dot(wn, L)));
    o.UV01 = float4(v.UV, v.UV);
    o.UVCloud = float2(v.UV.x + Time_pad.x * m.UVOffset_Scroll.w, v.UV.y);
    o.Fog = FogFactor(clip);
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    DrawData d = Draws[i.DrawId];
    MeshMaterial m = Materials[d.MaterialIndex];
    float4 nm = SampleTex(m.Tex.y, i.UV01.zw);
    float4 base = SampleTex(m.Tex.x, i.UV01.xy);
    float4 cloud = SampleTex(m.Tex.z, i.UVCloud);
    float4 cn = SampleTex(m.Tex.w, i.UVCloud);
    float3 n = nm.xyz * 2.0 - 1.0;
    float3 spec = pow(saturate(dot(n, i.HalfTS)), 16.0) * Light0Specular.rgb * m.Specular.rgb;
    float3 lit = saturate(dot(n, i.LightTS)) * m.Diffuse.rgb * Light0Diffuse.rgb + i.Color0.rgb;
    float3 surf = m.Colorization.rgb * i.AtmoCity.x + base.rgb;
    float city = base.a * i.AtmoCity.y * (1.0 - cloud.a);
    float3 ground = 2.0 * lit * surf + city * m.UVOffset_Scroll.rgb + spec * nm.a;
    float cd = saturate(dot(cn.xyz * 2.0 - 1.0, i.LightTS));
    float3 clouds = 2.0 * (cloud.rgb * cd * Light0Diffuse.rgb + i.Color0.rgb);
    float3 rgb = lerp(ground, clouds, cloud.a);
    return Epilogue(float4(rgb, i.Color0.a), i.Fog);
}

// Fixed-function ubershader (PORTING.md 3.4): D3D9 FF vertex lighting (directional light 0 +
// global ambient, material colour sources), texture-coordinate generation and transforms, and
// a 3-stage texture-stage combiner, all driven by the per-draw FfDrawData record (set 0
// binding 4) instead of compile-time keys, so one SPIR-V pair per vertex layout covers every
// vanilla and mod FF configuration (MeshAlpha, MeshAlphaScroll, Prim*, Phase*, FrameEffect*,
// SpaceFogOfWar, TerrainPassability, StencilDarken*, BlobStencilMasked, ...).
// Vertex permutations: FF_NORMAL (vertex has NORMAL), FF_COLOR (vertex has COLOR0), SKINNED (with
// FF_NORMAL: rigid palette skinning, bone index in NORMAL.w, same Bones[] convention as AlamoMesh).
#include "common.hlsli"

struct FfStage {
    uint4 A;   // color_op, color_arg1, color_arg2, alpha_op
    uint4 B;   // alpha_arg1, alpha_arg2, texcoord_index, transform_flags
};
struct FfDrawData {
    float4 MatDiffuse;
    float4 MatAmbient;
    float4 MatSpecular;
    float4 MatEmissive;
    float4 TFactor;
    float4 Params;          // x power, y lighting, z specular enable, w color vertex
    uint4 Sources;          // diffuse, specular, ambient, emissive material source (D3DMCS)
    float4 TexXform[12];    // [stage * 4 + row]
    FfStage Stage[3];
    uint4 Tex;
    uint4 Misc;             // x active stages
};
[[vk::binding(4, 0)]] StructuredBuffer<FfDrawData> FfDraws;

struct VSIn {
    [[vk::location(0)]] float3 Pos : POSITION;
#ifdef FF_NORMAL
    [[vk::location(1)]] float4 Normal : NORMAL;
#endif
    [[vk::location(2)]] float2 UV : TEXCOORD0;
#ifdef FF_COLOR
    [[vk::location(5)]] float4 Color : COLOR0;
#endif
};

struct VSOut {
    float4 Pos : SV_Position;
    float4 Diffuse : COLOR0;
    float4 Specular : COLOR1;
    float4 TC0 : TEXCOORD0;
    float4 TC1 : TEXCOORD1;
    float4 TC2 : TEXCOORD2;
    float Fog : TEXCOORD3;
    nointerpolation uint DrawId : TEXCOORD4;
};

float4 MaterialSource(uint src, float4 mat, float4 vcolor, bool hasColor, bool colorVertex) {
    // D3DMCS_COLOR1 = vertex diffuse; COLOR2 (vertex specular) is never present in Alamo layouts.
    return (colorVertex && hasColor && src == 1u) ? vcolor : mat;
}

float4 GenTexCoord(FfDrawData f, uint s, float2 uv, float3 viewPos, float3 viewN) {
    uint tci = f.Stage[s].B.z;
    uint mode = tci & 0xFFFF0000u;
    float4 tin;
    if (mode == 0x20000u) {               // CAMERASPACEPOSITION
        tin = float4(viewPos, 1.0);
    } else if (mode == 0x10000u) {        // CAMERASPACENORMAL
        tin = float4(viewN, 1.0);
    } else if (mode == 0x30000u) {        // CAMERASPACEREFLECTIONVECTOR
        float3 e = SafeNormalize(-viewPos);
        tin = float4(2.0 * dot(e, viewN) * viewN - e, 1.0);
    } else {                              // PASSTHRU: 2D coordinates expand to (u, v, 1, 0)
        tin = float4(uv, 1.0, 0.0);
    }
    uint ttff = f.Stage[s].B.w;
    if ((ttff & 0xFFu) != 0u) {
        uint b = s * 4u;
        tin = float4(dot(f.TexXform[b], tin), dot(f.TexXform[b + 1u], tin), dot(f.TexXform[b + 2u], tin),
                     dot(f.TexXform[b + 3u], tin));
    }
    return tin;
}

VSOut VSMain(VSIn v, uint iid : SV_InstanceID) {
    VSOut o;
    uint drawId = PC.DrawIndex + iid;
    DrawData d = Draws[drawId];
    FfDrawData f = FfDraws[d.MaterialIndex];
#ifdef FF_NORMAL
    float boneW = v.Normal.w;
#else
    float boneW = 0.0;
#endif
    float4 r0, r1, r2;
    WorldRows(d, boneW, r0, r1, r2);   // DrawData.World, or Bones[BoneBase + trunc(NORMAL.w)] (SKINNED)
    float3 wp = XformPoint(r0, r1, r2, v.Pos);
    float4 clip = Mul4(ViewProj, float4(wp, 1.0));
    o.Pos = clip;
    o.DrawId = drawId;
#ifdef FF_COLOR
    float4 vcolor = v.Color;
    bool hasColor = true;
#else
    float4 vcolor = float4(1, 1, 1, 1);
    bool hasColor = false;
#endif
#ifdef FF_NORMAL
    float3 wn = SafeNormalize(XformVector(r0, r1, r2, v.Normal.xyz));
#else
    float3 wn = float3(0, 0, 0);
#endif
    float4 diffuse = vcolor;
    float4 specular = float4(0, 0, 0, 0);
    if (f.Params.y != 0.0) {   // LIGHTING
        bool cv = f.Params.w != 0.0;
        float4 md = MaterialSource(f.Sources.x, f.MatDiffuse, vcolor, hasColor, cv);
        float4 ms = MaterialSource(f.Sources.y, f.MatSpecular, vcolor, hasColor, cv);
        float4 ma = MaterialSource(f.Sources.z, f.MatAmbient, vcolor, hasColor, cv);
        float4 me = MaterialSource(f.Sources.w, f.MatEmissive, vcolor, hasColor, cv);
        // D3D9 FF lighting as the engine set it up (RENDER.md 1.4 step 3, 2.3): D3DRS_AMBIENT = 0,
        // three directional lights (sun + 2 fills), scene ambient on light 0 only:
        //   colour = Me + Ma * ambient + sum_i Md * Ld_i * max(N.L_i, 0)
        //   spec   = sum_i Ms * Ls_i * max(N.H_i, 0)^power   (N.L_i > 0, LOCALVIEWER = TRUE)
        float3 Ls[3] = {Light0Vector.xyz, Light1Vector.xyz, Light2Vector.xyz};
        float3 Ld[3] = {Light0Diffuse.rgb, Light1Diffuse.rgb, Light2Diffuse.rgb};
        float3 Lsp[3] = {Light0Specular.rgb, Light1Specular.rgb, Light2Specular.rgb};
        float3 c = me.rgb + ma.rgb * LightAmbient.rgb;
        float3 sp = float3(0, 0, 0);
        float3 E = SafeNormalize(EyePos.xyz - wp);
        float p = max(f.Params.x, 0.0);
        for (uint li = 0u; li < 3u; ++li) {
            float ndl = dot(wn, Ls[li]);
            c += md.rgb * Ld[li] * max(ndl, 0.0);
            if (f.Params.z != 0.0 && ndl > 0.0) {   // SPECULARENABLE
                float3 H = SafeNormalize(E + Ls[li]);
                float s = p > 0.0 ? pow(max(dot(wn, H), 0.0), p) : 1.0;
                sp += ms.rgb * Lsp[li] * s;
            }
        }
        diffuse = float4(saturate(c), saturate(md.a));
        specular = float4(saturate(sp), 0.0);
    }
    o.Diffuse = diffuse;
    o.Specular = specular;
    float3 viewPos = Mul4(View, float4(wp, 1.0)).xyz;
    float3 viewN = float3(dot(View[0].xyz, wn), dot(View[1].xyz, wn), dot(View[2].xyz, wn));
    o.TC0 = GenTexCoord(f, 0u, v.UV, viewPos, viewN);
    o.TC1 = GenTexCoord(f, 1u, v.UV, viewPos, viewN);
    o.TC2 = GenTexCoord(f, 2u, v.UV, viewPos, viewN);
    o.Fog = FogFactor(clip);
    return o;
}

float4 Arg(uint sel, float4 diffuse, float4 current, float4 tex, float4 tfactor, float4 spec) {
    uint s = sel & 0xFu;
    float4 a = s == 0u ? diffuse : s == 1u ? current : s == 2u ? tex : s == 3u ? tfactor : s == 4u ? spec
                                                                                         : float4(0, 0, 0, 0);
    if ((sel & 0x10u) != 0u) a = 1.0 - a;      // D3DTA_COMPLEMENT
    if ((sel & 0x20u) != 0u) a = a.aaaa;       // D3DTA_ALPHAREPLICATE
    return a;
}

float4 Combine(uint op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 current, float4 tex,
               float4 tfactor) {
    switch (op) {
    case 2u: return a1;                                     // SELECTARG1
    case 3u: return a2;                                     // SELECTARG2
    case 4u: return a1 * a2;                                // MODULATE
    case 5u: return 2.0 * a1 * a2;                          // MODULATE2X
    case 6u: return 4.0 * a1 * a2;                          // MODULATE4X
    case 7u: return a1 + a2;                                // ADD
    case 8u: return a1 + a2 - 0.5;                          // ADDSIGNED
    case 9u: return 2.0 * (a1 + a2 - 0.5);                  // ADDSIGNED2X
    case 10u: return a1 - a2;                               // SUBTRACT
    case 11u: return a1 + a2 - a1 * a2;                     // ADDSMOOTH
    case 12u: return lerp(a2, a1, diffuse.a);               // BLENDDIFFUSEALPHA
    case 13u: return lerp(a2, a1, tex.a);                   // BLENDTEXTUREALPHA
    case 14u: return lerp(a2, a1, tfactor.a);               // BLENDFACTORALPHA
    case 15u: return a1 + a2 * (1.0 - tex.a);               // BLENDTEXTUREALPHAPM
    case 16u: return lerp(a2, a1, current.a);               // BLENDCURRENTALPHA
    case 17u: return a1 * a2;                               // PREMODULATE (approximation)
    case 18u: return float4(a1.rgb + a1.a * a2.rgb, a1.a);  // MODULATEALPHA_ADDCOLOR
    case 19u: return float4(a1.rgb * a2.rgb + a1.a, a1.a);  // MODULATECOLOR_ADDALPHA
    case 20u: return float4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);  // MODULATEINVALPHA_ADDCOLOR
    case 21u: return float4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);  // MODULATEINVCOLOR_ADDALPHA
    case 24u: return saturate(dot(a1.rgb * 2.0 - 1.0, a2.rgb * 2.0 - 1.0)).xxxx;  // DOTPRODUCT3
    case 25u: return a0 + a1 * a2;                          // MULTIPLYADD (arg0 = CURRENT)
    case 26u: return a0 * a1 + (1.0 - a0) * a2;             // LERP (arg0 = CURRENT)
    default: return current;                                // DISABLE / BUMPENVMAP*: pass through
    }
}

float4 StageTex(FfDrawData f, uint s, float4 tc) {
    uint packed = s == 0u ? f.Tex.x : s == 1u ? f.Tex.y : f.Tex.z;
    float2 uv = tc.xy;
    uint ttff = f.Stage[s].B.w;
    if ((ttff & 0x100u) != 0u) {   // D3DTTFF_PROJECTED: divide by the last counted component
        uint n = ttff & 0xFFu;
        float q = n == 4u ? tc.w : n == 3u ? tc.z : tc.y;
        uv = tc.xy / q;
    }
    return SampleTex(packed, uv);
}

float4 PSMain(VSOut i) : SV_Target0 {
    DrawData d = Draws[i.DrawId];
    FfDrawData f = FfDraws[d.MaterialIndex];
    float4 current = i.Diffuse;
    float4 tcs[3] = {i.TC0, i.TC1, i.TC2};
    for (uint s = 0u; s < 3u; ++s) {
        if (s >= f.Misc.x) break;
        uint cop = f.Stage[s].A.x;
        if (cop == 1u) break;      // D3DTOP_DISABLE ends the cascade
        float4 tex = StageTex(f, s, tcs[s]);
        float4 c1 = Arg(f.Stage[s].A.y, i.Diffuse, current, tex, f.TFactor, i.Specular);
        float4 c2 = Arg(f.Stage[s].A.z, i.Diffuse, current, tex, f.TFactor, i.Specular);
        float4 a1 = Arg(f.Stage[s].B.x, i.Diffuse, current, tex, f.TFactor, i.Specular);
        float4 a2 = Arg(f.Stage[s].B.y, i.Diffuse, current, tex, f.TFactor, i.Specular);
        float3 rgb = Combine(cop, c1, c2, current, i.Diffuse, current, tex, f.TFactor).rgb;
        uint aop = f.Stage[s].A.w;
        float alpha = aop == 1u ? current.a : Combine(aop, a1, a2, current, i.Diffuse, current, tex, f.TFactor).a;
        current = saturate(float4(rgb, alpha));
    }
    if (f.Params.z != 0.0) current.rgb = saturate(current.rgb + i.Specular.rgb);
    return Epilogue(current, i.Fog);
}

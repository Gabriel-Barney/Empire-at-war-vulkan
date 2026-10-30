// alamo_ui_draw HUD 2D pass (P2, docs/re/UI.md §2.5): textured, vertex-coloured quads in HUD canvas units drawn
// with an identity world and the HUD components ortho camera (push constants: canvas -> clip scale / offset).
// set 0: binding 0 = Texture2D pages[16] (0 white, 1 MT_CommandBar atlas, 2.. glyph pages), binding 1 = sampler.
// Clip-space Y is up (the renderer's D3D convention; the pass uses a negative-height viewport).

struct PushData {
    float4 xform;   // clip.xy = pos.xy * xform.xy + xform.zw
    uint page;      // texture page of the batch (dynamically uniform)
    uint mode;      // colour op: 0 = tex x diffuse (PrimOpaque / PrimAlpha / PrimAdditive), 1 = tex + diffuse (PrimModulate)
    uint2 pad;
};
[[vk::push_constant]] PushData PC;

[[vk::binding(0, 0)]] Texture2D Pages[32];  // + 16..31 image pages (P4 GUI textures)
[[vk::binding(1, 0)]] SamplerState Smp;

struct VSIn {
    [[vk::location(0)]] float2 pos : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 col : COLOR0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos * PC.xform.xy + PC.xform.zw, 0.0, 1.0);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    const float4 t = Pages[PC.page].Sample(Smp, i.uv);
    if (PC.mode == 1u) return saturate(t + i.col);  // PrimModulate: the blend multiplies the target by this
    return t * i.col;
}

// alamo_render TAA object motion [port] (render/src/taa.cpp, taa.hlsl).
// Re-draws the opaque items of instances that moved since the last TAA frame into an RGBA32F target, depth-tested
// (less-or-equal, no write) against the finished scene depth so only the visible surface of a mover is written:
//   rgb = where that surface point was in the previous frame's image (uv.x, uv.y, NDC depth), a = 1.
// The resolve reads it at the nearest-depth pixel of each 3x3 neighbourhood in place of the camera-only reprojection.
//
// Uses the scene build's hot set 0 (FrameCB, DrawData, bones: common.hlsli twin) for the item's world matrix or the
// rigid-skin bone, so every item of the build - translated or not - is covered. The mover's own motion arrives as
// Prev = previous view-projection x (previous world x inverse current world) of the instance, so the mover is moved back
// rigidly (its skeleton animation is not: that stays with the clip).
// SKINNED: vertex layout with the palette index in NORMAL.w (RSkin*); otherwise the static layout (no NORMAL read).

[[vk::binding(0, 0)]] cbuffer FrameCB {
    float4 View[4];
    float4 Proj[4];
    float4 ViewProj[4];
};

struct DrawData {
    float4 World[3];
    float4 LightScale;
    uint MaterialIndex;
    uint BoneBase;
    uint Flags;
    uint Pad;
};
struct BoneMatrix {
    float4 R0;
    float4 R1;
    float4 R2;
};
[[vk::binding(1, 0)]] StructuredBuffer<DrawData> Draws;
[[vk::binding(3, 0)]] StructuredBuffer<BoneMatrix> Bones;

struct VelPush {
    float4 Prev[4];   // world -> previous clip (previous view-projection x the instance's motion), register rows
    uint DrawIndex;   // the item's DrawData / bone slot
    float PixelX;     // clip offset of the translated draws (Renderer::translated_pixel_offset), 0 for the others
    float PixelY;
    uint Pad;
};
[[vk::push_constant]] VelPush PC;

// Depth nudge towards the camera (NDC): the scene's own shaders round differently from this one.
static const float kDepthBias = 3e-6;

struct VSIn {
    [[vk::location(0)]] float3 Pos : POSITION;
#ifdef SKINNED
    [[vk::location(1)]] float4 Normal : NORMAL;
#endif
};
struct VSOut {
    float4 Pos : SV_Position;
    float4 Prev : TEXCOORD0;
};

VSOut VSMain(VSIn i) {
    const DrawData d = Draws[PC.DrawIndex];
    const float4 p1 = float4(i.Pos, 1.0);
    float4 r0, r1, r2;
#ifdef SKINNED
    const BoneMatrix b = Bones[d.BoneBase + (uint)max(i.Normal.w, 0.0)];
    r0 = b.R0; r1 = b.R1; r2 = b.R2;
#else
    r0 = d.World[0]; r1 = d.World[1]; r2 = d.World[2];
#endif
    const float4 w = float4(dot(r0, p1), dot(r1, p1), dot(r2, p1), 1.0);
    VSOut o;
    o.Pos = float4(dot(ViewProj[0], w), dot(ViewProj[1], w), dot(ViewProj[2], w), dot(ViewProj[3], w));
    o.Pos.xy += float2(PC.PixelX, PC.PixelY) * o.Pos.w;
    o.Pos.z -= kDepthBias * o.Pos.w;
    o.Prev = float4(dot(PC.Prev[0], w), dot(PC.Prev[1], w), dot(PC.Prev[2], w), dot(PC.Prev[3], w));
    o.Prev.xy += float2(PC.PixelX, PC.PixelY) * o.Prev.w;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    if (i.Prev.w <= 1e-6) return float4(0.0, 0.0, 0.0, 0.0);  // behind the previous camera: no vector
    const float2 n = i.Prev.xy / i.Prev.w;
    return float4(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5, i.Prev.z / i.Prev.w, 1.0);
}

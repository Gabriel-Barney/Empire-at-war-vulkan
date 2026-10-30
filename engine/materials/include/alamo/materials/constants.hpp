// GPU constant layouts shared by the C++ side and engine/materials/shaders/common.hlsli
// (PORTING.md 2.2 - 2.4). Every member is a float4 / uint4 (or an array of them), so the
// layout is identical under std140, std430 and -fvk-use-dx-layout.
//
// Matrix convention ("register rows"): a matrix is stored as the rows R[i] that the vertex
// shader dots with the input vector, out[i] = dot(R[i], v). This is exactly the D3D9 register
// image of a column_major HLSL matrix, i.e. the TRANSPOSE of the row-major D3DXMATRIX the
// original engine passed to SetMatrix (row-vector convention v * M). Use
// matrix_rows_from_d3dx() to convert.
#pragma once
#include <cstddef>
#include <cstdint>

namespace alamo::materials {

struct float4 { float x = 0, y = 0, z = 0, w = 0; };
struct uint4 { uint32_t x = 0, y = 0, z = 0, w = 0; };
static_assert(sizeof(float4) == 16 && sizeof(uint4) == 16, "vector sizes");

// out[r] = column r of a row-major 4x4 (D3DXMATRIX m[16], m[row*4+col]).
void matrix_rows_from_d3dx(const float* d3dx16, float4* out_rows, int rows = 4);

// set 0, binding 0 - per view / phase. 992 bytes.
struct FrameCB {
    float4 view[4], proj[4], view_proj[4], view_inv[4];  // VIEW, PROJECTION, VIEWPROJECTION, VIEWINVERSE
    float4 eye_pos;                 // EYE_POSITION (world)
    float4 resolution_constants;    // RESOLUTION_CONSTANTS
    float4 fog_vals_fade_vals;      // FOG_VALS.xy, DISTANCE_FADE_VALS.xy
    float4 time_pad;                // TIME in .x
    float4 light_ambient;           // GLOBAL_AMBIENT (also D3DRS_AMBIENT for the FF path)
    float4 light0_vector;           // DIR_LIGHT_VEC_0, world space, points TOWARDS the light
    float4 light0_diffuse;          // DIR_LIGHT_DIFFUSE_0
    float4 light0_specular;         // DIR_LIGHT_SPECULAR_0
    float4 fow_tex_u, fow_tex_v, cloud_tex_u, cloud_tex_v;
    float4 bend_vector;             // WIND_BEND_VECTOR
    float4 wind_grass_vector;       // WIND_GRASS_PARAMS
    float4 fog_color;               // was D3DRS_FOGCOLOR
    float4 shadow_params;           // x SHADOW_EXTRUSION_DISTANCE, y DEPTH_SCALE (addition to 2.2)
    // Fixed-function lights 1 and 2 (the two fills, RENDER.md 2.3; zero in space). The FF path
    // lights with D3DLIGHT 0..2 + ambient (light 0's ambient = scene ambient, D3DRS_AMBIENT = 0);
    // the shader paths only see light 0 and the SH matrices. World space, towards the light;
    // diffuse = colour x intensity, specular = specular x 2 x intensity.
    float4 light1_vector, light1_diffuse, light1_specular;
    float4 light2_vector, light2_diffuse, light2_specular;
    // Scene SH irradiance (render lighting.hpp scene_light_constants), one value for the whole frame.
    float4 sph_all[12];     // SPH_LIGHT_ALL  float4x4[3], D3D9 register image (c0..c11 of the original)
    float4 sph_fill[12];    // SPH_LIGHT_FILL float4x4[3]
};
static_assert(offsetof(FrameCB, view) == 0);
static_assert(offsetof(FrameCB, proj) == 64);
static_assert(offsetof(FrameCB, view_proj) == 128);
static_assert(offsetof(FrameCB, view_inv) == 192);
static_assert(offsetof(FrameCB, eye_pos) == 256);
static_assert(offsetof(FrameCB, fog_vals_fade_vals) == 288);
static_assert(offsetof(FrameCB, light_ambient) == 320);
static_assert(offsetof(FrameCB, light0_vector) == 336);
static_assert(offsetof(FrameCB, fow_tex_u) == 384);
static_assert(offsetof(FrameCB, bend_vector) == 448);
static_assert(offsetof(FrameCB, fog_color) == 480);
static_assert(offsetof(FrameCB, shadow_params) == 496);
static_assert(offsetof(FrameCB, light1_vector) == 512);
static_assert(offsetof(FrameCB, light2_specular) == 592);
static_assert(offsetof(FrameCB, sph_all) == 608);
static_assert(offsetof(FrameCB, sph_fill) == 800);
static_assert(sizeof(FrameCB) == 992);

// set 0, binding 1 - per draw, StructuredBuffer<DrawData> indexed by
// PushConstants::draw_index + SV_InstanceID. 80 bytes. (The SH matrices are per frame: FrameCB.)
struct DrawData {
    float4 world[3];        // WORLD as register rows (float3x4): world_pos[i] = dot(world[i], float4(p,1))
    float4 light_scale;     // LIGHT_SCALE (.w = fade alpha)
    uint32_t material_index, bone_base, flags, pad;  // MaterialTable / FfDrawData index, palette slice
};
static_assert(offsetof(DrawData, world) == 0);
static_assert(offsetof(DrawData, light_scale) == 48);
static_assert(offsetof(DrawData, material_index) == 64);
static_assert(offsetof(DrawData, bone_base) == 68);
static_assert(sizeof(DrawData) == 80);

// Texture reference packed in MeshMaterial::tex / FfDrawData::tex: low 24 bits = bindless index
// into the set-1 texture array, high 8 bits = engine sampler slot (EngineSampler).
constexpr uint32_t pack_texture(uint32_t bindless_index, uint32_t sampler_slot) {
    return (bindless_index & 0x00FFFFFFu) | (sampler_slot << 24);
}
enum EngineSampler : uint32_t {   // set 1 binding 1 sampler array (quality-controlled by the engine)
    kSamplerWrapLinear = 0,       // WRAP, linear + linear mip (+ engine anisotropy)
    kSamplerClampLinear = 1,
    kSamplerWrapPoint = 2,        // Tree: MinFilter/MagFilter POINT, MipFilter LINEAR
    kSamplerClampPoint = 3,
    kEngineSamplerCount = 4
};
// "No texture" = bindless slot 0, which the renderer reserves for a 1x1 opaque white texture
// (D3D9 FF reads a missing texture as white). The sampler slot still goes in the high 8 bits.
constexpr uint32_t kNoTexture = 0u;

// set 0, binding 2 - MaterialTable record for the whole mesh family (PORTING.md 2.3), 96 bytes.
struct MeshMaterial {
    float4 diffuse;           // .xyz Diffuse, .w Diffuse.w where float4 (BatchMeshAlpha) / 1
    float4 emissive;          // .xyz Emissive, .w Shininess (Planet: AtmospherePower)
    float4 specular;          // .xyz Specular, .w BendScale (Tree/Grass)
    float4 colorization;      // team colour (Planet: Atmosphere)
    float4 uv_offset_scroll;  // .xy UVOffset, .zw UVScrollRate (Planet: .xyz CityColor, .w CloudScrollRate)
    uint4 tex;                // packed textures: Base, Normal/Gloss, Cloud, CloudNormal
};
static_assert(offsetof(MeshMaterial, diffuse) == 0);
static_assert(offsetof(MeshMaterial, emissive) == 16);
static_assert(offsetof(MeshMaterial, specular) == 32);
static_assert(offsetof(MeshMaterial, colorization) == 48);
static_assert(offsetof(MeshMaterial, uv_offset_scroll) == 64);
static_assert(offsetof(MeshMaterial, tex) == 80);
static_assert(sizeof(MeshMaterial) == 96);

// set 0, binding 3 - BonePalette SSBO, one world-space bone matrix per entry (PORTING.md 2.4).
// Rigid skinning, 1 bone per vertex, no weights. The vertex shader reads the bone index from
// NORMAL.w (truncated, as the original vs_1_1 code did) and fetches
// BonePalette[DrawData.bone_base + index]. NORMAL.w holds the SUBMESH-LOCAL index (the ALO
// 0x10006 bone map), so the slice at bone_base must be laid out in submesh bone-map order
// (24 entries max in vanilla data), unless the asset loader rewrites NORMAL.w to skeleton
// indices (then bone_base = the instance's skeleton slice).
struct BoneMatrix {
    float4 rows[3];           // world_pos[i] = dot(rows[i], float4(p, 1)); normals use rows[i].xyz
};
static_assert(sizeof(BoneMatrix) == 48);
constexpr int kOriginalPaletteBones = 24;     // m_skinMatrixArray float3x4[24], c0..c71

// set 0, binding 4 - per-draw fixed-function data (FF ubershader, PORTING.md 3.4), 432 bytes.
// Built on the CPU per draw from the resolved FF pass (fixed_function.hpp).
struct FfStage {
    uint32_t color_op, color_arg1, color_arg2, alpha_op;            // D3DTOP_* / D3DTA_* values
    uint32_t alpha_arg1, alpha_arg2, texcoord_index, transform_flags;  // D3DTSS_TCI_*, D3DTTFF_*
};
static_assert(sizeof(FfStage) == 32);
constexpr int kFfStages = 3;
struct FfDrawData {
    float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;  // D3DMATERIAL9 (FXLC-evaluated)
    float4 texture_factor;    // D3DRS_TEXTUREFACTOR as float4 (rgba 0..1)
    float4 params;            // x MaterialPower, y LIGHTING, z SPECULARENABLE, w COLORVERTEX
    uint4 sources;            // DIFFUSE/SPECULAR/AMBIENT/EMISSIVE MATERIALSOURCE (D3DMCS_*)
    float4 tex_transform[kFfStages][4];  // D3DTS_TEXTUREn as register rows (out[i] = dot(row i, tc))
    FfStage stage[kFfStages];
    uint4 tex;                // packed texture per stage (pack_texture), .w unused
    uint4 misc;               // x = number of active stages, y = vertex has colour (set by loader)
};
static_assert(offsetof(FfDrawData, texture_factor) == 64);
static_assert(offsetof(FfDrawData, sources) == 96);
static_assert(offsetof(FfDrawData, tex_transform) == 112);
static_assert(offsetof(FfDrawData, stage) == 304);
static_assert(offsetof(FfDrawData, tex) == 400);
static_assert(sizeof(FfDrawData) == 432);

// Generic D3D9 register files for translated SM1-3 shaders (PORTING.md 1, track B).
//   set 0 binding 0 : VSConstants { float4 c[256]; }   CTAB register order
//   set 0 binding 1 : PSConstants { float4 c[32];  }   ps_2_0 has 32, ps_1_x 8
//   set 1 binding 0..15 : combined image samplers s0..s15 (ps_1_x: sampler n = texture stage n)
//   push constant : PushConstants
struct VSConstants { float4 c[256]; };
struct PSConstants { float4 c[32]; };
static_assert(sizeof(VSConstants) == 4096 && sizeof(PSConstants) == 512);

}  // namespace alamo::materials

// CPU reference model of the Direct3D 9 fixed-function pipeline for the effects checked in C4b.
// Written from the D3D9 documentation semantics and hand-transcribed from the shipped FXO
// listings (docs/shaders/shaders_decompiled.zip effect.fx.txt + CATALOG.md expressions), so it
// is independent of engine/materials' state resolution and of the FF ubershader: a mismatch
// against the GPU is a bug in one of them.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace alamo::reftest {

using Vec4 = std::array<double, 4>;

// D3D9 numeric values (d3d9types.h), duplicated here on purpose.
namespace d9 {
enum : int { kDisable = 1, kSelectArg1 = 2, kSelectArg2 = 3, kModulate = 4, kModulate2x = 5, kModulate4x = 6, kAdd = 7 };
enum : int { kDiffuse = 0, kCurrent = 1, kTexture = 2, kTFactor = 3 };
enum : int { kBlendZero = 1, kBlendOne = 2, kSrcColor = 3, kInvSrcColor = 4, kSrcAlpha = 5, kInvSrcAlpha = 6,
             kDestColor = 9 };
enum : int { kMcsMaterial = 0, kMcsColor1 = 1, kMcsColor2 = 2 };
}  // namespace d9

struct RefStage {
    int color_op = d9::kDisable, color_arg1 = d9::kTexture, color_arg2 = d9::kCurrent;
    int alpha_op = d9::kDisable, alpha_arg1 = d9::kTexture, alpha_arg2 = d9::kCurrent;
    bool camera_space_position = false;  // TEXCOORDINDEX = D3DTSS_TCI_CAMERASPACEPOSITION
};

// One effect's best technique, transcribed from the FXO listing.
struct RefEffect {
    std::string name;
    bool shader_additive = false;   // MeshAdditive: DX8 vs/ps (tex * (LIGHT_SCALE.rgb * Color * LIGHT_SCALE.w, 1))
    bool lighting = false;
    bool color_vertex = true;
    int diffuse_source = d9::kMcsColor1;  // D3D9 defaults
    int ambient_source = d9::kMcsMaterial;
    int emissive_source = d9::kMcsMaterial;
    std::vector<RefStage> stages;   // up to the first DISABLE
    bool blend = false;
    int src_blend = d9::kBlendOne, dst_blend = d9::kBlendZero;
    bool z_write = true;
    bool vertex_color = false;      // mesh used for the test carries vertex colour
    bool scroll = false;            // MeshAlphaScroll texture transform on stage 0
    bool scanlines = false;         // PrimAlphaScanlines stage-1 camera-space transform
};
std::vector<RefEffect> reference_effects();

// Per-draw inputs (material parameters, engine values, lights).
struct RefParams {
    Vec4 diffuse{1, 1, 1, 1}, emissive{0, 0, 0, 0}, specular{1, 1, 1, 0}, color{1, 1, 1, 0};
    double uv_scroll[2] = {0, 0};
    double scanline_scale = 5.0, scanline_speed = 0.4;
    Vec4 light_scale{1, 1, 1, 1};
    double time = 0;
};
struct RefLights {
    double dir[3][3] = {};      // towards the light, normalised
    double diffuse[3][3] = {};  // colour x intensity
    double ambient[3] = {};     // scene ambient (light 0's D3DLIGHT ambient; D3DRS_AMBIENT = 0)
};

// The material colours / TFACTOR the FXO expressions produce for these parameters.
struct RefMaterial {
    Vec4 diffuse{0, 0, 0, 0}, ambient{0, 0, 0, 0}, emissive{0, 0, 0, 0};
};
RefMaterial reference_material(const RefEffect& e, const RefParams& p);

// Texture coordinates of the stages for a point on the mesh.
void reference_texcoords(const RefEffect& e, const RefParams& p, const double uv0[2], const double world[3],
                         double out_uv[2][2]);

// Final framebuffer value (0..1, before quantisation) of one pixel.
//   tex[s]: the texel sampled by stage s; vcolor: vertex colour (if the mesh has one);
//   normal: world normal; dst: framebuffer value before the draw.
Vec4 reference_pixel(const RefEffect& e, const RefParams& p, const RefLights& l, const Vec4 tex[2], const Vec4& vcolor,
                     const double normal[3], const Vec4& dst);

}  // namespace alamo::reftest

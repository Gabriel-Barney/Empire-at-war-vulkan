// alamo_render - scene lighting per docs/re/RENDER.md 2.2 / 2.3:
//   * one directional "sun" (light 0) -> DIR_LIGHT_VEC_0 (world, towards the light),
//     DIR_LIGHT_DIFFUSE_0 = colour x intensity, DIR_LIGHT_SPECULAR_0 = specular x 2 x intensity;
//   * two fill lights + ambient folded into 3rd-order spherical harmonics:
//     SPH_LIGHT_FILL = ambient + fills 1,2; SPH_LIGHT_ALL = fills + sun;
//   * space environments: sun direction from yaw a / elevation e = (cos e sin a, -cos e cos a, sin e)
//     and fill lights forced off.
// SH -> irradiance matrices: Ramamoorthi & Hanrahan 2001, E(n) = n1^T M n1 with n1 = (n, 1), one
// symmetric 4x4 per colour channel. A directional light of colour C contributes L_lm = C Y_lm(d)
// (so E(n) approximates C max(n.d, 0)); ambient adds directly to M[3][3] (E = ambient everywhere).
// The matrices are stored as the D3D9 register image (12 float4: R rows 0..3, G 4..7, B 8..11),
// which is what common.hlsli SphIrradiance() reads (the matrices are symmetric, so rows == columns).
#pragma once
#include <array>
#include <string>

#include "alamo/assets/math.hpp"
#include "alamo/materials/constants.hpp"

namespace alamo::terrain {
struct Environment;
}

namespace alamo::render {

struct DirLight {
    assets::Float3 direction{0, 0, 1};  // towards the light (normalised on use)
    assets::Float3 color{1, 1, 1};
    assets::Float3 specular{1, 1, 1};
    float intensity = 1.0f;
};

using Environment = terrain::Environment;  // TEE record (engine/terrain ted.hpp)

struct Lighting {
    DirLight sun;                       // light 0
    DirLight fill[2];                   // lights 1, 2 (intensity 0 in space)
    assets::Float3 ambient{0, 0, 0};
    // Scene post / clear values that come with an environment (TERRAIN_WATER_POST.md 5.2).
    assets::Float3 clear_color{0.3f, 0.3f, 0.7f};
    assets::Float3 shadow_color{0.5f, 0.5f, 0.5f};
    float bloom_strength = 1.0f, bloom_cutoff = 0.9f, bloom_size = 0.25f;
    std::string environment;            // name of the source record ("" = built-in)
    bool space = false;                 // space mode (the background scene: fills off, no stencil shadows; SPACE_AND_OPEN A)
    // Scene fog (TEE 0F/10/11/12; linear vertex fog, FOG_VALS = (1/(start-end), -end/(start-end))).
    bool fog_enabled = false;
    assets::Float3 fog_color{1, 1, 1};
    float fog_start = 1000.0f, fog_end = 10000.0f;
    // Background objects (TEE 19/1A object type names, 1B/1C scales, 1D..20 angles in degrees).
    std::string skydome[2];
    float skydome_scale[2] = {1, 1};
    float skydome_angle[2][2] = {{0, 0}, {0, 0}};

    // Built-in defaults from TERRAIN_WATER_POST.md 5.4 (real first environment records):
    //   space_default(): _MP_SPACE_KUAT "Environment 1" (sun (0.918,1,0.988) x 0.646 at yaw 217.1,
    //                    elev 12.5 deg, specular (0.455,0.667,0.796), ambient (0.027,0.051,0.055), fills off)
    //   land_default():  _MP_LAND_ENDOR "Sunrise_Clear" with its two fills.
    //   engine_default(): the Environment constructor record (scene without environments).
    static Lighting space_default();
    static Lighting land_default();
    static Lighting engine_default();
    // From a decoded TEE record; space = true zeroes the fill intensities (RENDER.md 1.2).
    static Lighting from_environment(const Environment& e, bool space);
    // Set the sun from the space-environment angles (radians).
    void set_sun_angles(float yaw, float elevation);
};
// Direction towards the light for yaw a / elevation e (radians): (cos e sin a, -cos e cos a, sin e).
assets::Float3 light_direction(float yaw, float elevation);

using SphMatrices = std::array<materials::float4, 12>;
// Irradiance matrices of `lights` (directional, colour x intensity each) plus `ambient`.
SphMatrices sph_matrices(const DirLight* lights, int count, const assets::Float3& ambient);
// Evaluates E(n) from the register image, as the shader does (tests).
assets::Float3 sph_irradiance(const SphMatrices& m, const assets::Float3& n);

struct SceneLightConstants {
    SphMatrices sph_fill{}, sph_all{};
};
SceneLightConstants scene_light_constants(const Lighting& l);
// Lights 0..2 (DIR_LIGHT_*_0..2: sun + the two fills, zero when a fill is off) and ambient of FrameCB.
void fill_frame_lights(const Lighting& l, materials::FrameCB& cb);
// Model transform of background object k (0/1): uniform scale (TEE 1B/1C), rotation about Z by the first
// angle and about X by the second (TEE 1D/1F, 1E/20, degrees). The renderer adds the camera position.
assets::Mat3x4 skydome_transform(const Lighting& l, int k);
// The one scene fog state every draw path uses (hot set, FF, translated, terrain / post). Returns
// l.fog_enabled; x / y = FOG_VALS = (1/(start-end), -end/(start-end)), or the RENDER.md 2.2 default
// (-0.0002, 3) when fog is off. Degenerate range (start == end, e.g. a TEE record with both 0): fog is
// still on, as a STEP at that distance - clear up to end - w, fully fogged from `end` on, w =
// max(kFogStepWidth, 1e-5 |end|) world units (the original's 1/(start-end) would be infinite).
inline constexpr float kFogStepWidth = 1e-3f;
bool scene_fog_vals(const Lighting& l, float& x, float& y);
// Scene fog into FrameCB when enabled: FOG_VALS (scene_fog_vals) and FogColor.
void apply_scene_fog(const Lighting& l, materials::FrameCB& cb);

}  // namespace alamo::render

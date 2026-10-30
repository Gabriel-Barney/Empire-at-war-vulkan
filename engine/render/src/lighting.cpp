#include "alamo/render/lighting.hpp"

#include "alamo/render/ted.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::render {

namespace {
assets::Float3 norm(const assets::Float3& v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0 ? assets::Float3{v.x / l, v.y / l, v.z / l} : assets::Float3{0, 0, 1};
}
}  // namespace

namespace {
constexpr float kDeg = 0.017453293f;
}

assets::Float3 light_direction(float yaw, float e) {
    return {std::cos(e) * std::sin(yaw), -std::cos(e) * std::cos(yaw), std::sin(e)};
}

Lighting Lighting::from_environment(const Environment& e, bool space) {
    Lighting l;
    DirLight* lights[3] = {&l.sun, &l.fill[0], &l.fill[1]};
    for (int i = 0; i < 3; ++i) {
        lights[i]->color = {e.light_color[i][0], e.light_color[i][1], e.light_color[i][2]};
        lights[i]->intensity = e.intensity[i];
        lights[i]->direction = light_direction(e.yaw[i], e.elevation[i]);
        lights[i]->specular = {0, 0, 0};  // fills get specular 0 (TERRAIN_WATER_POST.md 5.3)
    }
    l.sun.specular = {e.sun_specular[0], e.sun_specular[1], e.sun_specular[2]};
    if (space) l.fill[0].intensity = l.fill[1].intensity = 0.0f;  // RENDER.md 1.2
    l.ambient = {e.ambient[0], e.ambient[1], e.ambient[2]};
    l.clear_color = {e.clear_color[0], e.clear_color[1], e.clear_color[2]};
    l.shadow_color = {e.shadow_color[0], e.shadow_color[1], e.shadow_color[2]};
    l.bloom_strength = e.bloom_strength;
    l.bloom_cutoff = e.bloom_cutoff;
    l.bloom_size = e.bloom_size;
    l.environment = e.name;
    l.space = space;
    l.fog_enabled = e.fog_enabled && !space;
    l.fog_color = {e.fog_color[0], e.fog_color[1], e.fog_color[2]};
    l.fog_start = e.fog_start;
    l.fog_end = e.fog_end;
    for (int k = 0; k < 2; ++k) {
        l.skydome[k] = e.skydome[k];
        l.skydome_scale[k] = e.skydome_scale[k];
        l.skydome_angle[k][0] = e.skydome_angle_deg[k][0];
        l.skydome_angle[k][1] = e.skydome_angle_deg[k][1];
    }
    return l;
}

Lighting Lighting::engine_default() {
    Environment e;  // constructor values
    e.name = "engine default";
    return from_environment(e, false);
}

Lighting Lighting::space_default() {
    Environment e;  // _MP_SPACE_KUAT env 0 (TERRAIN_WATER_POST.md 5.4)
    e.name = "Kuat (built-in)";
    e.light_color[0][0] = 0.918f;
    e.light_color[0][1] = 1.000f;
    e.light_color[0][2] = 0.988f;
    e.intensity[0] = 0.646f;
    e.yaw[0] = 217.1f * kDeg;
    e.elevation[0] = 12.5f * kDeg;
    e.sun_specular[0] = 0.455f;
    e.sun_specular[1] = 0.667f;
    e.sun_specular[2] = 0.796f;
    e.ambient[0] = 0.027f;
    e.ambient[1] = 0.051f;
    e.ambient[2] = 0.055f;
    e.bloom_strength = 1.0f;
    e.bloom_cutoff = 0.9f;
    e.bloom_size = 0.25f;
    e.skydome[0] = "STARS_LOW";  // Kuat sky objects (5.4); the second backdrop is rotated by 210 degrees
    e.skydome[1] = "STAR_BACKDROP_GREEN_ORANGE_2";
    e.skydome_angle_deg[1][1] = 210.0f;  // TEE 20
    return from_environment(e, true);
}

Lighting Lighting::land_default() {
    Environment e;  // _MP_LAND_ENDOR "Sunrise_Clear" (TERRAIN_WATER_POST.md 5.4)
    e.name = "Endor Sunrise_Clear (built-in)";
    const float sun[3] = {1.000f, 0.729f, 0.357f}, f1[3] = {0.745f, 0.733f, 0.851f}, f2[3] = {0.851f, 0.808f, 0.733f};
    for (int k = 0; k < 3; ++k) {
        e.light_color[0][k] = sun[k];
        e.light_color[1][k] = f1[k];
        e.light_color[2][k] = f2[k];
    }
    e.intensity[0] = 0.646f;
    e.intensity[1] = 0.33f;
    e.intensity[2] = 0.5f;
    e.yaw[0] = 220.0f * kDeg;
    e.elevation[0] = 33.0f * kDeg;
    e.yaw[1] = 349.0f * kDeg;
    e.elevation[1] = -18.0f * kDeg;
    e.yaw[2] = 85.0f * kDeg;
    e.elevation[2] = -42.0f * kDeg;
    e.ambient[0] = 0.145f;
    e.ambient[1] = 0.184f;
    e.ambient[2] = 0.188f;
    e.bloom_size = 1.0f;
    e.fog_enabled = true;  // Endor Sunrise_Clear fog 0..3500, colour (0.729,0.812,0.722)
    e.fog_start = 0.0f;
    e.fog_end = 3500.0f;
    e.fog_color[0] = 0.729f;
    e.fog_color[1] = 0.812f;
    e.fog_color[2] = 0.722f;
    e.skydome[0] = "DAY_STORM_SKY";
    return from_environment(e, false);
}

void Lighting::set_sun_angles(float yaw, float e) { sun.direction = light_direction(yaw, e); }

SphMatrices sph_matrices(const DirLight* lights, int count, const assets::Float3& ambient) {
    // SH coefficients L_lm per channel (9 x 3).
    float L[9][3] = {};
    for (int i = 0; i < count; ++i) {
        const DirLight& li = lights[i];
        if (li.intensity <= 0.0f) continue;
        const assets::Float3 d = norm(li.direction);
        const float y[9] = {0.282095f,
                            0.488603f * d.y,
                            0.488603f * d.z,
                            0.488603f * d.x,
                            1.092548f * d.x * d.y,
                            1.092548f * d.y * d.z,
                            0.315392f * (3.0f * d.z * d.z - 1.0f),
                            1.092548f * d.x * d.z,
                            0.546274f * (d.x * d.x - d.y * d.y)};
        const float c[3] = {li.color.x * li.intensity, li.color.y * li.intensity, li.color.z * li.intensity};
        for (int k = 0; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch) L[k][ch] += c[ch] * y[k];
    }
    const float c1 = 0.429043f, c2 = 0.511664f, c3 = 0.743125f, c4 = 0.886227f, c5 = 0.247708f;
    const float amb[3] = {ambient.x, ambient.y, ambient.z};
    SphMatrices out{};
    for (int ch = 0; ch < 3; ++ch) {
        // index: 0 L00, 1 L1-1, 2 L10, 3 L11, 4 L2-2, 5 L2-1, 6 L20, 7 L21, 8 L22
        const float L00 = L[0][ch], L1m1 = L[1][ch], L10 = L[2][ch], L11 = L[3][ch], L2m2 = L[4][ch], L2m1 = L[5][ch],
                    L20 = L[6][ch], L21 = L[7][ch], L22 = L[8][ch];
        const float M[4][4] = {{c1 * L22, c1 * L2m2, c1 * L21, c2 * L11},
                               {c1 * L2m2, -c1 * L22, c1 * L2m1, c2 * L1m1},
                               {c1 * L21, c1 * L2m1, c3 * L20, c2 * L10},
                               {c2 * L11, c2 * L1m1, c2 * L10, c4 * L00 - c5 * L20 + amb[ch]}};
        for (int r = 0; r < 4; ++r) out[static_cast<size_t>(ch * 4 + r)] = {M[r][0], M[r][1], M[r][2], M[r][3]};
    }
    return out;
}

assets::Float3 sph_irradiance(const SphMatrices& m, const assets::Float3& n) {
    const float v[4] = {n.x, n.y, n.z, 1.0f};
    float r[3];
    for (int ch = 0; ch < 3; ++ch) {
        float acc = 0;
        for (int j = 0; j < 4; ++j) {
            const materials::float4& col = m[static_cast<size_t>(ch * 4 + j)];
            const float dot4 = col.x * v[0] + col.y * v[1] + col.z * v[2] + col.w * v[3];
            acc += dot4 * v[j];
        }
        r[ch] = acc;
    }
    return {r[0], r[1], r[2]};
}

SceneLightConstants scene_light_constants(const Lighting& l) {
    SceneLightConstants s;
    s.sph_fill = sph_matrices(l.fill, 2, l.ambient);
    const DirLight all[3] = {l.fill[0], l.fill[1], l.sun};
    s.sph_all = sph_matrices(all, 3, l.ambient);
    return s;
}

namespace {
// DIR_LIGHT_*_n (RENDER.md 2.2): vector towards the light (w 0), diffuse = colour x intensity (w 1),
// specular = specular x 2 x intensity (w 0). A light with intensity 0 (space fills) is all zero.
void light_constants(const DirLight& l, materials::float4& vec, materials::float4& dif, materials::float4& spec) {
    if (l.intensity <= 0.0f) {
        vec = dif = spec = {};
        return;
    }
    const assets::Float3 d = norm(l.direction);
    vec = {d.x, d.y, d.z, 0.0f};
    dif = {l.color.x * l.intensity, l.color.y * l.intensity, l.color.z * l.intensity, 1.0f};
    spec = {l.specular.x * 2.0f * l.intensity, l.specular.y * 2.0f * l.intensity, l.specular.z * 2.0f * l.intensity, 0.0f};
}
}  // namespace

void fill_frame_lights(const Lighting& l, materials::FrameCB& cb) {
    const assets::Float3 d = norm(l.sun.direction);
    cb.light0_vector = {d.x, d.y, d.z, 0.0f};
    cb.light0_diffuse = {l.sun.color.x * l.sun.intensity, l.sun.color.y * l.sun.intensity, l.sun.color.z * l.sun.intensity, 1.0f};
    cb.light0_specular = {l.sun.specular.x * 2.0f * l.sun.intensity, l.sun.specular.y * 2.0f * l.sun.intensity,
                          l.sun.specular.z * 2.0f * l.sun.intensity, 0.0f};
    light_constants(l.fill[0], cb.light1_vector, cb.light1_diffuse, cb.light1_specular);
    light_constants(l.fill[1], cb.light2_vector, cb.light2_diffuse, cb.light2_specular);
    cb.light_ambient = {l.ambient.x, l.ambient.y, l.ambient.z, 1.0f};
}

assets::Mat3x4 skydome_transform(const Lighting& l, int k) {
    const int i = k ? 1 : 0;
    // Second angle (TEE 1F / 20) = yaw about Z, first (1D / 1E) = tilt about X: the only angle set in vanilla
    // maps is the second one of Kuat's backdrop ("rotated by 210 degrees"), read as a yaw. [assumption]
    const float a = l.skydome_angle[i][1] * kDeg, b = l.skydome_angle[i][0] * kDeg, s = l.skydome_scale[i] > 0 ? l.skydome_scale[i] : 1.0f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    // R = Rz(a) * Rx(b), then scale.
    assets::Mat3x4 m;
    m.m[0][0] = ca * s;
    m.m[0][1] = -sa * cb * s;
    m.m[0][2] = sa * sb * s;
    m.m[1][0] = sa * s;
    m.m[1][1] = ca * cb * s;
    m.m[1][2] = -ca * sb * s;
    m.m[2][0] = 0.0f;
    m.m[2][1] = sb * s;
    m.m[2][2] = cb * s;
    return m;
}

bool scene_fog_vals(const Lighting& l, float& x, float& y) {
    x = -0.0002f;  // RENDER.md 2.2 default (start 10000, end 15000)
    y = 3.0f;
    if (!l.fog_enabled) return false;
    float start = l.fog_start;
    const float end = l.fog_end;
    if (start == end) start = end - std::max(kFogStepWidth, std::fabs(end) * 1e-5f);  // degenerate: a step at `end`
    x = 1.0f / (start - end);
    y = -end / (start - end);
    return true;
}

void apply_scene_fog(const Lighting& l, materials::FrameCB& cb) {
    if (!scene_fog_vals(l, cb.fog_vals_fade_vals.x, cb.fog_vals_fade_vals.y)) return;
    cb.fog_color = {l.fog_color.x, l.fog_color.y, l.fog_color.z, 1.0f};
}

}  // namespace alamo::render

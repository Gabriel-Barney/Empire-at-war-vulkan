// test_render: main and the tests that stayed (see test_render_common.hpp; the others are in test_render_*.cpp).
#include "test_render_common.hpp"

namespace test_render_t {

// ------------------------------------------------------------------------------------------- CPU

void test_formats() {
    using TF = assets::TextureFormat;
    CHECK(render::to_rhi_format(TF::BC1_RGBA) == rhi::Format::BC1);
    CHECK(render::to_rhi_format(TF::BC2) == rhi::Format::BC2);
    CHECK(render::to_rhi_format(TF::BC3) == rhi::Format::BC3);
    CHECK(render::to_rhi_format(TF::RGBA8) == rhi::Format::RGBA8);
    CHECK(render::to_rhi_format(TF::BGRA8) == rhi::Format::BGRA8);
    CHECK(render::to_rhi_format(TF::R8) == rhi::Format::R8);
    CHECK(render::to_rhi_format(TF::R8G8) == rhi::Format::R8G8);
    CHECK(render::to_rhi_format(TF::Unknown) == rhi::Format::Undefined);
    // The VkFormat names assets documents agree with what rhi creates.
    for (TF f : {TF::BC1_RGBA, TF::BC2, TF::BC3, TF::RGBA8, TF::BGRA8, TF::R8, TF::R8G8}) {
        const rhi::Format r = render::to_rhi_format(f);
        CHECK(rhi::to_vk(r) != VK_FORMAT_UNDEFINED);
        CHECK(render::to_rhi_format(f) != rhi::Format::Undefined);
        // Level sizes agree (assets' level math vs rhi's subresource math), incl. tiny BC mips.
        for (uint32_t w : {1u, 2u, 3u, 4u, 5u, 64u, 256u})
            CHECK(assets::texture_level_size(f, w, w) == rhi::subresource_size(r, w, w));
    }
    CHECK(std::string(assets::texture_format_vk_name(TF::BC1_RGBA)) == "VK_FORMAT_BC1_RGBA_UNORM_BLOCK");
    CHECK(rhi::to_vk(rhi::Format::BC1) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);

    using S = assets::Swizzle;
    rhi::Swizzle out = rhi::Swizzle::A8;
    CHECK(render::to_rhi_swizzle({}, out) && out == rhi::Swizzle::Identity);
    CHECK(render::to_rhi_swizzle({S::R, S::R, S::R, S::One}, out) && out == rhi::Swizzle::RRR1);   // L8
    CHECK(render::to_rhi_swizzle({S::Zero, S::Zero, S::Zero, S::R}, out) && out == rhi::Swizzle::A8);  // A8
    CHECK(render::to_rhi_swizzle({S::R, S::R, S::R, S::G}, out) && out == rhi::Swizzle::RRRG);   // A8L8
    CHECK(render::to_rhi_swizzle({S::R, S::G, S::B, S::One}, out) && out == rhi::Swizzle::X8);
    CHECK(!render::to_rhi_swizzle({S::B, S::G, S::R, S::A}, out) && out == rhi::Swizzle::Identity);
    // rhi's view mapping matches assets' documented mapping for each.
    const VkComponentMapping l8 = rhi::component_mapping(rhi::Swizzle::RRR1);
    CHECK(l8.r == VK_COMPONENT_SWIZZLE_R && l8.g == VK_COMPONENT_SWIZZLE_R && l8.b == VK_COMPONENT_SWIZZLE_R &&
          l8.a == VK_COMPONENT_SWIZZLE_ONE);
    const VkComponentMapping a8 = rhi::component_mapping(rhi::Swizzle::A8);
    CHECK(a8.r == VK_COMPONENT_SWIZZLE_ZERO && a8.a == VK_COMPONENT_SWIZZLE_R);
    const VkComponentMapping la = rhi::component_mapping(rhi::Swizzle::RRRG);
    CHECK(la.r == VK_COMPONENT_SWIZZLE_R && la.a == VK_COMPONENT_SWIZZLE_G);
}

void test_layouts() {
    using ML = materials::VertexLayout;
    struct Case {
        const char* name;
        ML expect;
        bool skinned;
    };
    // The 10 vertex formats of assets model.hpp.
    const Case cases[] = {
        {"alD3dVertN", ML::N, false},
        {"alD3dVertNU2", ML::NU2, false},
        {"alD3dVertNU2C", ML::NU2C, false},
        {"alD3dVertNU2U3U3", ML::NU2U3U3, false},
        {"alD3dVertNU2U3U3C", ML::NU2U3U3C, false},
        {"alD3dVertGrass", ML::NU2, false},
        {"alD3dVertRSkinNU2", ML::RSkinNU2, true},
        {"alD3dVertRSkinNU2U3U3", ML::RSkinNU2U3U3, true},
        {"alD3dVertB4I4NU2", ML::RSkinNU2, true},
        {"alD3dVertB4I4NU2U3U3", ML::RSkinNU2U3U3, true},
    };
    for (const Case& c : cases) {
        const ML l = render::vertex_layout_for(c.name);
        if (l != c.expect) std::printf("  layout %s -> %s\n", c.name, materials::vertex_layout_name(l));
        CHECK(l == c.expect);
        CHECK(materials::vertex_layout_skinned(l) == c.skinned);
        CHECK(render::assets_layout_for(l) == (c.skinned ? assets::VertexLayout::Skinned : assets::VertexLayout::Static));
        CHECK(render::vertex_tables_agree(l));
        // Lower-case spelling is accepted too.
        CHECK(render::vertex_layout_for(alamo::ascii_lower(c.name)) == c.expect);
        render::VertexInputDesc vin;
        CHECK(render::build_vertex_input(l, {}, vin));
        CHECK(vin.binding.stride == 64 && vin.attributes.size() == 6);
    }
    // Traits path (unknown name) and the materials-only RSkinNU2C.
    CHECK(render::vertex_layout_for("alD3dVertRSkinNU2C") == ML::RSkinNU2C);
    assets::VertexFormatTraits t;
    t.known = false;
    t.uv_sets = 1;
    t.tangent_frame = true;
    CHECK(render::vertex_layout_for("alD3dVertSomethingNew", t) == ML::NU2U3U3);
    CHECK(render::vertex_layout_for("garbage", assets::VertexFormatTraits{}) == ML::Unknown);
    // Shader input subset: POSITION, TEXCOORD0, COLOR0 of the static layout.
    const uint32_t locs[] = {0, 2, 5};
    render::VertexInputDesc vin;
    CHECK(render::build_vertex_input(ML::NU2, locs, vin) && vin.attributes.size() == 3);
    CHECK(vin.attributes[2].location == 5 && vin.attributes[2].offset == 56 && vin.attributes[2].format == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(render::build_vertex_input(ML::RSkinNU2, locs, vin) && vin.attributes[2].offset == 60);
    CHECK(!render::vertex_tables_agree(ML::PrimVert) && !render::vertex_tables_agree(ML::Unknown));
}

void test_samplers() {
    materials::SamplerKey k;  // linear/linear/linear wrap
    rhi::SamplerKey r = render::to_rhi_sampler(k);
    CHECK(r == rhi::SamplerKey::linear_wrap());
    k.u = k.v = k.w = materials::AddressMode::ClampToEdge;
    CHECK(render::to_rhi_sampler(k) == rhi::SamplerKey::linear_clamp());
    k.mag = k.min = materials::Filter::Nearest;
    k.mip = materials::MipmapMode::Nearest;
    CHECK(render::to_rhi_sampler(k) == rhi::SamplerKey::point_clamp());
    materials::SamplerKey m;
    m.mip_enable = 0;
    m.max_mip_level = 2;
    m.max_anisotropy = 16;
    m.u = materials::AddressMode::MirroredRepeat;
    m.v = materials::AddressMode::ClampToBorder;
    m.border_color = 0xFFFFFFFFu;
    const float bias = -0.5f;
    std::memcpy(&m.mip_lod_bias_bits, &bias, 4);
    r = render::to_rhi_sampler(m, 8);
    CHECK(r.max_anisotropy == 8);
    CHECK(near_eq(r.min_lod, 2.0f) && near_eq(r.max_lod, 2.0f));
    CHECK(r.mip_mode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
    CHECK(r.address_u == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT && r.address_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    CHECK(r.border == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
    CHECK(near_eq(r.lod_bias, -0.5f));
    m.border_color = 0x00FFFFFFu;
    CHECK(render::to_rhi_sampler(m).border == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    m.border_color = 0xFF000000u;
    CHECK(render::to_rhi_sampler(m).border == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
    m.max_anisotropy = 1;
    CHECK(render::to_rhi_sampler(m).max_anisotropy == 0);
    m.u = materials::AddressMode::MirrorClampToEdge;
    CHECK(render::to_rhi_sampler(m).address_u == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);

    // [port] Renderer-wide anisotropy override: max(key, override) for linear minification, capped.
    materials::SamplerKey lin;  // plain trilinear, max_anisotropy 1
    CHECK(render::to_rhi_sampler(lin, 16, 0) == render::to_rhi_sampler(lin));  // off = today's key
    CHECK(render::to_rhi_sampler(lin, 16, 1).max_anisotropy == 0);
    CHECK(render::to_rhi_sampler(lin, 16, 4).max_anisotropy == 4);
    CHECK(render::to_rhi_sampler(lin, 16, 16).max_anisotropy == 16);
    CHECK(render::to_rhi_sampler(lin, 8, 16).max_anisotropy == 8);  // device cap wins
    CHECK(render::to_rhi_sampler(lin, 1, 16).max_anisotropy == 0);  // no samplerAnisotropy
    {
        rhi::SamplerKey a = render::to_rhi_sampler(lin, 16, 8), b = render::to_rhi_sampler(lin);
        b.max_anisotropy = 8;
        CHECK(a == b);  // only the anisotropy changes
    }
    lin.max_anisotropy = 8;  // the material asks for more than the setting: kept
    CHECK(render::to_rhi_sampler(lin, 16, 2).max_anisotropy == 8);
    CHECK(render::to_rhi_sampler(lin, 16, 16).max_anisotropy == 16);
    materials::SamplerKey pt;  // point minification (Tree): untouched, even with linear magnification
    pt.min = materials::Filter::Nearest;
    CHECK(render::to_rhi_sampler(pt, 16, 16).max_anisotropy == 0);
    CHECK(render::to_rhi_sampler(pt, 16, 16) == render::to_rhi_sampler(pt));
    pt.mag = materials::Filter::Nearest;
    CHECK(render::to_rhi_sampler(pt, 16, 16) == render::to_rhi_sampler(pt));
    pt.max_anisotropy = 4;  // an explicit anisotropic key stays as the material set it
    CHECK(render::to_rhi_sampler(pt, 16, 16).max_anisotropy == 4);
    CHECK(render::anisotropy_level(0, 16) == 0 && render::anisotropy_level(1, 16) == 0);
    CHECK(render::anisotropy_level(2, 16) == 2 && render::anisotropy_level(16, 16) == 16);
    CHECK(render::anisotropy_level(64, 16) == 16 && render::anisotropy_level(16, 8) == 8);
    {   // [port] Texture detail: bits 8..15 of the override skip top mip levels (minLod) of linearly minified, mip-mapped keys.
        materials::SamplerKey tri;
        CHECK(render::to_rhi_sampler(tri, 16, 0 | (0u << 8)) == render::to_rhi_sampler(tri));
        CHECK(render::to_rhi_sampler(tri, 16, 2u << 8).min_lod == 2.0f && render::to_rhi_sampler(tri, 16, 2u << 8).max_anisotropy == 0);
        CHECK(render::to_rhi_sampler(tri, 16, 4u | (3u << 8)).min_lod == 3.0f && render::to_rhi_sampler(tri, 16, 4u | (3u << 8)).max_anisotropy == 4);
        materials::SamplerKey base = tri;
        base.max_mip_level = 5;  // the material already starts lower: kept
        CHECK(render::to_rhi_sampler(base, 16, 2u << 8).min_lod == 5.0f);
        materials::SamplerKey nomip = tri;
        nomip.mip_enable = 0;  // level 0 only: nothing to skip
        CHECK(render::to_rhi_sampler(nomip, 16, 3u << 8) == render::to_rhi_sampler(nomip));
        materials::SamplerKey nearest = tri;
        nearest.min = materials::Filter::Nearest;  // point minification: lookups and UI keep level 0
        CHECK(render::to_rhi_sampler(nearest, 16, 3u << 8) == render::to_rhi_sampler(nearest));
    }
    CHECK(render::anisotropy_level(16, 1) == 0 && render::anisotropy_level(4, 0) == 0);
}

void test_camera() {
    // Perspective: depth 0 at near, 1 at far (D3DXMatrixPerspectiveFovRH, camera looks down -Z).
    const render::Mat4 p = render::perspective_rh(1.0f, 1.5f, 2.0f, 500.0f);
    CHECK(near_eq(project(p, {0, 0, -2}).z, 0.0f));
    CHECK(near_eq(project(p, {0, 0, -500}).z, 1.0f, 1e-3f));
    CHECK(project(p, {0, 0, -50}).z > 0.0f && project(p, {0, 0, -50}).z < 1.0f);
    CHECK(project(p, {0, 0, -10}).w > 0.0f);  // in front -> positive w
    // Vertical FOV 1 rad: a point at angle 0.5 above the axis lands on y = +1 (clip Y up).
    CHECK(near_eq(project(p, {0, std::tan(0.5f) * 10.0f, -10}).y, 1.0f));
    // Horizontal FOV -> vertical: x edge at aspect.
    const float fx = 1.2f, aspect = 16.0f / 9.0f;
    const float fy = render::fov_y_from_horizontal(fx, aspect);
    CHECK(near_eq(std::tan(fy * 0.5f) * aspect, std::tan(fx * 0.5f)));
    const render::Mat4 p2 = render::perspective_rh(fy, aspect, 1.0f, 100.0f);
    CHECK(near_eq(project(p2, {std::tan(fx * 0.5f) * 10.0f, 0, -10}).x, 1.0f));

    // look_at: Z up, right-handed. Camera on -Y looking at the origin: +X is right, +Z is up.
    const render::Mat4 v = render::look_at_rh({0, -10, 0}, {0, 0, 0});
    const render::Float4 o = v * render::Float4{0, 0, 0, 1};
    CHECK(near_eq(o.x, 0) && near_eq(o.y, 0) && near_eq(o.z, -10.0f));  // 10 units in front (-Z)
    const render::Float4 rx = v * render::Float4{1, 0, 0, 1};
    CHECK(near_eq(rx.x, 1.0f));
    const render::Float4 uz = v * render::Float4{0, 0, 1, 1};
    CHECK(near_eq(uz.y, 1.0f));
    const render::Mat4 vi = v.inverse_affine();
    const render::Float4 e = vi * render::Float4{0, 0, 0, 1};
    CHECK(near_eq(e.x, 0) && near_eq(e.y, -10.0f) && near_eq(e.z, 0));

    // Orbit camera framing a sphere keeps it inside the frustum, and make_view composes VP = P * V.
    render::OrbitCamera cam;
    cam.frame({10, 20, 30}, 50.0f, aspect);
    const render::ViewMatrices vm = render::make_view(cam, 1600, 900);
    const render::Float4 c = project(vm.view_proj, {10, 20, 30});
    CHECK(near_eq(c.x, 0.0f) && near_eq(c.y, 0.0f) && c.z > 0.0f && c.z < 1.0f);
    float zn = 0, zf = 0;
    cam.clip_planes(zn, zf);
    CHECK(zn > 0 && zn < cam.distance - 50.0f && zf > cam.distance + 50.0f);
    // The sphere's top (Z up) projects inside +y; its horizontal extremes inside |x| <= 1.
    const Float3 eye = cam.eye();
    CHECK(eye.z > 30.0f);  // positive pitch: above the target
    const render::Float4 top = project(vm.view_proj, {10, 20, 80});
    CHECK(top.y > 0.0f && top.y <= 1.0f);
    materials::FrameCB cb;
    render::fill_frame_cb(vm, 1.5f, cb);
    CHECK(near_eq(cb.view_proj[3].z, vm.view_proj.m[3][2]) && near_eq(cb.time_pad.x, 1.5f));
    CHECK(near_eq(cb.resolution_constants.x, 1600.0f) && near_eq(cb.resolution_constants.w, 0.5f / 900.0f));

    {   // Background camera (SPACE_AND_OPEN.md D.7 2) = the main camera except near 10 / far 100000: same view,
        // eye, view_inv and FOV terms (bit for bit), only the depth terms differ.
        render::OrbitCamera sc;
        sc.target = {241.0f, -57.0f, 0.0f};
        sc.yaw = -1.5707963f;
        sc.pitch = 50.0f * 0.017453293f;
        sc.distance = 1000.0f;
        sc.fov_x = render::tactical_fov_x(55.0f, 16.0f / 9.0f);
        const render::ViewMatrices mv = render::make_view(sc, 1280, 720, 10.0f, 7000.0f);
        const render::ViewMatrices bv = render::background_view(mv);
        CHECK(std::memcmp(&bv.view, &mv.view, sizeof mv.view) == 0 && std::memcmp(&bv.view_inv, &mv.view_inv, sizeof mv.view_inv) == 0);
        CHECK(bv.eye.x == mv.eye.x && bv.eye.y == mv.eye.y && bv.eye.z == mv.eye.z && bv.width == mv.width && bv.height == mv.height);
        CHECK(bv.proj.m[0][0] == mv.proj.m[0][0] && bv.proj.m[1][1] == mv.proj.m[1][1] && bv.proj.m[0][2] == mv.proj.m[0][2] &&
              bv.proj.m[1][2] == mv.proj.m[1][2]);
        const render::ViewMatrices ref = render::make_view(sc, 1280, 720, render::kBackgroundNear, render::kBackgroundFar);
        CHECK(near_eq(bv.proj.m[2][2], ref.proj.m[2][2]) && near_eq(bv.proj.m[2][3], ref.proj.m[2][3]));
        // A point 50000 ahead: outside the main frustum (far 7000), inside the background one; same x / y in both.
        const Float3 fwd{mv.view.m[2][0] * -1.0f, mv.view.m[2][1] * -1.0f, mv.view.m[2][2] * -1.0f};
        const Float3 far_pt{mv.eye.x + fwd.x * 50000.0f, mv.eye.y + fwd.y * 50000.0f, mv.eye.z + fwd.z * 50000.0f};
        const render::Float4 pm = project(mv.view_proj, far_pt), pb = project(bv.view_proj, far_pt);
        CHECK(pm.z > 1.0f && pb.z > 0.0f && pb.z < 1.0f && near_eq(pm.x, pb.x) && near_eq(pm.y, pb.y));
        // Default Space_Mode pose (D.7 2): eye = look-at + 1000 x (0, -0.643, 0.766).
        CHECK(std::fabs(mv.eye.x - 241.0f) < 0.1f && std::fabs(mv.eye.y - (-57.0f - 642.8f)) < 0.5f && std::fabs(mv.eye.z - 766.0f) < 0.5f);
    }
}

// [port] SSAO depth reconstruction (ssao.hpp, the CPU mirror of ssao.hlsl's ViewPos): view point -> uv + depth ->
// view point round trip for a perspective projection (normal and reversed depth, sub-pixel offset) and an orthographic one.
// [port: Gabriel] Render scale is a share of the pixels: each side x sqrt(scale).
void test_render_scale_extent() {
    const VkExtent2D uhd{3840, 2160};
    const VkExtent2D s75 = render::render_scale_extent(uhd, 0.75f);
    CHECK(s75.width == 3326 && s75.height == 1871);
    const VkExtent2D s50 = render::render_scale_extent(uhd, 0.5f);
    CHECK(s50.width == 2715 && s50.height == 1527);
    const VkExtent2D same = render::render_scale_extent(uhd, 1.0f);
    CHECK(same.width == 3840 && same.height == 2160);
    const VkExtent2D above = render::render_scale_extent(uhd, 2.0f);  // never above native
    CHECK(above.width == 3840 && above.height == 2160);
    const VkExtent2D low = render::render_scale_extent(uhd, 0.01f);  // clamped to kRenderScaleMin: half each side
    CHECK(low.width == 1920 && low.height == 1080);
    const VkExtent2D fhd = render::render_scale_extent({1920, 1080}, 0.75f);
    CHECK(fhd.width == 1663 && fhd.height == 935);
    const VkExtent2D tiny = render::render_scale_extent({1, 1}, 0.25f);
    CHECK(tiny.width == 1 && tiny.height == 1);
}

void test_ssao_math() {
    render::Mat4 persp = render::perspective_rh(0.8f, 16.0f / 9.0f, 10.0f, 7000.0f);
    render::Mat4 jitter = persp;
    jitter.m[0][2] = 0.0007f;  // TAA-style sub-pixel offsets
    jitter.m[1][2] = -0.0011f;
    render::Mat4 reversed = persp;  // depth 1 at near, 0 at far
    reversed.m[2][2] = -persp.m[2][2] - 1.0f;
    reversed.m[2][3] = -persp.m[2][3];
    render::Mat4 ortho;
    ortho.m[0][0] = 0.01f;
    ortho.m[1][1] = 0.02f;
    ortho.m[2][2] = -1.0f / 990.0f;
    ortho.m[2][3] = -10.0f / 990.0f;
    const Float3 pts[4] = {{0, 0, -10.5f}, {12, -7, -100}, {-300, 210, -1500}, {2000, 900, -6500}};
    for (const render::Mat4* m : {&persp, &jitter, &reversed, &ortho}) {
        const render::SsaoProjection k = render::ssao_projection(*m);
        for (const Float3& p : pts) {
            if (m == &ortho && p.z < -1000.0f) continue;
            float u = 0, v = 0, d = 0;
            render::ssao_project(k, p, u, v, d);
            const Float3 q = render::ssao_view_position(k, u, v, d);
            const float tol = 1e-3f * std::max(1.0f, -p.z);
            CHECK(std::fabs(q.x - p.x) <= tol && std::fabs(q.y - p.y) <= tol && std::fabs(q.z - p.z) <= tol);
            CHECK(d >= 0.0f && d <= 1.0f);
            // Same depth and NDC as the projection matrix itself (y down in uv).
            const render::Float4 c = *m * render::Float4{p.x, p.y, p.z, 1.0f};
            CHECK(near_eq(d, c.z / c.w, 1e-5f) && near_eq(u, 0.5f + 0.5f * c.x / c.w, 1e-5f) && near_eq(v, 0.5f - 0.5f * c.y / c.w, 1e-5f));
        }
    }
    // Normal depth: near plane at 0, far at 1; reversed: near at 1.
    const render::SsaoProjection kp = render::ssao_projection(persp), kr = render::ssao_projection(reversed);
    CHECK(near_eq(render::ssao_view_position(kp, 0.5f, 0.5f, 0.0f).z, -10.0f, 1e-3f));
    CHECK(near_eq(render::ssao_view_position(kp, 0.5f, 0.5f, 1.0f).z, -7000.0f, 1.0f));
    CHECK(near_eq(render::ssao_view_position(kr, 0.5f, 0.5f, 1.0f).z, -10.0f, 1e-3f));
    // Screen corner uv (0, 0) = top-left = (-x, +y) in view space.
    const Float3 tl = render::ssao_view_position(kp, 0.0f, 0.0f, 0.5f);
    CHECK(tl.x < 0.0f && tl.y > 0.0f);
}

// Planet spin (SPACE_AND_OPEN.md D.7 4): the planets' _IDLE_00 clip turns bone Hoth +360 deg about local Z over
// 240 frames at fps 1.0; clip time = ticks / 30 x Idle_Anim_00_Rate_Mod. This checks the time base with
// assets::sample's generic loop (period = frame count). R3: MapScene itself now wraps at count - 1 like the original
// (ANIM_PARTICLES.md A.9: one revolution per 239 s); that rule is tested in engine/anim (alamo_anim_tests).
void test_idle_spin() {
    CHECK(render::MapScene::idle_anim_seconds(1800, 1.0f) == 60.0f);   // 60 s of 30 Hz ticks
    CHECK(render::MapScene::idle_anim_seconds(1800, 0.25f) == 15.0f);  // Coruscant_Backdrop rate 0.25
    CHECK(render::MapScene::idle_anim_seconds(0, 1.0f) == 0.0f);
    assets::Skeleton sk;
    sk.names = {"Root", "Hoth"};
    sk.parents = {-1, 0};
    sk.visible = {1, 1};
    sk.billboard_mode = {0, 0};
    sk.bind_local = {Mat3x4{}, Mat3x4{}};
    sk.finalize();
    assets::AnimationClip clip;
    clip.fps = 1.0f;
    clip.frame_count = 240;
    assets::BoneTrack tr;
    tr.bone_name = "Hoth";
    tr.translation = {Float3{}};
    tr.scale = {Float3{1, 1, 1}};
    for (uint32_t f = 0; f < clip.frame_count; ++f) {
        const double a = 0.5 * 2.0 * 3.14159265358979323846 * static_cast<double>(f) / 240.0;
        tr.rotation.push_back({0.0f, 0.0f, static_cast<float>(std::sin(a)), static_cast<float>(std::cos(a))});
    }
    clip.tracks.push_back(tr);
    auto angle_at = [&](uint64_t tick, float rate) {
        assets::Pose pose;
        assets::sample(sk, clip, render::MapScene::idle_anim_seconds(tick, rate), true, pose);
        std::vector<Mat3x4> world(sk.size());
        assets::compute_world(sk, pose.local, world);
        return z_angle_deg(world[1]);
    };
    const float a60 = angle_at(60 * 30, 1.0f), a0 = angle_at(0, 1.0f), a120 = angle_at(120 * 30, 1.0f);
    std::printf("idle spin: t=0 %.3f deg, t=60 s %.3f deg, t=120 s %.3f deg, rate 0.25 t=240 s %.3f deg\n", static_cast<double>(a0),
                static_cast<double>(a60), static_cast<double>(a120), static_cast<double>(angle_at(240 * 30, 0.25f)));
    CHECK(near_eq(a0, 0.0f, 1e-3f) && near_eq(a60, 90.0f, 1e-3f) && near_eq(std::fabs(a120), 180.0f, 1e-3f));
    CHECK(near_eq(angle_at(240 * 30, 0.25f), 90.0f, 1e-3f));         // 960 s per revolution at 0.25
    CHECK(near_eq(angle_at(240 * 30, 1.0f), 0.0f, 1e-3f));           // looped: one full turn per 240 s
    CHECK(near_eq(angle_at(60 * 30 + 1, 1.0f), 90.0f + 360.0f / 240.0f / 30.0f, 1e-3f));  // one tick = 1/30 frame
}

void test_lod() {
    CHECK(render::choose_lod(-1, 1.0f) == -1);
    CHECK(render::choose_lod(2, 1.0f) == 2);
    CHECK(render::choose_lod(2, 0.0f) == 0);
    CHECK(render::choose_lod(2, 0.5f) == 1);
    CHECK(render::choose_lod(2, 0.34f) == 1);
    CHECK(render::choose_lod(2, 0.33f) == 0);
    CHECK(render::choose_lod(0, 1.0f) == 0);
    CHECK(render::mesh_alt_index("HULL_ALT1") == 1);
    CHECK(render::mesh_alt_index("hull_alt0_lod2") == 0);
    CHECK(render::mesh_alt_index("HULL_LOD1") == -1);
    assets::ModelAsset m;
    m.meshes = {mesh("BODY", -1), mesh("BODY_LOD0", 0), mesh("BODY_LOD1", 1), mesh("BODY_LOD2", 2), mesh("FLASH", -1, true),
                mesh("DMG_ALT1", -1), mesh("DMG_ALT0", -1)};
    CHECK(render::max_lod(m.meshes) == 2);
    const render::MeshSelection s = render::select_meshes(m);
    CHECK(s.lod == 2);
    const uint8_t expect[] = {1, 0, 0, 1, 0, 0, 1};
    for (size_t i = 0; i < m.meshes.size(); ++i) CHECK(s.mesh_visible[i] == expect[i]);
    const render::MeshSelection s0 = render::select_meshes(m, 0.0f, 1);
    CHECK(s0.lod == 0 && s0.mesh_visible[1] && !s0.mesh_visible[3] && s0.mesh_visible[5] && !s0.mesh_visible[6]);
    // Submesh filter: render geometry only, hidden flag, visible mesh.
    for (uint32_t i = 0; i < 4; ++i) {
        assets::Submesh sm;
        sm.mesh = i == 3 ? 1 : 0;
        sm.vertex_count = 3;
        sm.index_count = 3;
        if (i == 1) sm.flags = assets::kSubmeshShadowVolume;
        if (i == 2) sm.flags = assets::kSubmeshHidden;
        m.submeshes.push_back(sm);
    }
    const std::vector<uint32_t> d = render::drawable_submeshes(m, s);
    CHECK(d.size() == 1 && d[0] == 0);
    {   // + an ALO-hidden shadow volume: uploaded (masked) only with shadow volumes and include_hidden both on.
        assets::ModelAsset mv = m;
        assets::Submesh hv;
        hv.mesh = 0;
        hv.vertex_count = 3;
        hv.index_count = 3;
        hv.flags = assets::kSubmeshShadowVolume | assets::kSubmeshHidden;
        mv.submeshes.push_back(hv);
        using U = std::vector<uint32_t>;
        CHECK(render::drawable_submeshes(mv, s) == (U{0}));
        CHECK(render::drawable_submeshes(mv, s, true) == (U{0, 1}));
        CHECK(render::drawable_submeshes(mv, s, false, true) == (U{0, 2}));
        CHECK(render::drawable_submeshes(mv, s, true, true) == (U{0, 1, 2, 4}));
    }
    materials::PsoKey k;
    CHECK(render::blend_mode_for_key(k) == render::BlendMode::Opaque);
    k.blend_enable = 1;
    k.src_color = materials::BlendFactor::SrcAlpha;
    k.dst_color = materials::BlendFactor::OneMinusSrcAlpha;
    CHECK(render::blend_mode_for_key(k) == render::BlendMode::Alpha);
    k.dst_color = materials::BlendFactor::One;
    CHECK(render::blend_mode_for_key(k) == render::BlendMode::Additive);
    // LOD detail (SPACE_AND_OPEN.md B.4): d = min(LOD_Bias x coverage, (MeshDetail + 1) / 2), bias 4 default.
    CHECK(render::kDefaultLodBias == 4.0f && render::Instance{}.lod_bias == 4.0f);
    CHECK(near_eq(render::lod_detail(4.0f, 0.1f, 1.0f), 0.4f) && near_eq(render::lod_detail(4.0f, 0.5f, 1.0f), 1.0f));
    CHECK(near_eq(render::lod_detail(4.0f, 0.5f, 0.0f), 0.5f) && near_eq(render::lod_detail(0.5f, 0.2f, 1.0f), 0.1f));
    CHECK(render::choose_lod(2, render::lod_detail(4.0f, 0.1f, 1.0f)) == 1 && render::choose_lod(2, render::lod_detail(1.0f, 0.1f, 1.0f)) == 0);
    // Hidden meshes (B.9): Damage_Decal names (exact, case-insensitive) + STEALTH/buildup/charged substrings;
    // without XML the *_BLAST* fallback.
    CHECK(render::fallback_damage_decal_rule("HP_F-R_Blast") && render::fallback_damage_decal_rule("hp_trac_blast") && !render::fallback_damage_decal_rule("Girders01"));
    CHECK(render::is_state_mesh("Hull_STEALTH") && render::is_state_mesh("Gun_BuildUp_01") && render::is_state_mesh("charged_fx") &&
          !render::is_state_mesh("HULL"));
    assets::ModelAsset dm;
    dm.meshes = {mesh("HULL", -1), mesh("HP_M-C_Blast", -1), mesh("Cloak_Stealth", -1), mesh("HP_X_BLAST", -1)};
    CHECK(render::select_meshes(dm, 1.0f, 0, false).mesh_visible[1] == 0);
    CHECK(render::select_meshes(dm, 1.0f, 0, false).damage_hidden == 3);
    CHECK(render::select_meshes(dm, 1.0f, 0, true).mesh_visible[1] == 1);
    const render::DamageDecalRule hull_rule = [](std::string_view n) { return n == "HULL"; };
    CHECK(render::select_meshes(dm, 1.0f, 0, false, hull_rule).mesh_visible[0] == 0 &&
          render::select_meshes(dm, 1.0f, 0, false, hull_rule).mesh_visible[1] == 1);
    CHECK(render::select_meshes_all_lods(dm, 0, false, nullptr).damage_hidden == 0);
    CHECK(render::RendererDesc{}.hide_damage_decals && render::RendererDesc{}.damage_decal_rule == &render::default_hidden_mesh_rule);
    {
        const std::string decals[] = {"hp_m-c_blast"};  // the type's Damage_Decal (case differs)
        std::vector<uint8_t> mk = render::hidden_mesh_mask(dm, decals);
        CHECK(mk == (std::vector<uint8_t>{0, 1, 1, 0}));  // HP_X_BLAST is no decal of this type: shown
        CHECK(render::hidden_mesh_mask(dm, &render::default_hidden_mesh_rule) == (std::vector<uint8_t>{0, 1, 1, 1}));
        CHECK(render::set_hardpoint_destroyed(mk, dm, "HP_M-C_BLAST", true) == 1 && mk[1] == 0);   // destroyed: decal shown
        CHECK(render::set_hardpoint_destroyed(mk, dm, "HP_M-C_BLAST", false) == 1 && mk[1] == 1);  // repaired: hidden
        CHECK(render::set_hardpoint_destroyed(mk, dm, "NOPE", true) == 0);
    }
    // MP palette (B.3): 9 colours in GameConstants order, alpha 1.
    {
        const render::MpPalette p = render::default_mp_palette();
        CHECK(near_eq(p.color[0].x, 78.0f / 255.0f) && near_eq(p.color[0].z, 237.0f / 255.0f) && p.color[0].w == 1.0f);  // Blue
        CHECK(near_eq(p.color[1].x, 237.0f / 255.0f) && near_eq(p.color[8].y, 129.0f / 255.0f));                         // Red, Eight
        CHECK(near_eq(render::owner_team_color(9, p).x, p.color[0].x) && render::owner_team_color(7, p).w == 1.0f);
    }
    // Galactic camera (B.8): horizontal FOV atan(0.5) at every aspect, near 10, far 10000.
    {
        render::OrbitCamera gc;
        gc.fov_x = 1.0f;
        for (const float a : {4.0f / 3.0f, 16.0f / 9.0f}) {
            const uint32_t w = a > 1.5f ? 1600u : 1200u;
            const render::ViewMatrices gv = render::make_galactic_view(gc, w, 900);
            const float fy = render::fov_y_from_horizontal(std::atan(0.5f), static_cast<float>(w) / 900.0f);
            CHECK(near_eq(gv.proj.m[1][1], 1.0f / std::tan(0.5f * fy)));  // vertical scale from the fixed horizontal FOV
            CHECK(near_eq(gv.proj.m[0][0] * static_cast<float>(w) / 900.0f, gv.proj.m[1][1]));
            const float zn = gv.proj.m[2][3] / gv.proj.m[2][2], zf = gv.proj.m[2][3] / (gv.proj.m[2][2] + 1.0f);
            CHECK(std::fabs(zn - 10.0f) < 1e-2f && std::fabs(zf - 10000.0f) < 5.0f);
        }
        CHECK(near_eq(render::kGalacticFovX, std::atan(0.5f)));
    }
    // Phase buckets (RENDER.md 1.3 order).
    CHECK(render::phase_bucket(materials::RenderPhase::None) == render::PhaseBucket::Opaque);
    CHECK(render::phase_bucket(materials::RenderPhase::Transparent) == render::PhaseBucket::Transparent);
    CHECK(render::phase_bucket(materials::RenderPhase::Heat) == render::PhaseBucket::Heat);
    CHECK(render::phase_bucket(materials::RenderPhase::TerrainMesh) < render::phase_bucket(materials::RenderPhase::Opaque));
    CHECK(render::phase_bucket(materials::RenderPhase::Shadow) < render::phase_bucket(materials::RenderPhase::Transparent));
    assets::MaterialRef mat;
    assets::MaterialParam np;
    np.name = "NormalTexture";
    np.type = assets::ParamType::Texture;
    np.texture = "n.dds";
    mat.params.push_back(np);
    CHECK(render::diffuse_texture_name(mat).empty());
    assets::MaterialParam bp = np;
    bp.name = "BaseTexture";
    bp.texture = "b.dds";
    mat.params.push_back(bp);
    CHECK(render::diffuse_texture_name(mat) == "b.dds");
}

void test_lighting() {
    using assets::Float3;
    render::DirLight sun;
    sun.direction = {0, 0, 1};
    sun.intensity = 1.0f;
    const render::SphMatrices m = render::sph_matrices(&sun, 1, {0, 0, 0});
    const Float3 up = render::sph_irradiance(m, {0, 0, 1}), side = render::sph_irradiance(m, {1, 0, 0}),
                 down = render::sph_irradiance(m, {0, 0, -1});
    // Order-2 SH of a clamped cosine: ~1 facing the light, ~0.09 at 90 degrees (true value 0), ~0 facing away.
    CHECK(up.x > 0.9f && up.x < 1.15f && near_eq(up.x, up.y) && near_eq(up.x, up.z));
    CHECK(side.x > 0.05f && side.x < 0.15f);
    CHECK(std::fabs(down.x) < 0.1f);
    const render::SphMatrices a = render::sph_matrices(nullptr, 0, {0.25f, 0.5f, 0.75f});
    const Float3 e = render::sph_irradiance(a, {0.3f, -0.4f, 0.866f});
    CHECK(near_eq(e.x, 0.25f) && near_eq(e.y, 0.5f) && near_eq(e.z, 0.75f));  // ambient is direction-independent
    for (size_t i = 0; i < 4; ++i)  // symmetric matrices: register rows == columns
        for (size_t j = 0; j < 4; ++j) {
            const float* ri = &m[i].x;
            const float* rj = &m[j].x;
            CHECK(near_eq(ri[j], rj[i]));
        }
    // Space default: fills off -> SPH_LIGHT_FILL is ambient only; SPH_LIGHT_ALL adds the sun.
    render::Lighting l = render::Lighting::space_default();
    l.set_sun_angles(0.0f, 0.0f);  // yaw 0, elevation 0 -> (0, -1, 0)
    CHECK(near_eq(l.sun.direction.x, 0) && near_eq(l.sun.direction.y, -1) && near_eq(l.sun.direction.z, 0));
    const render::SceneLightConstants s = render::scene_light_constants(l);
    const Float3 f1 = render::sph_irradiance(s.sph_fill, {0, -1, 0}), f2 = render::sph_irradiance(s.sph_fill, {0, 1, 0});
    CHECK(near_eq(f1.x, l.ambient.x) && near_eq(f2.x, l.ambient.x));
    const Float3 a1 = render::sph_irradiance(s.sph_all, {0, -1, 0}), a2 = render::sph_irradiance(s.sph_all, {0, 1, 0});
    CHECK(a1.x > a2.x + 0.4f * l.sun.intensity);
    materials::FrameCB cb{};
    render::fill_frame_lights(l, cb);
    CHECK(near_eq(cb.light0_vector.y, -1.0f) && near_eq(cb.light0_diffuse.x, l.sun.color.x * l.sun.intensity));
    CHECK(near_eq(cb.light0_specular.x, l.sun.specular.x * 2.0f * l.sun.intensity));
    // SH lighting contract: a FrameCB without SH under a lit scene fails the check; the renderer's single FrameCB
    // builder (every extras combination) carries SH; an unlit scene (all zero) passes either way.
    CHECK(!render::Renderer::frame_cb_sh_ok(cb, l));
    {
        render::Renderer r0;  // frame_constants() needs no device
        r0.set_lighting(l);
        render::OrbitCamera oc;
        const render::ViewMatrices v = render::make_view(oc, 64, 64);
        for (uint32_t extras : {0u, 1u, 3u, 4u, 7u}) {
            materials::FrameCB fc{};
            r0.frame_constants(v, extras, fc);
            CHECK(render::Renderer::frame_cb_sh_ok(fc, l));
            CHECK(std::memcmp(fc.sph_all, s.sph_all.data(), sizeof fc.sph_all) == 0 &&
                  std::memcmp(fc.sph_fill, s.sph_fill.data(), sizeof fc.sph_fill) == 0);
        }
        render::Lighting dark = l;
        dark.sun.intensity = dark.fill[0].intensity = dark.fill[1].intensity = 0.0f;
        dark.ambient = {0, 0, 0};
        CHECK(render::Renderer::frame_cb_sh_ok(materials::FrameCB{}, dark));
        CHECK(r0.frame_cb_violations() == 0);
    }
}

void test_pipeline_keys() {
    VkFormat c = VK_FORMAT_UNDEFINED, d = VK_FORMAT_UNDEFINED;
    const uint8_t id = render::render_pass_id(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    CHECK(id != 0 && render::render_pass_formats(id, c, d) && c == VK_FORMAT_B8G8R8A8_UNORM && d == VK_FORMAT_D32_SFLOAT);
    const uint8_t id2 = render::render_pass_id(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    CHECK(id2 != 0 && id2 != id && render::render_pass_formats(id2, c, d) && c == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(render::render_pass_id(VK_FORMAT_R8_UNORM, VK_FORMAT_D32_SFLOAT) == 0 && !render::render_pass_formats(0, c, d));
    CHECK(render::effect_key("Data\\Art\\Shaders\\MeshBumpColorize.fx") == "MESHBUMPCOLORIZE");
    {   // D.1 creation order without a file provider: the engine head ranks first, other names at size().
        render::EffectCache ec(nullptr);
        materials::MaterialEffect head, other;
        head.name = "STENCILDARKEN";
        other.name = "AAA_TEST";
        const uint32_t rh = ec.load_rank(head), ro = ec.load_rank(other);
        CHECK(rh < ro && ro == static_cast<uint32_t>(materials::EffectLoadOrder().size()));
    }
    // SPIR-V reflection of the hot-set bump VS: POSITION, NORMAL, TEXCOORD0, TANGENT, BINORMAL.
    const materials::ProgramLookup pl = materials::find_program("MeshBumpColorize");
    CHECK(pl.status == materials::ProgramStatus::Ok && pl.program.vs && pl.program.ps);
    if (pl.program.vs) {
        const std::vector<uint32_t> locs = render::spirv_input_locations(pl.program.vs->spirv);
        CHECK((locs == std::vector<uint32_t>{0, 1, 2, 3, 4}));
        CHECK(render::shader_blob_by_id(pl.program.vs->id) == pl.program.vs);
    }
    const materials::ProgramLookup ad = materials::find_program("MeshAdditive");
    if (ad.program.vs) CHECK((render::spirv_input_locations(ad.program.vs->spirv) == std::vector<uint32_t>{0, 2}));
    // PsoKey -> pipeline description (engine device defaults: cull BACK with CCW front faces).
    materials::PsoKey k = materials::resolve_pso(materials::engine_device_defaults());
    k.vs_id = pl.program.vs ? pl.program.vs->id : 0;
    k.ps_id = pl.program.ps ? pl.program.ps->id : 0;
    k.vertex_layout = materials::VertexLayout::NU2U3U3;
    k.render_pass = id;
    render::PipelineBuild b;
    std::string err;
    CHECK(render::pipeline_desc_from_key(k, VK_NULL_HANDLE, b, &err));
    CHECK(b.desc.raster.cull == VK_CULL_MODE_BACK_BIT && b.desc.raster.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    CHECK(b.attributes.size() == 5 && b.binding.stride == 64 && b.desc.depth_format == VK_FORMAT_D32_SFLOAT);
    CHECK(b.color == VK_FORMAT_B8G8R8A8_UNORM && b.desc.color_formats.size() == 1);
    k.blend_enable = 1;
    k.src_color = materials::BlendFactor::SrcAlpha;
    k.dst_color = materials::BlendFactor::One;
    k.stencil_enable = 1;
    k.front.pass_op = materials::StencilOp::IncrementAndWrap;
    k.cull = materials::CullMode::None;
    CHECK(render::pipeline_desc_from_key(k, VK_NULL_HANDLE, b, &err));
    CHECK(b.desc.blend.enable && b.desc.blend.src_color == VK_BLEND_FACTOR_SRC_ALPHA && b.desc.blend.dst_color == VK_BLEND_FACTOR_ONE);
    CHECK(b.desc.depth.stencil_test && b.desc.depth.front.passOp == VK_STENCIL_OP_INCREMENT_AND_WRAP);
    CHECK(b.desc.raster.cull == VK_CULL_MODE_NONE);
    k.render_pass = 0;
    CHECK(!render::pipeline_desc_from_key(k, VK_NULL_HANDLE, b, &err));
}

void test_ted() {
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    };
    auto chunk = [&](uint32_t id, const std::vector<uint8_t>& payload, bool children) {
        std::vector<uint8_t> v;
        put32(v, id);
        put32(v, static_cast<uint32_t>(payload.size()) | (children ? 0x80000000u : 0u));
        v.insert(v.end(), payload.begin(), payload.end());
        return v;
    };
    auto mini_f = [&](std::vector<uint8_t>& v, uint8_t id, std::initializer_list<float> fs) {
        v.push_back(id);
        v.push_back(static_cast<uint8_t>(fs.size() * 4));
        for (float f : fs) {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            put32(v, u);
        }
    };
    std::vector<uint8_t> rec;
    rec.push_back(0x14);
    rec.push_back(5);
    for (char ch : std::string("Test")) rec.push_back(static_cast<uint8_t>(ch));
    rec.push_back(0);
    mini_f(rec, 0x00, {0.5f, 0.6f, 0.7f});
    mini_f(rec, 0x05, {0.8f});
    mini_f(rec, 0x08, {1.5707963f});
    mini_f(rec, 0x0B, {0.0f});
    mini_f(rec, 0x04, {0.1f, 0.2f, 0.3f});
    mini_f(rec, 0x23, {0.0f});
    mini_f(rec, 0x11, {500.0f});
    mini_f(rec, 0x12, {100.0f});
    std::vector<uint8_t> list = chunk(6, rec, false);
    const std::vector<uint8_t> end = chunk(5, {}, false);
    list.insert(list.end(), end.begin(), end.end());
    std::vector<uint8_t> start = {0x25, 4, 0, 0, 0, 0};
    std::vector<uint8_t> envset = chunk(4, list, true);
    const std::vector<uint8_t> st = chunk(8, start, false);
    envset.insert(envset.end(), st.begin(), st.end());
    const std::vector<uint8_t> world = chunk(0x100, envset, true);
    const std::vector<uint8_t> file = chunk(1, world, true);
    render::MapEnvironments me;
    std::string err;
    CHECK(render::read_map_environments(file, me, &err));
    CHECK(me.space && me.environments.size() == 1 && me.start() && me.start()->name == "Test");
    if (me.start()) {
        const render::Environment& e = *me.start();
        CHECK(near_eq(e.light_color[0][1], 0.6f) && near_eq(e.intensity[0], 0.8f) && near_eq(e.bloom_strength, 0.0f));
        CHECK(near_eq(e.fog_start, 100.0f) && near_eq(e.fog_end, 500.0f));      // stored min / max
        CHECK(near_eq(e.intensity[1], 0.5f) && near_eq(e.bloom_cutoff, 0.9f));  // constructor defaults
        const render::Lighting l = render::Lighting::from_environment(e, me.space);
        CHECK(near_eq(l.sun.direction.x, 1.0f) && near_eq(l.sun.direction.y, 0.0f, 1e-3f));  // yaw 90, elev 0 -> +X
        CHECK(l.fill[0].intensity == 0.0f && near_eq(l.ambient.z, 0.3f) && near_eq(l.sun.intensity, 0.8f));
    }
    std::vector<uint8_t> bad = file;
    bad.pop_back();
    CHECK(!render::read_map_environments(bad, me, &err));
    CHECK(render::map_path("_MP_SPACE_KUAT") == "DATA\\ART\\MAPS\\_MP_SPACE_KUAT.TED");
    const render::Lighting k = render::Lighting::space_default();
    CHECK(near_eq(k.sun.intensity, 0.646f) && near_eq(k.ambient.x, 0.027f) && k.fill[0].intensity == 0.0f);
    CHECK(near_eq(render::Lighting::land_default().fill[1].intensity, 0.5f));
}
// World CPU helpers: placement transform, cloud matrices, mirrored reflection camera, oblique clip plane.
void test_world() {
    {   // placement: R = Rz(yaw) * scale, then the stored position
        const float pos[3] = {10.0f, 20.0f, 5.0f}, rot[3] = {0.0f, 0.0f, 90.0f};
        const Mat3x4 m = terrain::placement_transform(pos, rot, 2.0f);
        const Float3 p = m.transform_point(Float3{1.0f, 0.0f, 0.0f});
        CHECK(near_eq(p.x, 10.0f, 1e-4f) && near_eq(p.y, 22.0f, 1e-4f) && near_eq(p.z, 5.0f, 1e-4f));
        const Float3 q = m.transform_point(Float3{0.0f, 0.0f, 1.0f});
        CHECK(near_eq(q.z, 7.0f, 1e-4f));
    }
    {   // CLOUD_TEX_U/V = (1/s,0,0,-ox/s), (0,1/s,0,-oy/s); update(): dt clamped to 1 s, offset += velocity dt
        materials::float4 u, v;
        render::WorldRenderer::cloud_matrices(500.0f, 100.0f, 50.0f, u, v);
        CHECK(near_eq(u.x, 0.002f) && u.y == 0.0f && near_eq(u.w, -0.2f) && near_eq(v.y, 0.002f) && near_eq(v.w, -0.1f));
        render::WorldRenderer w;
        w.update(2.5f);
        w.update(0.25f);
        CHECK(near_eq(w.water_time(), 1.25f) && near_eq(w.cloud_offset_x(), 6.25f) && near_eq(w.cloud_offset_y(), 6.25f));
    }
    {   // mirrored camera: P through the mirrored view == mirror(P) through the camera; eye mirrored; det -1
        render::OrbitCamera cam;
        cam.target = {100.0f, 200.0f, 0.0f};
        cam.yaw = -1.2f;
        cam.pitch = 0.6f;
        cam.distance = 400.0f;
        const render::ViewMatrices v = render::make_view(cam, 640, 480, 10.0f, 7000.0f);
        const float h = 12.0f;
        const render::ViewMatrices mv = render::WorldRenderer::mirrored_view(v, h);
        const render::Float4 a = mv.view_proj * render::Float4{130.0f, 260.0f, 40.0f, 1.0f};
        const render::Float4 b = v.view_proj * render::Float4{130.0f, 260.0f, 2.0f * h - 40.0f, 1.0f};
        CHECK(near_eq(a.x / a.w, b.x / b.w, 1e-4f) && near_eq(a.y / a.w, b.y / b.w, 1e-4f) && near_eq(a.z / a.w, b.z / b.w, 1e-5f));
        CHECK(near_eq(mv.eye.z, 2.0f * h - v.eye.z, 1e-3f) && near_eq(mv.eye.x, v.eye.x, 1e-3f));
        const auto& m = mv.view.m;
        const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                          m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        CHECK(near_eq(det, -1.0f, 1e-4f));
        const render::Float4 e = mv.view * render::Float4{mv.eye.x, mv.eye.y, mv.eye.z, 1.0f};  // eye at the view origin
        CHECK(near_eq(e.x, 0.0f, 1e-2f) && near_eq(e.y, 0.0f, 1e-2f) && near_eq(e.z, 0.0f, 1e-2f));
        // Reflection clip (keep z >= h): above the plane in front -> 0 <= z <= w; below -> z < 0. x / y unchanged.
        render::ViewMatrices rc = mv;
        CHECK(render::WorldRenderer::clip_projection(rc, render::Float4{0.0f, 0.0f, 1.0f, -h}));
        const render::Float4 up = rc.view_proj * render::Float4{100.0f, 200.0f, h + 30.0f, 1.0f};
        const render::Float4 dn = rc.view_proj * render::Float4{100.0f, 200.0f, h - 30.0f, 1.0f};
        const render::Float4 up0 = mv.view_proj * render::Float4{100.0f, 200.0f, h + 30.0f, 1.0f};
        CHECK(up.z >= 0.0f && up.z <= up.w && dn.z < 0.0f);
        CHECK(near_eq(up.x / up.w, up0.x / up0.w, 1e-4f) && near_eq(up.y / up.w, up0.y / up0.w, 1e-4f));
        // The oblique matrix replaces the far plane by the plane through the camera's far frustum corner Q with
        // the largest clip-plane distance (it touches the original frustum only there). Points along the ray
        // to Q are therefore cut exactly at the far distance with far x1, and only at 8x far with the far plane
        // pushed out (WorldRenderer::kObliqueFarScale); the oblique near plane keeps cutting the other side.
        render::ViewMatrices fs = mv;
        CHECK(render::WorldRenderer::clip_projection(fs, render::Float4{0.0f, 0.0f, 1.0f, -h}, render::WorldRenderer::kObliqueFarScale));
        const float zf = 7000.0f;
        float cplane[3] = {0, 0, 0};  // the plane's xyz in view space (sign only): p_world . V^-1
        for (int j = 0; j < 3; ++j) cplane[j] = mv.view_inv.m[2][j];  // plane (0,0,1,-h): row 2 of V^-1
        const float qx = (cplane[0] > 0.0f ? 1.0f : -1.0f) / mv.proj.m[0][0], qy = (cplane[1] > 0.0f ? 1.0f : -1.0f) / mv.proj.m[1][1];
        auto along_q = [&](float t) { return mv.view_inv * render::Float4{qx * t, qy * t, -t, 1.0f}; };  // view ray to Q
        auto inside = [](const render::Float4& c) { return c.z >= 0.0f && c.z <= c.w; };
        const render::Float4 q99 = along_q(0.99f * zf), q120 = along_q(1.2f * zf), q900 = along_q(9.0f * zf);
        CHECK(q99.z > h && q120.z > h);  // on the kept side of the water plane
        const render::Float4 c99 = rc.view_proj * q99, c120 = rc.view_proj * q120, f99 = fs.view_proj * q99;
        const render::Float4 f120 = fs.view_proj * q120, f900 = fs.view_proj * q900;
        std::printf("oblique far plane along the far corner ray: z/w at 0.99 far %.4f (x1) %.4f (x8); 1.2 far %.4f (x1) %.4f (x8); "
                    "9 far %.4f (x8)\n",
                    c99.z / c99.w, f99.z / f99.w, c120.z / c120.w, f120.z / f120.w, f900.z / f900.w);
        CHECK(inside(c99) && inside(f99));    // in front of the far corner: kept by both
        CHECK(!inside(c120) && c120.z > c120.w);  // beyond the camera far: cut by the x1 oblique far plane ...
        CHECK(inside(f120));                      // ... kept once the far plane is pushed out
        CHECK(!inside(f900) && f900.z > f900.w);  // the pushed plane still cuts (beyond 8x far)
        // The pushed-out matrix keeps the oblique near plane: below the water clipped, above kept, near and far.
        const render::Float4 fup = fs.view_proj * render::Float4{100.0f, 200.0f, h + 30.0f, 1.0f};
        const render::Float4 fdn = fs.view_proj * render::Float4{100.0f, 200.0f, h - 30.0f, 1.0f};
        const render::Float4 qdn = fs.view_proj * render::Float4{q99.x, q99.y, 2.0f * h - q99.z, 1.0f};  // q99 mirrored below
        CHECK(inside(fup) && fdn.z < 0.0f && qdn.z < 0.0f);
        // Refraction clip (keep z <= h) with the real camera above the water.
        render::ViewMatrices fc = v;
        CHECK(render::WorldRenderer::clip_projection(fc, render::Float4{0.0f, 0.0f, -1.0f, h}));
        const render::Float4 below = fc.view_proj * render::Float4{100.0f, 200.0f, h - 30.0f, 1.0f};
        const render::Float4 above = fc.view_proj * render::Float4{100.0f, 200.0f, h + 30.0f, 1.0f};
        CHECK(below.z >= 0.0f && below.z <= below.w && above.z < 0.0f);
        // The eye on the kept side: no oblique plane (unchanged).
        render::ViewMatrices nc = v;
        CHECK(!render::WorldRenderer::clip_projection(nc, render::Float4{0.0f, 0.0f, 1.0f, -h}));
        CHECK(std::memcmp(&nc.proj, &v.proj, sizeof v.proj) == 0);
    }
    {   // tactical FOV: 55 degrees at 4:3, widened for 16:9
        CHECK(near_eq(render::tactical_fov_x(55.0f, 4.0f / 3.0f), 55.0f * 0.017453293f));
        CHECK(near_eq(render::tactical_fov_x(55.0f, 16.0f / 9.0f), 2.0f * std::atan(std::tan(27.5f * 0.017453293f) * 0.75f * 16.0f / 9.0f)));
    }
}

// GameTypeResolver precedence on a synthetic database: the map mode's model field along the whole variant
// chain first (an explicit field on the variant wins, a base's field of the map mode beats the variant's
// other-mode field), then the other mode's field, then Galactic_Model_Name; Scale_Factor from the first
// record of the chain that writes it; the first definition of a duplicated name wins; cycles terminate.
void test_type_resolution() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() /
                         ("alamo_render_types_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path x = tmp / "Data" / "XML";
    std::error_code ec;
    fs::create_directories(x, ec);
    auto write = [](const fs::path& p, const std::string& s) {
        std::ofstream f(p, std::ios::binary);
        f << s;
    };
    write(x / "GameObjectFiles.xml", "<Game_Object_Files><File>Types.xml</File><File>Later.xml</File></Game_Object_Files>");
    write(x / "Types.xml",
          "<Objects>"
          "<Unit Name=\"Base\"><Land_Model_Name>base_land.alo</Land_Model_Name><Space_Model_Name>base_space.alo</Space_Model_Name>"
          "<Galactic_Model_Name>base_gal.alo</Galactic_Model_Name><Scale_Factor>2.0</Scale_Factor></Unit>"
          "<Unit Name=\"Var_Space\"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type>"
          "<Space_Model_Name>var_space.alo</Space_Model_Name><Scale_Factor>1.0</Scale_Factor></Unit>"
          "<Unit Name=\"Var_Land\"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type><Land_Model_Name>var_land.alo</Land_Model_Name></Unit>"
          "<Unit Name=\"Var_Var\"><Variant_Of_Existing_Type>Var_Space</Variant_Of_Existing_Type></Unit>"
          "<Unit Name=\"Space_Only\"><Space_Model_Name>only_space.alo</Space_Model_Name></Unit>"
          "<Unit Name=\"Gal_Only\"><Galactic_Model_Name>only_gal.alo</Galactic_Model_Name><Scale_Factor>-3</Scale_Factor></Unit>"
          "<Unit Name=\"Loop_A\"><Variant_Of_Existing_Type>Loop_B</Variant_Of_Existing_Type></Unit>"
          "<Unit Name=\"Loop_B\"><Variant_Of_Existing_Type>Loop_A</Variant_Of_Existing_Type><Land_Model_Name>loop.alo</Land_Model_Name></Unit>"
          "<Unit Name=\"No_Model\"/>"
          "</Objects>");
    write(x / "Later.xml", "<Objects><Unit Name=\"BASE\"><Land_Model_Name>duplicate.alo</Land_Model_Name></Unit></Objects>");
    {
        DirectoryProvider dp(path_to_utf8(tmp));
        data::GameDatabase db;
        (void)db.load(dp);  // no GameConstants / other lists: false, the objects still load
        render::GameTypeResolver r(db);
        auto model = [&](const char* type, bool space) { return r.model_of(type, space); };
        auto scale = [&](const char* type) {
            terrain::ResolvedType t;
            return r.resolve(alamo::name_crc(type), false, t) ? t.scale_factor : -1.0f;
        };
        CHECK(model("Base", false) == "base_land.alo" && model("Base", true) == "base_space.alo");
        CHECK(model("Var_Space", true) == "var_space.alo");    // the variant's own field of the map mode
        CHECK(model("Var_Space", false) == "base_land.alo");   // the base's land field beats the variant's space field
        CHECK(model("Var_Land", false) == "var_land.alo" && model("Var_Land", true) == "base_space.alo");
        CHECK(model("Var_Var", true) == "var_space.alo" && model("Var_Var", false) == "base_land.alo");  // two levels
        CHECK(model("Space_Only", false) == "only_space.alo");  // land map: falls back to the space field
        CHECK(model("Gal_Only", false) == "only_gal.alo" && model("Gal_Only", true) == "only_gal.alo");
        CHECK(model("Loop_A", false) == "loop.alo");           // cycle terminates, the base's field found
        CHECK(model("No_Model", false).empty() && model("Unknown_Type", false).empty());
        terrain::ResolvedType t;
        CHECK(!r.resolve(alamo::name_crc("Unknown_Type"), false, t) && r.resolve(alamo::name_crc("No_Model"), false, t));
        CHECK(scale("Base") == 2.0f && scale("Var_Space") == 1.0f && scale("Var_Land") == 2.0f && scale("Var_Var") == 1.0f);
        CHECK(scale("Gal_Only") == 1.0f);  // invalid Scale_Factor -> 1
        std::printf("type resolution: Var_Space land %s / space %s, Var_Land space %s, duplicate BASE -> %s\n",
                    model("Var_Space", false).c_str(), model("Var_Space", true).c_str(), model("Var_Land", true).c_str(),
                    model("Base", false).c_str());
    }
    fs::remove_all(tmp, ec);
}

// Hot-set / FF CLOUD_TEXTURE slots: a sampler with the CLOUD_TEXTURE semantic and no material texture is
// bound to the world's cloud texture; a slot holding a material texture or another semantic is not.
void test_cloud_slots() {
    materials::ResolvedPass p;
    materials::SamplerBinding base, cloud, fow;
    base.slot = 0;
    base.texture_param = "BaseTexture";
    cloud.slot = 1;
    cloud.texture_param = "m_cloudTexture";
    cloud.texture_semantic = "CLOUD_TEXTURE";
    fow.slot = 2;
    fow.texture_semantic = "FOW_TEXTURE";
    p.samplers = {base, cloud, fow};
    const int none[4] = {0, -1, -1, -1}, bound[4] = {0, 3, -1, -1};
    CHECK(render::Renderer::cloud_texture_slots(p, none) == 0x2);
    CHECK(render::Renderer::cloud_texture_slots(p, bound) == 0);  // a material texture in the slot wins
    p.samplers[1].texture_semantic = "cloud_texture";             // semantics compare case-insensitively
    CHECK(render::Renderer::cloud_texture_slots(p, none) == 0x2);
}

// cu8f t05 (CU8F_T03_T05_RE §2.1 / §2.2): the particle scene wind. Land = speed x (cos w, sin w, 0) of the start
// environment (0 deg = +X, 90 deg = +Y); no environment = the ctor default (0 deg, 2.0); space = zero.
void test_scene_wind() {
    terrain::MapData m;
    m.terrain.present = true;  // land
    Float3 w = render::scene_wind(m);
    CHECK(near_eq(w.x, 2.0f) && near_eq(w.y, 0.0f) && w.z == 0.0f);
    terrain::Environment a, b;
    a.wind_direction_deg = 0.0f;
    a.wind_speed = 7.0f;
    b.wind_direction_deg = 90.0f;
    b.wind_speed = 10.0f;
    m.environments.environments = {a, b};
    m.environments.start_index = 1;
    w = render::scene_wind(m);
    CHECK(std::fabs(w.x) < 1e-4f && near_eq(w.y, 10.0f) && w.z == 0.0f);
    m.environments.start_index = 0;
    w = render::scene_wind(m);
    CHECK(near_eq(w.x, 7.0f) && std::fabs(w.y) < 1e-4f);
    m.terrain.present = false;  // space: never written
    w = render::scene_wind(m);
    CHECK(w.x == 0.0f && w.y == 0.0f && w.z == 0.0f);
    // cu8g t03: the wind of the environment Set_New_Environment made active (index form; clamped to n - 1), space zero.
    m.terrain.present = true;
    m.environments.start_index = 0;
    w = render::scene_wind(m, 1);
    CHECK(std::fabs(w.x) < 1e-4f && near_eq(w.y, 10.0f));
    w = render::scene_wind(m, 0);
    CHECK(near_eq(w.x, 7.0f) && std::fabs(w.y) < 1e-4f);
    w = render::scene_wind(m, 9);  // clamped to the last environment
    CHECK(std::fabs(w.x) < 1e-4f && near_eq(w.y, 10.0f));
    CHECK(render::active_environment(m, 9) == &m.environments.environments[1] && render::active_environment(m, 0) == &m.environments.environments[0]);
    m.terrain.present = false;
    w = render::scene_wind(m, 1);
    CHECK(w.x == 0.0f && w.y == 0.0f && w.z == 0.0f);
    terrain::MapData none;
    CHECK(render::active_environment(none, 0) == nullptr);
    none.terrain.present = true;
    w = render::scene_wind(none, 3);  // land without environments: the record ctor default
    CHECK(near_eq(w.x, 2.0f) && w.z == 0.0f);
    // A scene without a map has no environment to switch to.
    render::MapScene empty;
    CHECK(!empty.set_environment(1) && empty.environment_index() == 0 && empty.environment() == nullptr);
    // WorldRenderer::set_clouds before init: scale / velocity only (the offset keeps accumulating from there).
    render::WorldRenderer world;
    world.update(1.0f);  // default velocity (5, 5)
    terrain::CloudParams cp;
    cp.scale = 800.0f;
    cp.velocity[0] = 1.0f;
    cp.velocity[1] = -2.0f;
    cp.texture = "W_Clouds01.tga";
    world.set_clouds(cp);
    world.update(1.0f);
    CHECK(near_eq(world.cloud_scale(), 800.0f) && near_eq(world.cloud_velocity_x(), 1.0f) && near_eq(world.cloud_velocity_y(), -2.0f));
    CHECK(near_eq(world.cloud_offset_x(), 6.0f) && near_eq(world.cloud_offset_y(), 3.0f) && world.cloud_texture_name() == nullptr);
}

// cu8g t03 (real data): MapScene::set_environment before start() switches the active environment of a loaded land map:
// lighting(), environment() and scene_wind() follow it; out of range clamps; the same index changes nothing.
void test_scene_environment_real() {
    const char* data = std::getenv("ALAMO_FOC_DATA");
    const std::string root = data ? data : "";
    if (root.empty()) {
        std::printf("scene environment: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::MapScene scene;
    CHECK(scene.load(*files, "_MP_LAND_ENDOR", nullptr, serial_for, &err));  // 4 environments (CU8G_T02_ENV_PICK_RE §7)
    const std::vector<terrain::Environment>& envs = scene.map().environments.environments;
    std::printf("scene environment: %zu environments, start %u\n", envs.size(), scene.environment_index());
    CHECK(envs.size() >= 2 && !scene.space());
    if (envs.empty()) return;
    const uint32_t n = static_cast<uint32_t>(envs.size());
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t before = scene.environment_index();
        CHECK(scene.set_environment(i) == (i != before));
        CHECK(scene.environment_index() == i && scene.environment() == &envs[i]);
        CHECK(!scene.set_environment(i));  // already active
        const std::array<float, 3> wv = envs[i].wind_vector();
        const Float3 w = scene.scene_wind();
        CHECK(near_eq(w.x, wv[0]) && near_eq(w.y, wv[1]) && w.z == 0.0f);
        const render::Lighting l = render::Lighting::from_environment(envs[i], false);
        CHECK(near_eq(scene.lighting().sun.intensity, l.sun.intensity) && near_eq(scene.lighting().fog_start, l.fog_start) &&
              scene.lighting().fog_enabled == l.fog_enabled && scene.lighting().skydome[0] == l.skydome[0]);
    }
    scene.set_environment(n + 5);
    CHECK(scene.environment_index() == n - 1);
}

void test_png() {
    const uint8_t iend[] = {'I', 'E', 'N', 'D'};
    CHECK(alamo::crc32(iend, sizeof iend) == 0xAE426082u);
    const char* wiki = "Wikipedia";
    CHECK(render::adler32({reinterpret_cast<const uint8_t*>(wiki), 9}) == 0x11E60398u);
    std::vector<uint8_t> px(3 * 2 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(i * 7);
    const std::vector<uint8_t> f = render::encode_png_rgba(3, 2, px);
    CHECK(f.size() > 8 + 25 + 12);
    CHECK(f[0] == 0x89 && f[1] == 'P' && f[12] == 'I' && f[13] == 'H');
    // Walk the chunks: CRCs valid, IDAT = zlib header + one stored block holding the scanlines.
    size_t pos = 8;
    bool saw_iend = false;
    std::vector<uint8_t> idat;
    while (pos + 12 <= f.size()) {
        const uint32_t len = (uint32_t(f[pos]) << 24) | (uint32_t(f[pos + 1]) << 16) | (uint32_t(f[pos + 2]) << 8) | f[pos + 3];
        const std::span<const uint8_t> typed(f.data() + pos + 4, len + 4);
        const size_t c = pos + 8 + len;
        const uint32_t crc = (uint32_t(f[c]) << 24) | (uint32_t(f[c + 1]) << 16) | (uint32_t(f[c + 2]) << 8) | f[c + 3];
        CHECK(alamo::crc32(typed.data(), typed.size()) == crc);
        if (!std::memcmp(f.data() + pos + 4, "IDAT", 4)) idat.assign(f.begin() + static_cast<std::ptrdiff_t>(pos + 8),
                                                                   f.begin() + static_cast<std::ptrdiff_t>(pos + 8 + len));
        if (!std::memcmp(f.data() + pos + 4, "IEND", 4)) saw_iend = true;
        pos = c + 4;
    }
    CHECK(saw_iend && pos == f.size());
    CHECK(idat.size() == 2 + 5 + 2 * (1 + 12) + 4);
    if (idat.size() == 2 + 5 + 26 + 4) {
        CHECK(idat[0] == 0x78 && ((idat[0] << 8) | idat[1]) % 31 == 0);
        CHECK(idat[2] == 1 && idat[3] == 26 && idat[4] == 0 && idat[5] == static_cast<uint8_t>(~26));
        CHECK(idat[7] == 0 && idat[8] == px[0] && idat[7 + 13] == 0 && idat[7 + 14] == px[12]);
    }
}

}  // namespace test_render_t

using namespace test_render_t;


// Hardpoint plumbing (hardpoints.hpp): XML record, mesh -> bone map, effects, per-instance visibility composition.
void test_hardpoints() {
    {   // <HardPoint> record: known fields + every tag kept (repeats, case-insensitive lookup, empty element).
        const std::string xml =
            "<HardPoints><HardPoint Name=\"HP_Test\">\n"
            "  <Type> HARD_POINT_WEAPON_LASER </Type>\n"
            "  <Model_To_Attach>EV_Test_HP.alo</Model_To_Attach>\n"
            "  <Attachment_Bone> HP_A_BONE </Attachment_Bone>\n"
            "  <Damage_Decal />\n"
            "  <Fire_Inaccuracy_Distance> Fighter, 70.0 </Fire_Inaccuracy_Distance>\n"
            "  <Fire_Inaccuracy_Distance> Bomber, 70.0 </Fire_Inaccuracy_Distance>\n"
            "  <Damage_Particles>HP_A_EMITDAMAGE</Damage_Particles>\n"
            "  <Engine_Particles>HP_E_MAINENGINES</Engine_Particles>\n"
            "  <Engine_Death_Hide_Engine_Particles>No</Engine_Death_Hide_Engine_Particles>\n"
            "</HardPoint><HardPoint Name=\"HP_Dummy\"><Type>HARD_POINT_DUMMY_ART</Type><Is_Destroyable>No</Is_Destroyable></HardPoint></HardPoints>";
        const data::XmlDocument doc = data::XmlDocument::parse(std::vector<uint8_t>(xml.begin(), xml.end()), "hp.xml");
        CHECK(doc.ok());
        const render::HardpointDef d = render::parse_hardpoint(doc.root().child("HardPoint"));
        CHECK(d.known && d.name == "HP_Test" && d.attachment_bone == "HP_A_BONE" && d.model_to_attach == "EV_Test_HP.alo");
        CHECK(d.damage_decal.empty() && d.has_tag("damage_decal") && !d.has_tag("Health"));
        CHECK(d.tags.size() == 9 && d.tag("TYPE") == "HARD_POINT_WEAPON_LASER" && d.tag("Fire_Inaccuracy_Distance") == "Fighter, 70.0");
        CHECK(d.type == "HARD_POINT_WEAPON_LASER" && !d.dummy_art() && d.destroyable && d.damage_particles == "HP_A_EMITDAMAGE" &&
              d.engine_particles == "HP_E_MAINENGINES" && !d.hide_engine_particles);
        const render::HardpointDef dd = render::parse_hardpoint(doc.root().child("HardPoint").next_sibling("HardPoint"));
        CHECK(dd.name == "HP_Dummy" && dd.dummy_art() && !dd.destroyable && dd.hide_engine_particles);  // flag default true
    }
    // Synthetic owner (B.10 rule): Root(0) -> HP_A_BONE(1) -> {HP_A_EMITDAMAGE(2), HP_A_BLAST(3)};
    // Root -> HP_B_BONE(4) -> {HP_B_MAINENGINES(5), HP_B_EMITDAMAGE(6)}.
    assets::ModelAsset m;
    m.skeleton.names = {"Root", "HP_A_BONE", "HP_A_EMITDAMAGE", "HP_A_BLAST", "HP_B_BONE", "HP_B_MAINENGINES", "HP_B_EMITDAMAGE"};
    m.skeleton.parents = {-1, 0, 1, 1, 0, 4, 4};
    auto add = [&](const char* name, uint32_t bone, bool hidden = false) {
        assets::MeshInfo mi = mesh(name, -1, hidden);
        mi.attach_bone = bone;
        m.meshes.push_back(mi);
    };
    add("HULL", 0);              // 0
    add("HP_A_Blast", 3);        // 1 Damage_Decal of A (first match)
    add("A_SCORCH", 2, true);    // 2 ALO hidden, under A's Damage_Particles bone
    add("Shield_Stealth", 0);    // 3 state mesh
    add("ENGINE_GLOW", 5);       // 4 under B's Engine_Particles bone
    add("hp_a_blast", 0);        // 5 duplicate decal name: never reached
    add("BAD_BONE", 99);         // 6 bone out of range
    m.proxies = {{"p_a_damage", 2, false}, {"p_engine", 5, false}, {"p_b_damage", 6, false}};  // sub-objects 7, 8, 9
    const render::MeshBoneMap bm(m);
    CHECK(bm.mesh_count() == 7 && bm.bone_count() == 7 && bm.bone_of(2) == 2 && bm.bone_of(6) == -1 && bm.bone_of(100) == -1);
    CHECK(bm.parent_of(2) == 1 && bm.parent_of(0) == -1 && bm.parent_of(9) == -1);
    // Subtree: own bone or a descendant, never bone 0 (as root or as the sub-object's bone).
    CHECK(bm.in_subtree(2, 1) && bm.in_subtree(1, 1) && !bm.in_subtree(1, 0) && !bm.in_subtree(0, 0) && !bm.in_subtree(5, 1) &&
          !bm.in_subtree(-1, 1));
    CHECK(bm.meshes_on_bone(0) == (std::vector<uint32_t>{0, 3, 5}) && bm.meshes_in_subtree(1) == (std::vector<uint32_t>{1, 2}));
    CHECK(bm.meshes_in_subtree(0).empty() && bm.meshes_on_bone(-1).empty());
    std::vector<render::HardpointDef> defs(3);
    defs[0].name = "HP_A";
    defs[0].attachment_bone = "HP_A_BONE";
    defs[0].damage_decal = "HP_A_BLAST";
    defs[0].damage_particles = "hp_a_emitdamage";
    defs[0].model_to_attach = "A.alo";
    defs[1].name = "HP_B";
    defs[1].attachment_bone = "HP_B_MAINENGINES";  // attached model inside the Engine_Particles subtree
    defs[1].damage_particles = "HP_B_EMITDAMAGE";
    defs[1].engine_particles = "HP_B_MAINENGINES";
    defs[1].model_to_attach = "B.alo";
    defs[2].name = "HP_C_Dummy";
    defs[2].type = "HARD_POINT_DUMMY_ART";
    defs[2].damage_particles = "HP_A_EMITDAMAGE";  // shares A's bones
    const render::HardpointLayout l = render::build_hardpoint_layout(m, defs);
    CHECK(l.meshes == 7 && l.proxies == 3 && l.hardpoints == 3 && l.sub_objects() == 13 && l.attachment_index(1) == 11);
    CHECK(l.attach_bone == (std::vector<int32_t>{1, 5, -1}));
    using V = std::vector<uint8_t>;
    using U = std::vector<uint32_t>;
    //                       meshes 0..6           proxies  attachments
    CHECK(l.base == (V{0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0}));
    CHECK(render::build_hardpoint_layout(m, defs, true).base == (V{0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));  // ALO flags only
    CHECK(render::base_hidden_mask(m, defs) == (V{0, 1, 1, 1, 0, 0, 0}));
    CHECK(l.effects[0].show == (U{1, 2, 7}) && l.effects[0].hide.empty());
    CHECK(l.effects[1].show == (U{9}) && l.effects[1].hide == (U{4, 8, 11}));
    CHECK(l.effects[2].show.empty() && l.effects[2].hide.empty());  // DUMMY_ART: never changes visuals
    {
        std::vector<render::HardpointDef> noflag = defs;
        noflag[1].hide_engine_particles = false;
        noflag[0].destroyable = false;
        const render::HardpointLayout l2 = render::build_hardpoint_layout(m, noflag);
        CHECK(l2.effects[1].hide.empty() && l2.effects[1].show == (U{9}) && l2.effects[0].show.empty() && l2.base == l.base);
    }
    CHECK(render::meshes_named(m, "ENGINE_glow") == (U{4}) && render::meshes_named(m, "").empty());
    {   // Composition: base -> destroyed hardpoints in destruction order (show, hide) -> overrides.
        render::HardpointVisibility v;
        v.reset(3);
        CHECK(v.set_destroyed(0, true) && !v.set_destroyed(3, true) && v.destroyed(0) && v.mask().empty() && !v.ready());
        v.init(std::make_shared<const render::HardpointLayout>(l));
        const uint8_t* storage = v.mask().data();
        auto mask = [&] { return V(v.mask().begin(), v.mask().end()); };
        CHECK(v.ready() && mask() == (V{0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0}));  // A: decal + ALO-hidden scorch + proxy shown
        CHECK(v.attachment_visible(0) && v.attachment_visible(1) && !v.attachment_visible(2));
        CHECK(v.set_destroyed(1, true) && v.set_destroyed(1, true) && v.destroyed_count() == 2);
        CHECK(mask() == (V{0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0}));  // B: damage proxy shown, engine subtree hidden
        CHECK(v.attachment_visible(0) && !v.attachment_visible(1));
        CHECK(v.set_destroyed(2, true) && mask() == (V{0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0}));  // dummy: no change
        CHECK(v.set_override(3, render::MeshOverride::Show) && v.set_override(0, render::MeshOverride::Hide) &&
              !v.set_override(13, render::MeshOverride::Show));
        CHECK(v.mask()[3] == 0 && v.mask()[0] == 1);
        CHECK(v.set_override(3, render::MeshOverride::None) && v.set_override(0, render::MeshOverride::None) && v.mask()[3] == 1);
        // Debug reset (the game never repairs): back to the setup mask.
        CHECK(v.set_destroyed(0, false) && v.set_destroyed(1, false) && v.set_destroyed(2, false) && mask() == l.base);
        CHECK(v.mask().data() == storage);  // stable storage for Instance::mesh_hidden
        // Last write wins: A shows ENGINE_GLOW (4), B hides it.
        render::HardpointLayout lo = l;
        lo.effects[0].show = {4};
        render::HardpointVisibility w;
        w.reset(3);
        w.init(std::make_shared<const render::HardpointLayout>(lo));
        CHECK(w.set_destroyed(0, true) && w.set_destroyed(1, true) && w.hidden(4));
        CHECK(w.set_destroyed(0, false) && w.set_destroyed(1, false) && w.set_destroyed(1, true) && w.set_destroyed(0, true) && !w.hidden(4));
        // Fallback layout (no XML): the given mesh mask + ALO proxy flags, no effects.
        w.init(std::make_shared<const render::HardpointLayout>(render::fallback_hardpoint_layout(m, V{1, 0, 0, 0, 0, 0, 0}, 3)));
        CHECK(w.destroyed(0) && w.mask().size() == 13 && w.hidden(0) && !w.hidden(1) && !w.attachment_visible(0));
    }
    {   // Attachment transform: owner x bone (pose when given, else bind), identity bone for -1.
        assets::ModelAsset o;
        o.skeleton.names = {"Root", "B"};
        o.skeleton.parents = {-1, 0};
        o.skeleton.bind_world.resize(2);
        o.skeleton.bind_world[1].m[0][3] = 5.0f;
        Mat3x4 owner;
        owner.m[1][3] = 2.0f;
        CHECK(near_eq(render::attachment_transform(o, owner, {}, 1).m[0][3], 5.0f) && near_eq(render::attachment_transform(o, owner, {}, 1).m[1][3], 2.0f));
        std::vector<Mat3x4> pose(2);
        pose[1].m[2][3] = 7.0f;
        CHECK(near_eq(render::attachment_transform(o, owner, pose, 1).m[2][3], 7.0f) && near_eq(render::attachment_transform(o, owner, pose, 1).m[0][3], 0.0f));
        CHECK(near_eq(render::attachment_transform(o, owner, {}, -1).m[1][3], 2.0f));
    }
    render::TypeRenderInfo ti;
    ti.hardpoints = {"HP_Star_Destroyer_Weapon_FL", "HP_Star_Destroyer_Engines"};
    CHECK(ti.hardpoint_index("hp_star_destroyer_engines") == 1 && ti.hardpoint_index("HP_Nope") == -1 && ti.hardpoint_index("") == -1);
}

// GCC flags free() inside a replaced operator delete (-Wmismatched-new-delete) although the replaced operator new
// above allocates with malloc(); the pairing is correct by construction.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    if (g_count_allocs.load(std::memory_order_relaxed)) {
        const uint64_t k = g_allocs.fetch_add(1, std::memory_order_relaxed);
        if (k < 8) g_alloc_sizes[k] = n;
    }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif


int main() {
    test_formats();
    test_hardpoints();
    test_layouts();
    test_samplers();
    test_camera();
    test_render_scale_extent();
    test_ssao_math();
    test_idle_spin();
    test_lod();
    test_lighting();
    test_pipeline_keys();
    test_ted();
    test_png();
    test_world();
    test_type_resolution();
    test_cloud_slots();
    test_scene_wind();
    test_scene_environment_real();
    std::printf("cpu: %s\n", g_fail ? "FAILED" : "ok");
    const int gpu = run_gpu(ALAMO_RENDER_SOURCE_DIR);
    if (gpu) ++g_fail;
    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}

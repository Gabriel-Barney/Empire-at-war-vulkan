// alamo_render_reference_tests (C4b): fixed-function alpha/additive effects and skinned draws checked
// against independent references. No framework; non-zero exit on failure.
//   CPU (always): the D3D9 FF reference model, the per-draw FF evaluation API (materials
//     FfDrawTemplate) against the FXO formulas, the alPrimVert attribute tables, PNG/glTF readers.
//   GPU (skips without a Vulkan 1.3 device; validation required when toolchain\vvl is present):
//     synthetic quads drawn through engine/render with each effect, compared per pixel with the
//     reference model (tolerance 2/255).
//   Data (ALAMO_FOC_DATA = FoC "corruption" folder): the real FXOs for the GPU comparison; the
//     Royal Guard (and Dark Trooper if present) skinned against the Phase 1 glTF export
//     (docs/formats/examples) on the CPU, and rendered for silhouette IoU against the reference
//     PNGs -> logs/reference/<name>_<frame>.png.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/assets/model.hpp"
#include "alamo/assets/skeleton.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/materials/effect_loader.hpp"
#include "alamo/materials/fixed_function.hpp"
#include "alamo/materials/fxlc.hpp"
#include "alamo/materials/params.hpp"
#include "alamo/materials/programs.hpp"
#include "alamo/render/camera.hpp"
#include "alamo/render/lighting.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"
#include "alamo/render/test/gpu.hpp"
#include "ff_reference.hpp"
#include "gltf.hpp"
#include "png_decode.hpp"

using namespace alamo;
using namespace alamo::reftest;
using render::Mat3x4;

namespace {

using ::alamo::test::g_fail;

const std::string kSrcDir = ALAMO_RENDER_SOURCE_DIR;

// ------------------------------------------------------------------------------------------- CPU

void test_reference_model() {
    // MODULATE2X saturates; SRCALPHA/INVSRCALPHA blend.
    const auto effects = reference_effects();
    CHECK(effects.size() == 8);
    const RefEffect* ma = nullptr;
    for (const auto& e : effects)
        if (e.name == "MeshAlpha") ma = &e;
    CHECK(ma != nullptr);
    if (!ma) return;
    RefParams p;
    p.diffuse = {1, 1, 1, 0.5};
    RefLights l;
    l.dir[0][1] = -1;  // towards -Y
    l.diffuse[0][0] = l.diffuse[0][1] = l.diffuse[0][2] = 1;
    const double n[3] = {0, -1, 0};
    const Vec4 tex[2] = {Vec4{0.75, 0.25, 0.5, 1.0}, Vec4{0, 0, 0, 0}};
    const Vec4 out = reference_pixel(*ma, p, l, tex, Vec4{1, 1, 1, 1}, n, Vec4{0, 0, 0, 0});
    // diffuse = Md * Ld * 1 = 1 (alpha 0.5); colour = sat(2 * tex * 1); alpha = tex.a * 0.5; blend over black.
    CHECK(std::fabs(out[0] - 1.0 * 0.5) < 1e-9 && std::fabs(out[1] - 0.5 * 0.5) < 1e-9);
    CHECK(std::fabs(out[3] - 0.5 * 0.5) < 1e-9);  // alpha: 0.5 * 0.5 + 0 * 0.5
}

// alPrimVert: RENDER.md 1.4 dynamic FVF buffers (XYZ|DIFFUSE|TEX1 24 B, XYZ|NORMAL|DIFFUSE|TEX1 36 B).
void test_prim_vertex_layout() {
    const materials::VertexLayoutDesc a = materials::vertex_layout_desc(materials::VertexLayout::PrimVert);
    CHECK(a.stride == 24 && a.attributes.size() == 3);
    for (const auto& at : a.attributes) {
        if (at.location == materials::kLocPosition) CHECK(at.offset == 0);
        if (at.location == materials::kLocColor0) CHECK(at.offset == 12 && at.vk_format == materials::vkfmt::kB8G8R8A8Unorm);
        if (at.location == materials::kLocTexcoord0) CHECK(at.offset == 16);
    }
    const materials::VertexLayoutDesc b = materials::vertex_layout_desc(materials::VertexLayout::PrimVertLit);
    CHECK(b.stride == 36 && b.attributes.size() == 4);
    for (const auto& at : b.attributes) {
        if (at.location == materials::kLocNormal) CHECK(at.offset == 12);
        if (at.location == materials::kLocColor0) CHECK(at.offset == 24 && at.vk_format == materials::vkfmt::kB8G8R8A8Unorm);
        if (at.location == materials::kLocTexcoord0) CHECK(at.offset == 28);
    }
}

void test_readers() {
    // Inflate a stored block + a fixed-Huffman stream (zlib header 78 01).
    const uint8_t stored[] = {0x78, 0x01, 0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c', 0, 0, 0, 0};
    std::vector<uint8_t> out;
    CHECK(zlib_inflate(stored, out) && out.size() == 3 && out[0] == 'a' && out[2] == 'c');
    // Round trip through the renderer's PNG writer (stored deflate) and our decoder.
    std::vector<uint8_t> rgba(4 * 3 * 2);
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<uint8_t>(i * 9);
    const std::vector<uint8_t> file = render::encode_png_rgba(3, 2, rgba);
    Image img;
    CHECK(decode_png(file, img) && img.width == 3 && img.height == 2 && img.rgba == rgba);
    Json j;
    CHECK(parse_json("{\"a\":[1,2.5,{\"b\":true}],\"c\":\"x\\\"y\"}", j));
    CHECK(j.get("a") && j.get("a")->arr.size() == 3 && j.get("a")->arr[1].num == 2.5 && j.get("c")->str == "x\"y");
}

// ------------------------------------------------------------------------------------------- GPU

using Gpu = render::test::GpuCore;

using Offscreen = render::test::OffscreenWithDepth;  // alamo/render/offscreen.hpp

bool render_frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm,
                  std::span<const render::Instance> inst, render::GpuModel* resident, const float clear[4],
                  render::DrawStats* stats = nullptr) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    if (resident) r.make_resident(*resident);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.depth = o.depth.handle;
    pt.depth_view = o.depth.view;
    pt.extent = {o.w, o.h};
    std::memcpy(pt.clear, clear, sizeof pt.clear);
    pt.color_after = rhi::ImageState::TransferSrc;
    render::begin_pass(fn, cmd, pt);
    const bool drawn = r.draw(*f, cmd, vm, inst, render::DrawMode::Lit, stats);
    render::end_pass(fn, cmd, pt);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {o.w, o.h, 1};
    fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
    const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                          VK_ACCESS_2_HOST_READ_BIT);
    rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    if (acq.wait_value) fs.waits = {&wait, 1};
    if (!g.ring.end_frame(*f, fs)) return false;
    g.ring.wait_idle();
    g.alloc.invalidate(o.readback.alloc);
    return drawn;
}

// 4x4 double inverse (Gauss-Jordan); returns false when singular.
bool invert4(const double a[4][4], double out[4][4]) {
    double m[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? a[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        if (std::fabs(m[piv][c]) < 1e-15) return false;
        for (int k = 0; k < 8; ++k) std::swap(m[c][k], m[piv][k]);
        const double d = m[c][c];
        for (int k = 0; k < 8; ++k) m[c][k] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c + 4];
    return true;
}

// Pixel centre -> point on the plane y = 0 (negative-height viewport: row 0 is NDC y = +1).
bool pixel_to_plane(const render::ViewMatrices& vm, double px, double py, double world[3]) {
    double vp[4][4], inv[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) vp[r][c] = vm.view_proj.m[r][c];
    if (!invert4(vp, inv)) return false;
    const double nx = 2.0 * (px + 0.5) / vm.width - 1.0, ny = 1.0 - 2.0 * (py + 0.5) / vm.height;
    double p[2][3];
    for (int k = 0; k < 2; ++k) {
        const double clip[4] = {nx, ny, static_cast<double>(k), 1.0};
        double h[4];
        for (int r = 0; r < 4; ++r) h[r] = inv[r][0] * clip[0] + inv[r][1] * clip[1] + inv[r][2] * clip[2] + inv[r][3] * clip[3];
        for (int r = 0; r < 3; ++r) p[k][r] = h[r] / h[3];
    }
    const double dy = p[1][1] - p[0][1];
    if (std::fabs(dy) < 1e-12) return false;
    const double t = -p[0][1] / dy;
    for (int r = 0; r < 3; ++r) world[r] = p[0][r] + t * (p[1][r] - p[0][r]);
    return true;
}

// Logical 4x4 texture stored as 8x8 (each texel duplicated 2x2) so that bilinear samples in the
// central half of a logical texel are exact.
struct TexPattern {
    std::array<std::array<uint8_t, 4>, 16> texel{};
    std::shared_ptr<assets::TextureAsset> make(const std::string& name) const {
        auto t = std::make_shared<assets::TextureAsset>();
        t->name = name;
        t->format = assets::TextureFormat::RGBA8;
        t->width = t->height = 8;
        t->mip_levels = 1;
        t->data.resize(8 * 8 * 4);
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                std::memcpy(&t->data[(y * 8 + x) * 4], texel[(y / 2) * 4 + x / 2].data(), 4);
        t->levels.push_back({0, 0, 8, 8, 0, t->data.size()});
        return t;
    }
    // Logical texel for wrapped uv, or -1 when uv is too close to a texel border.
    int lookup(double u, double v) const {
        const double fu = (u - std::floor(u)) * 4.0, fv = (v - std::floor(v)) * 4.0;
        const double du = fu - std::floor(fu), dv = fv - std::floor(fv);
        if (du < 0.3 || du > 0.7 || dv < 0.3 || dv > 0.7) return -1;
        return static_cast<int>(std::floor(fv)) * 4 + static_cast<int>(std::floor(fu));
    }
    Vec4 value(int i) const {
        const auto& t = texel[static_cast<size_t>(i)];
        return {t[0] / 255.0, t[1] / 255.0, t[2] / 255.0, t[3] / 255.0};
    }
};

void add_quad(assets::ModelAsset& m, const std::string& name, float x0, float x1, float z0, float z1, const uint8_t col[4],
              bool with_color, const std::string& effect, const std::vector<assets::MaterialParam>& params) {
    assets::MeshInfo mi;
    mi.name = name;
    mi.first_submesh = static_cast<uint32_t>(m.submeshes.size());
    mi.submesh_count = 1;
    m.meshes.push_back(mi);
    assets::Submesh sm;
    sm.mesh = static_cast<uint32_t>(m.meshes.size() - 1);
    sm.vertex_format = with_color ? "alD3dVertNU2C" : "alD3dVertNU2";
    sm.format.known = true;
    sm.format.uv_sets = 1;
    sm.format.color = with_color;
    sm.layout = assets::VertexLayout::Static;
    const float px[4] = {x0, x1, x1, x0}, pz[4] = {z0, z0, z1, z1};
    const float u[4] = {0, 1, 1, 0}, v[4] = {1, 1, 0, 0};
    sm.vertex_count = 4;
    sm.vertices.resize(4 * 64);
    for (int i = 0; i < 4; ++i) {
        assets::StaticVertex st{};
        st.position[0] = px[i];
        st.position[2] = pz[i];
        st.normal[1] = -1.0f;
        st.tangent[0] = 1.0f;
        st.binormal[2] = 1.0f;
        st.uv0[0] = u[i];
        st.uv0[1] = v[i];
        std::memcpy(st.color, col, 4);
        std::memcpy(sm.vertices.data() + i * 64, &st, 64);
    }
    const uint16_t ccw[6] = {0, 1, 2, 0, 2, 3};  // counter-clockwise seen from the camera on -Y
    sm.index_type = assets::IndexType::U16;
    sm.index_count = 6;
    sm.indices.resize(12);
    std::memcpy(sm.indices.data(), ccw, 12);
    sm.bounds = {{x0, 0, z0}, {x1, 0, z1}};
    sm.sphere = {{0.5f * (x0 + x1), 0, 0.5f * (z0 + z1)}, 0.8f};
    sm.material.effect = effect;
    sm.material.params = params;
    m.submeshes.push_back(std::move(sm));
}

assets::MaterialParam fparam(const char* n, float x, float y = 0, float z = 0, float w = 0) {
    assets::MaterialParam p;
    p.name = n;
    p.type = assets::ParamType::Float4;
    p.value = {x, y, z, w};
    return p;
}
assets::MaterialParam tparam(const char* n, const char* tex) {
    assets::MaterialParam p;
    p.name = n;
    p.type = assets::ParamType::Texture;
    p.texture = tex;
    return p;
}

// Mirrors renderer.cpp param_store(): ALO material values by name.
materials::ParamStore store_for(const assets::MaterialRef& m) {
    materials::ParamStore s;
    for (const auto& p : m.params) {
        if (p.type == assets::ParamType::Texture) continue;
        if (p.type == assets::ParamType::Float) s.set(p.name, {p.value.x});
        else if (p.type == assets::ParamType::Int) s.set(p.name, {static_cast<float>(p.int_value)});
        else s.set(p.name, {p.value.x, p.value.y, p.value.z, p.value.w});
    }
    return s;
}

struct FfResult {
    std::string effect;
    double max_err = 0;   // in 1/255 units
    uint64_t compared = 0;
    bool ok = false;
};

// One effect: 4 quads with different vertex colours / material parameters, two LIGHT_SCALE settings.
FfResult run_ff_effect(Gpu& g, render::EffectCache& effects, const RefEffect& re) {
    FfResult res;
    res.effect = re.name;
    const auto fx = effects.get(re.name + ".fx");
    if (!fx) {
        std::printf("  %s: effect not found\n", re.name.c_str());
        return res;
    }
    TexPattern base, lines;
    for (int i = 0; i < 16; ++i) {
        base.texel[static_cast<size_t>(i)] = {static_cast<uint8_t>(17 * i), static_cast<uint8_t>(255 - 13 * i),
                                              static_cast<uint8_t>((i * 97) % 256), static_cast<uint8_t>(40 + 13 * i)};
        lines.texel[static_cast<size_t>(i)] = {static_cast<uint8_t>((i * 37) % 128), static_cast<uint8_t>((i * 59) % 128),
                                               static_cast<uint8_t>(i * 8), static_cast<uint8_t>(255 - i * 16)};
    }
    const uint8_t vcols[4][4] = {{255, 255, 255, 255}, {200, 100, 50, 180}, {30, 220, 140, 90}, {128, 128, 255, 255}};
    struct QuadParams {
        float diffuse[4], emissive[3], color[3], scroll[2];
    };
    const QuadParams qp[4] = {{{1, 1, 1, 1}, {0, 0, 0}, {1, 1, 1}, {0.25f, 0.5f}},
                              {{0.8f, 0.6f, 0.4f, 0.75f}, {0.1f, 0.05f, 0.0f}, {0.9f, 0.5f, 0.2f}, {-0.25f, 0.125f}},
                              {{0.3f, 0.9f, 0.5f, 0.5f}, {0.0f, 0.2f, 0.1f}, {0.2f, 0.7f, 1.0f}, {0.5f, 0.0f}},
                              {{1.0f, 0.2f, 0.7f, 0.25f}, {0.3f, 0.0f, 0.3f}, {0.6f, 0.6f, 0.6f}, {0.0f, -0.375f}}};
    auto model = std::make_shared<assets::ModelAsset>();
    model->name = "FF_" + re.name;
    model->skeleton.names = {"ROOT"};
    model->skeleton.parents = {-1};
    model->skeleton.visible = {1};
    model->skeleton.billboard_mode = {0};
    model->skeleton.bind_local = {Mat3x4{}};
    model->skeleton.finalize();
    const float xs[2][2] = {{-0.95f, -0.05f}, {0.05f, 0.95f}};
    for (int q = 0; q < 4; ++q) {
        std::vector<assets::MaterialParam> ps = {
            tparam("BaseTexture", "base.dds"), tparam("Scanlines", "lines.dds"),
            fparam("Diffuse", qp[q].diffuse[0], qp[q].diffuse[1], qp[q].diffuse[2], qp[q].diffuse[3]),
            fparam("Emissive", qp[q].emissive[0], qp[q].emissive[1], qp[q].emissive[2]),
            fparam("Color", qp[q].color[0], qp[q].color[1], qp[q].color[2]),
            fparam("UVScrollRate", qp[q].scroll[0], qp[q].scroll[1])};
        const float* x = xs[q % 2];
        const float* z = xs[1 - q / 2];
        add_quad(*model, "Q" + std::to_string(q), x[0], x[1], z[0], z[1], vcols[q], re.vertex_color, re.name + ".fx", ps);
    }
    model->bounds = {{-1, -0.1f, -1}, {1, 0.1f, 1}};
    model->sphere = {{0, 0, 0}, 1.5f};
    auto textures = [&](std::string_view name, assets::TextureRole) -> std::shared_ptr<const assets::TextureAsset> {
        if (name == "base.dds") return base.make("base.dds");
        if (name == "lines.dds") return lines.make("lines.dds");
        return nullptr;
    };

    render::Renderer r;
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.depth_format = VK_FORMAT_D32_SFLOAT;
    rd.effects = &effects;
    if (!r.init(g.ctx, g.alloc, g.up, rd)) return res;
    render::GpuModel gm;
    std::string err;
    CHECK(r.upload_model(model, textures, gm, nullptr, &err));
    for (const auto& t : gm.buffer_tickets) g.up.wait(t);
    for (const auto& t : gm.textures) g.up.wait(t.ticket);
    int base_index = -1;
    for (size_t i = 0; i < gm.textures.size(); ++i)
        if (gm.textures[i].name == "base.dds") base_index = static_cast<int>(i);

    render::Lighting light = render::Lighting::space_default();
    light.sun.direction = {0.3f, -0.8f, 0.5f};
    light.sun.color = {1.0f, 0.9f, 0.8f};
    light.sun.intensity = 0.7f;
    light.fill[0].direction = {-0.6f, -0.5f, 0.2f};
    light.fill[0].color = {0.2f, 0.3f, 0.5f};
    light.fill[0].intensity = 0.5f;
    light.fill[1].direction = {0.1f, -0.2f, -0.9f};
    light.fill[1].color = {0.6f, 0.2f, 0.1f};
    light.fill[1].intensity = 0.4f;
    light.ambient = {0.2f, 0.15f, 0.1f};
    r.set_lighting(light);
    materials::FrameCB fcb{};
    render::fill_frame_lights(light, fcb);
    RefLights rl;
    const materials::float4* dirs[3] = {&fcb.light0_vector, &fcb.light1_vector, &fcb.light2_vector};
    const materials::float4* difs[3] = {&fcb.light0_diffuse, &fcb.light1_diffuse, &fcb.light2_diffuse};
    for (int i = 0; i < 3; ++i) {
        const double d[3] = {dirs[i]->x, dirs[i]->y, dirs[i]->z};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; ++k) {
            rl.dir[i][k] = len > 0 ? d[k] / len : 0.0;
            rl.diffuse[i][k] = (&difs[i]->x)[k];
        }
    }
    rl.ambient[0] = fcb.light_ambient.x;
    rl.ambient[1] = fcb.light_ambient.y;
    rl.ambient[2] = fcb.light_ambient.z;

    Offscreen o;
    CHECK(o.create(g, 256, 256));
    render::OrbitCamera cam;
    cam.yaw = -1.5707963f;
    cam.pitch = 0.0f;
    cam.distance = 5.0f;
    cam.radius = 2.0f;
    cam.fov_x = 2.0f * std::atan(0.2f);
    const render::ViewMatrices vm = render::make_view(cam, 256, 256);
    float view_inv[16];  // row-major D3DXMATRIX (row-vector) = transpose of the column-vector rows
    for (int rr = 0; rr < 4; ++rr)
        for (int c = 0; c < 4; ++c) view_inv[rr * 4 + c] = vm.view_inv.m[c][rr];
    const float clear[4] = {0.25f, 0.5f, 0.75f, 0.5f};
    const Vec4 dst = {std::round(0.25 * 255) / 255, std::round(0.5 * 255) / 255, std::round(0.75 * 255) / 255,
                      std::round(0.5 * 255) / 255};

    const materials::float4 scales[2] = {{1, 1, 1, 1}, {0.9f, 0.7f, 0.5f, 0.6f}};
    const float times[2] = {0.0f, 0.9375f};
    res.ok = true;
    for (int variant = 0; variant < 2; ++variant) {
        materials::ExprInputs in;
        in.light_scale = scales[variant];
        in.time = times[variant];
        in.view_inv = view_inv;
        // Per-draw FF state through the renderer's per-instance FF inputs hook (Instance::ff_hook): the
        // reference's own template (its vertex-colour rule) evaluated at this variant's inputs (TIME included).
        std::vector<std::vector<std::unique_ptr<materials::FfDrawTemplate>>> tpls(gm.submeshes.size());
        for (size_t si = 0; si < gm.submeshes.size(); ++si) {
            render::GpuSubmesh& gs = gm.submeshes[si];
            CHECK(gs.lit == render::LitStatus::Lit);
            tpls[si].resize(gs.passes.size());
            for (size_t pi = 0; pi < gs.passes.size(); ++pi) {
                render::GpuPass& gp = gs.passes[pi];
                if (!gp.ff || !gp.pass || !gp.pass->ff || !gs.effect || !gs.effect->effect) continue;
                auto tpl = std::make_unique<materials::FfDrawTemplate>();
                const materials::ParamStore st = store_for(model->submeshes[gs.submesh].material);
                CHECK(tpl->build(*gp.pass->ff, *gs.effect->effect, st.lookup(), re.vertex_color));
                if (std::getenv("ALAMO_REF_VERBOSE") && variant == 0 && si == 0) {
                    const materials::FfDrawData fd = tpl->evaluate(in);
                    const materials::PsoKey dk = r.draw_key(gs, gp, in.light_scale.w);
                    const materials::ShaderBlob* vsb = render::shader_blob_by_id(dk.vs_id);
                    std::printf("  %s: vs %s layout %s sources %u %u %u %u params %.2f %.2f %.2f %.2f\n", re.name.c_str(),
                                vsb ? vsb->name : "?", materials::vertex_layout_name(dk.vertex_layout), fd.sources.x,
                                fd.sources.y, fd.sources.z, fd.sources.w, fd.params.x, fd.params.y, fd.params.z, fd.params.w);
                }
                tpls[si][pi] = std::move(tpl);
                if (gp.tex[0] < 0 && base_index >= 0) gp.tex[0] = base_index;  // particle texture (engine-bound)
            }
        }
        const render::FfDrawHook hook = [&](const render::GpuSubmesh& gs, size_t pi, const materials::ExprInputs&,
                                            materials::FfDrawData& data) {
            const size_t si = static_cast<size_t>(&gs - gm.submeshes.data());
            if (si < tpls.size() && pi < tpls[si].size() && tpls[si][pi]) tpls[si][pi]->update(in, data);
        };
        render::Instance inst;
        inst.model = &gm;
        inst.ff_hook = &hook;
        inst.light_scale = scales[variant];
        render::DrawStats ds;
        CHECK(render_frame(g, r, o, vm, {&inst, 1}, &gm, clear, &ds));
        CHECK(ds.unlit_draws == 0 && ds.pending_pipeline == 0);

        RefParams rp;
        rp.light_scale = {scales[variant].x, scales[variant].y, scales[variant].z, scales[variant].w};
        rp.time = times[variant];
        uint64_t bad = 0;
        for (uint32_t py = 0; py < o.h; ++py) {
            for (uint32_t px = 0; px < o.w; ++px) {
                double w[3];
                if (!pixel_to_plane(vm, px, py, w)) continue;
                int q = -1;
                for (int k = 0; k < 4; ++k) {
                    const float* x = xs[k % 2];
                    const float* z = xs[1 - k / 2];
                    if (w[0] > x[0] + 0.01 && w[0] < x[1] - 0.01 && w[2] > z[0] + 0.01 && w[2] < z[1] - 0.01) q = k;
                }
                const uint8_t* p = o.px(px, py);
                if (q < 0) continue;
                const float* x = xs[q % 2];
                const float* z = xs[1 - q / 2];
                const double uv0[2] = {(w[0] - x[0]) / (x[1] - x[0]), (z[1] - w[2]) / (z[1] - z[0])};
                for (int k = 0; k < 4; ++k) rp.diffuse[static_cast<size_t>(k)] = qp[q].diffuse[k];
                rp.emissive = {qp[q].emissive[0], qp[q].emissive[1], qp[q].emissive[2], 0};
                rp.color = {qp[q].color[0], qp[q].color[1], qp[q].color[2], 0};
                rp.uv_scroll[0] = qp[q].scroll[0];
                rp.uv_scroll[1] = qp[q].scroll[1];
                double uv[2][2];
                reference_texcoords(re, rp, uv0, w, uv);
                const int t0 = base.lookup(uv[0][0], uv[0][1]);
                const int t1 = re.scanlines ? lines.lookup(uv[1][0], uv[1][1]) : 0;
                if (t0 < 0 || t1 < 0) continue;
                const Vec4 tex[2] = {base.value(t0), lines.value(t1)};
                const Vec4 vc = {vcols[q][0] / 255.0, vcols[q][1] / 255.0, vcols[q][2] / 255.0, vcols[q][3] / 255.0};
                const double n[3] = {0, -1, 0};
                const Vec4 ref = reference_pixel(re, rp, rl, tex, vc, n, dst);
                double e = 0;
                for (size_t c = 0; c < 4; ++c) e = std::max(e, std::fabs(ref[c] * 255.0 - p[c]));
                res.max_err = std::max(res.max_err, e);
                ++res.compared;
                if (e > 2.0) {
                    if (bad < 3 || (std::getenv("ALAMO_REF_VERBOSE") && (px % 16) == 0 && (py % 16) == 0))
                        std::printf("  %s v%d q%d px(%u,%u) texel %d: gpu (%u,%u,%u,%u) ref (%.1f,%.1f,%.1f,%.1f)\n", re.name.c_str(),
                                    variant, q, px, py, t0, p[0], p[1], p[2], p[3], ref[0] * 255, ref[1] * 255, ref[2] * 255,
                                    ref[3] * 255);
                    ++bad;
                }
            }
        }
        if (bad) res.ok = false;
    }
    // State mapping: the resolved PSO matches the transcription.
    for (const auto& gs : gm.submeshes)
        for (const auto& gp : gs.passes) {
            const materials::PsoKey& k = gp.pass->pso[0];
            CHECK(k.blend_enable == (re.blend ? 1 : 0));
            CHECK(k.depth_write == (re.z_write ? 1 : 0));
            if (gp.ff && gp.pass->ff) CHECK(static_cast<size_t>(gp.pass->ff->active_stages) == re.stages.size());
        }
    g.ring.wait_idle();
    o.destroy(g);
    r.destroy_model(gm);
    r.shutdown();
    return res;
}

// ------------------------------------------------------------------------------------------- skinning

struct SkinFrame {
    const char* label;     // file suffix: bind / idle_f20 / attack_f10 / jetpack_f30
    const char* model;
    const char* anim;      // nullptr = bind
    int frame;
    const char* ref_png;   // docs/formats/examples/...
    bool gltf;             // compare positions against EI_ROYALGAURD.gltf (bind + its animation)
};

// Model-space positions of the render submeshes for a pose (CPU, engine/assets math).
struct PosedSubmesh {
    uint32_t submesh;
    std::vector<double> pos;  // xyz
};
std::vector<PosedSubmesh> pose_submeshes(const assets::ModelAsset& m, std::span<const Mat3x4> world) {
    std::vector<PosedSubmesh> out;
    std::vector<Mat3x4> pal;
    for (uint32_t s = 0; s < m.submeshes.size(); ++s) {
        const assets::Submesh& sm = m.submeshes[s];
        PosedSubmesh ps;
        ps.submesh = s;
        ps.pos.resize(sm.vertex_count * 3);
        if (sm.skinned()) {
            pal.assign(std::max<size_t>(sm.bone_palette.size(), 1), Mat3x4{});
            assets::compute_skin_palette(m.skeleton, world, sm.bone_palette, pal);
        }
        for (uint32_t v = 0; v < sm.vertex_count; ++v) {
            const uint8_t* vp = sm.vertices.data() + static_cast<size_t>(v) * 64;
            assets::Float3 p;
            std::memcpy(&p, vp, 12);
            Mat3x4 M = world[sm.attach_bone < world.size() ? sm.attach_bone : 0];
            if (sm.skinned()) {
                float nw;
                std::memcpy(&nw, vp + 24, 4);
                const uint32_t idx = static_cast<uint32_t>(std::max(nw, 0.0f));
                M = pal[idx < pal.size() ? idx : 0];
            }
            const assets::Float3 q = M.transform_point(p);
            ps.pos[v * 3] = q.x;
            ps.pos[v * 3 + 1] = q.y;
            ps.pos[v * 3 + 2] = q.z;
        }
        out.push_back(std::move(ps));
    }
    return out;
}

bool exported_by_alo2gltf(const assets::ModelAsset& m, const assets::Submesh& sm) {
    if (sm.index_count == 0 || m.meshes[sm.mesh].hidden) return false;
    const std::string sh = alamo::ascii_lower(sm.material.effect);
    return sh.find("shadow") == std::string::npos && sh.find("collision") == std::string::npos;
}

struct Mask {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> m;
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && static_cast<uint32_t>(x) < w && static_cast<uint32_t>(y) < h && m[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
    }
    bool bbox(int& x0, int& y0, int& x1, int& y1) const {
        x0 = y0 = 1 << 30;
        x1 = y1 = -1;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                if (m[static_cast<size_t>(y) * w + x]) {
                    x0 = std::min(x0, static_cast<int>(x));
                    y0 = std::min(y0, static_cast<int>(y));
                    x1 = std::max(x1, static_cast<int>(x));
                    y1 = std::max(y1, static_cast<int>(y));
                }
        return x1 >= 0;
    }
};

// IoU after aligning bounding-box centres and scaling uniformly to the reference's larger bbox side.
double silhouette_iou(const Mask& ref, const Mask& ours) {
    int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    if (!ref.bbox(ax0, ay0, ax1, ay1) || !ours.bbox(bx0, by0, bx1, by1)) return 0.0;
    const double sa = std::max(ax1 - ax0, ay1 - ay0) + 1.0, sb = std::max(bx1 - bx0, by1 - by0) + 1.0;
    const double s = sb / sa;  // ours pixels per reference pixel
    const double acx = 0.5 * (ax0 + ax1), acy = 0.5 * (ay0 + ay1), bcx = 0.5 * (bx0 + bx1), bcy = 0.5 * (by0 + by1);
    uint64_t inter = 0, uni = 0;
    const int pad = 4;
    for (int y = ay0 - pad - static_cast<int>(sa / 2); y <= ay1 + pad + static_cast<int>(sa / 2); ++y)
        for (int x = ax0 - pad - static_cast<int>(sa / 2); x <= ax1 + pad + static_cast<int>(sa / 2); ++x) {
            const bool a = ref.at(x, y);
            const int ox = static_cast<int>(std::lround(bcx + (x - acx) * s)), oy = static_cast<int>(std::lround(bcy + (y - acy) * s));
            const bool b = ours.at(ox, oy);
            inter += (a && b) ? 1u : 0u;
            uni += (a || b) ? 1u : 0u;
        }
    return uni ? static_cast<double>(inter) / static_cast<double>(uni) : 0.0;
}

void run_skinning(Gpu* g, const vfs::Vfs& files) {
    const std::string examples = kSrcDir + "/docs/formats/examples/";
    Gltf gltf;
    std::string err;
    const bool have_gltf = gltf.load(examples + "EI_ROYALGAURD.gltf", &err);
    CHECK(have_gltf);
    if (!have_gltf) std::printf("  glTF: %s\n", err.c_str());
    std::printf("skinning: glTF %zu primitives, %zu joints, animation '%s' (%d frames)\n", gltf.primitives.size(),
                gltf.skin_joints.size(), gltf.animation_name.c_str(), gltf.animation_frames);

    const SkinFrame frames[] = {
        {"bind", "EI_ROYALGAURD", nullptr, 0, "EI_ROYALGAURD_bind.png", true},
        {"idle_f20", "EI_ROYALGAURD", "EI_ROYALGAURD_IDLE_00", 20, "EI_ROYALGAURD_idle_f20.png", true},
        {"attack_f10", "EI_ROYALGAURD", "EI_ROYALGAURD_ATTACK_00", 10, "EI_ROYALGAURD_attack_f10.png", false},
        {"jetpack_f30", "EI_DARKTROOP_THREE", "EI_DARKTROOP_THREE_JETPACK_00", 30, "EI_DARKTROOP_THREE_jetpack_f30.png", false},
    };
    assets::AssetCache cache(files);
    render::EffectCache effects(&files);
    std::unique_ptr<render::Renderer> rr;
    if (g) {
        rr = std::make_unique<render::Renderer>();
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.depth_format = VK_FORMAT_D32_SFLOAT;
        rd.effects = &effects;
        if (!rr->init(g->ctx, g->alloc, g->up, rd)) rr.reset();
    }
    std::map<std::string, render::GpuModel> uploaded;
    std::filesystem::create_directories(kSrcDir + "/logs/reference");

    for (const SkinFrame& f : frames) {
        const auto model = cache.model(f.model);
        if (!model) {
            std::printf("  %s_%s: model %s not in the install (skipped)\n", f.model, f.label, f.model);
            continue;
        }
        std::shared_ptr<const assets::AnimationClip> clip;
        if (f.anim) {
            clip = cache.animation(f.anim);
            if (!clip) {
                std::printf("  %s_%s: animation %s not in the install (skipped)\n", f.model, f.label, f.anim);
                continue;
            }
        }
        const assets::Skeleton& sk = model->skeleton;
        std::vector<Mat3x4> world(sk.size());
        if (clip) {
            assets::Pose pose;
            assets::sample(sk, *clip, static_cast<float>(f.frame) / clip->fps, false, pose);
            assets::compute_world(sk, pose.local, world);
        } else {
            world = sk.bind_world;
        }
        const double radius = std::max(1e-3f, model->sphere.radius);

        // (a) geometric comparison against the glTF's own skin + animation.
        if (f.gltf && have_gltf) {
            const bool anim_ok = !clip || gltf.animation_name.empty() ||
                                 alamo::iequals(gltf.animation_name, f.anim);
            CHECK(anim_ok);
            const auto ref = gltf.posed_positions(clip ? f.frame : -1, true);
            const auto ours = pose_submeshes(*model, world);
            size_t next = 0, matched = 0;
            double max_err = 0;
            uint64_t verts = 0;
            for (size_t pi = 0; pi < gltf.primitives.size(); ++pi) {
                const size_t nv = gltf.primitives[pi].positions.size() / 3;
                // next exported submesh with the same vertex count and the same stored position
                while (next < ours.size()) {
                    const assets::Submesh& sm = model->submeshes[ours[next].submesh];
                    float p0[3];
                    std::memcpy(p0, sm.vertices.data(), 12);
                    if (exported_by_alo2gltf(*model, sm) && sm.vertex_count == nv && nv > 0 &&
                        std::fabs(p0[0] - gltf.primitives[pi].positions[0]) < 1e-4f &&
                        std::fabs(p0[1] - gltf.primitives[pi].positions[1]) < 1e-4f &&
                        std::fabs(p0[2] - gltf.primitives[pi].positions[2]) < 1e-4f)
                        break;
                    ++next;
                }
                if (next >= ours.size()) break;
                const auto& a = ref[pi];
                const auto& b = ours[next].pos;
                for (size_t i = 0; i < a.size(); ++i) max_err = std::max(max_err, std::fabs(a[i] - b[i]));
                verts += nv;
                ++matched;
                ++next;
            }
            std::printf("  %s_%s: positions vs glTF: %zu/%zu primitives, %llu vertices, max error %.3g (%.3g x radius %.3g)\n",
                        f.model, f.label, matched, gltf.primitives.size(), static_cast<unsigned long long>(verts), max_err,
                        max_err / radius, radius);
            CHECK(matched == gltf.primitives.size());
            CHECK(max_err < 1e-3 * radius);
        }

        // (b) visual: three views like render_gltf.py (3/4: elev 25 azim -60, top: 90/-90, side: 0/-90).
        if (!rr) continue;
        auto it = uploaded.find(f.model);
        if (it == uploaded.end()) {
            render::GpuModel gm;
            auto texsrc = [&](std::string_view name, assets::TextureRole role) { return cache.texture(name, role); };
            if (!rr->upload_model(model, texsrc, gm, nullptr, &err)) {
                std::printf("  %s: upload failed: %s\n", f.model, err.c_str());
                continue;
            }
            for (const auto& t : gm.buffer_tickets) g->up.wait(t);
            for (const auto& t : gm.textures) g->up.wait(t.ticket);
            it = uploaded.emplace(f.model, std::move(gm)).first;
        }
        render::GpuModel& gm = it->second;
        Image refimg;
        const bool have_ref = read_png(examples + f.ref_png, refimg, &err);
        CHECK(have_ref);
        const uint32_t W = 600, H = 640;
        std::vector<uint8_t> composite(static_cast<size_t>(3 * W) * H * 4, 255);
        const float views[3][2] = {{25, -60}, {88.5f, -90}, {0, -90}};
        const char* names[3] = {"3/4", "top", "side"};
        std::printf("  %s_%s silhouette IoU:", f.model, f.label);
        for (int v = 0; v < 3; ++v) {
            Offscreen o;
            if (!o.create(*g, W, H)) break;
            render::OrbitCamera cam;
            const assets::Aabb bb = gm.drawn_bounds();
            const assets::Float3 c{0.5f * (bb.min.x + bb.max.x), 0.5f * (bb.min.y + bb.max.y), 0.5f * (bb.min.z + bb.max.z)};
            cam.fov_x = 0.25f;
            cam.yaw = views[v][1] * 3.14159265f / 180.0f;
            cam.pitch = views[v][0] * 3.14159265f / 180.0f;
            cam.frame(c, std::max(1e-3f, gm.drawn_sphere().radius), static_cast<float>(W) / static_cast<float>(H));
            const render::ViewMatrices vm = render::make_view(cam, W, H);
            render::Instance inst;
            inst.model = &gm;
            inst.bone_world = world;
            const float white[4] = {1, 1, 1, 1};
            CHECK(render_frame(*g, *rr, o, vm, {&inst, 1}, &gm, white));
            Mask ours{W, H, std::vector<uint8_t>(static_cast<size_t>(W) * H, 0)}, refm;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    const uint8_t* p = o.px(x, y);
                    ours.m[static_cast<size_t>(y) * W + x] = (p[0] < 245 || p[1] < 245 || p[2] < 245) ? 1 : 0;
                    std::memcpy(&composite[(static_cast<size_t>(y) * 3 * W + static_cast<size_t>(v) * W + x) * 4], p, 4);
                }
            double iou = 0;
            if (have_ref && refimg.width >= 3 * W / 2) {
                const uint32_t pw = refimg.width / 3;
                refm.w = pw;
                refm.h = refimg.height;
                refm.m.assign(static_cast<size_t>(pw) * refimg.height, 0);
                for (uint32_t y = 60; y < refimg.height; ++y)  // skip the titles
                    for (uint32_t x = 0; x < pw; ++x) {
                        const uint8_t* p = refimg.px(static_cast<uint32_t>(v) * pw + x, y);
                        refm.m[static_cast<size_t>(y) * pw + x] = (p[0] < 235 || p[1] < 235 || p[2] < 235) ? 1 : 0;
                    }
                iou = silhouette_iou(refm, ours);
            }
            std::printf(" %s %.3f", names[v], iou);
            CHECK(iou > 0.6);
            g->ring.wait_idle();
            o.destroy(*g);
        }
        std::printf("\n");
        const std::string png = kSrcDir + "/logs/reference/" + f.model + "_" + f.label + ".png";
        CHECK(render::write_png_rgba(png, 3 * W, H, composite));
        std::printf("    wrote %s\n", png.c_str());
    }
    if (rr) {
        g->ring.wait_idle();
        for (auto& [n, gm] : uploaded) rr->destroy_model(gm);
        rr->shutdown();
    }
}

// Per-draw FF / fade evaluation on the real FXOs (CPU): the FXO expressions re-evaluated per draw.
void test_per_draw_fxo(const vfs::Vfs& files) {
    materials::EffectLibrary lib(files);
    auto alpha = lib.load("MeshAlpha.fx");
    CHECK(alpha.has_value() && !alpha->passes.empty() && alpha->passes[0].ff.has_value());
    if (alpha && !alpha->passes.empty() && alpha->passes[0].ff) {
        materials::ParamStore st;
        st.set("Diffuse", {0.8f, 0.6f, 0.4f, 0.9f});
        st.set("Emissive", {0.1f, 0.2f, 0.3f});
        materials::FfDrawTemplate tpl;
        CHECK(tpl.build(*alpha->passes[0].ff, *alpha->effect, st.lookup(), false));
        CHECK(tpl.dynamic());
        materials::ExprInputs in;
        in.light_scale = {0.5f, 1.0f, 0.25f, 0.4f};
        const materials::FfDrawData d = tpl.evaluate(in);
        // MaterialDiffuse = m_lightScale * Diffuse; MaterialAmbient = Diffuse; MaterialEmissive.xyz = Emissive
        CHECK(std::fabs(d.mat_diffuse.x - 0.4f) < 1e-6f && std::fabs(d.mat_diffuse.y - 0.6f) < 1e-6f &&
              std::fabs(d.mat_diffuse.z - 0.1f) < 1e-6f && std::fabs(d.mat_diffuse.w - 0.36f) < 1e-6f);
        CHECK(std::fabs(d.mat_ambient.x - 0.8f) < 1e-6f && std::fabs(d.mat_ambient.w - 0.9f) < 1e-6f);
        CHECK(std::fabs(d.mat_emissive.z - 0.3f) < 1e-6f);
        // MeshAlpha is always blended (no fade switch): both PSO variants blend SRCALPHA/INVSRCALPHA.
        CHECK(!alpha->passes[0].fade_dynamic && alpha->passes[0].select_exact(in).blend_enable == 1);
        std::printf("per-draw: MeshAlpha LIGHT_SCALE (0.5,1,0.25,0.4) -> MaterialDiffuse (%.3f %.3f %.3f %.3f)\n",
                    d.mat_diffuse.x, d.mat_diffuse.y, d.mat_diffuse.z, d.mat_diffuse.w);
    }
    auto add = lib.load("MeshAdditive.fx");  // best technique DX8 (hand-written program); FF fallback technique
    CHECK(add.has_value() && !add->passes.empty() && !add->passes[0].fixed_function);
    auto bump = lib.load("MeshBumpColorize.fx");
    CHECK(bump.has_value() && !bump->passes.empty() && bump->passes[0].fade_expr != nullptr);
    if (bump && !bump->passes.empty()) {
        const materials::ResolvedPass& p = bump->passes[0];
        materials::ExprInputs in;
        for (float w : {1.0f, 0.999f, 0.5f, 0.0f, 1.5f}) {
            in.light_scale.w = w;
            CHECK(p.blend_enabled(in) == (w < 1.0f));  // AlphaBlendEnable = (m_lightScale.w < 1)
            CHECK(p.select_exact(in).blend_enable == (w < 1.0f ? 1 : 0));
        }
    }
    auto scroll = lib.load("MeshAlphaScroll.fx");
    if (scroll && !scroll->passes.empty() && scroll->passes[0].ff) {
        materials::ParamStore st;
        st.set("UVScrollRate", {0.25f, -0.5f});
        materials::FfDrawTemplate tpl;
        CHECK(tpl.build(*scroll->passes[0].ff, *scroll->effect, st.lookup(), false));
        materials::ExprInputs in;
        in.time = 2.0f;
        const materials::FfDrawData d = tpl.evaluate(in);
        CHECK(std::fabs(d.tex_transform[0][0].z - 0.5f) < 1e-6f && std::fabs(d.tex_transform[0][1].z + 1.0f) < 1e-6f);
    }
}
int run_gpu_and_data() {
    const bool strict = render::setup_validation_env(kSrcDir);
    const char* root = std::getenv("ALAMO_FOC_DATA");
    std::unique_ptr<vfs::Vfs> files;
    if (root && *root) {
        vfs::VfsConfig cfg;
        cfg.foc_root = root;
        std::string err;
        files = vfs::Vfs::create(cfg, &err);
        CHECK(files != nullptr);
    } else {
        std::printf("data: ALAMO_FOC_DATA not set, FXO and skinning parts skipped\n");
    }
    Gpu g;
    bool gpu_ok = false;
    {
        rhi::Loader probe;
        if (probe.open()) {
            rhi::ContextDesc cd;
            cd.app_name = "alamo_render_reference_tests";
            cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
            gpu_ok = g.ctx.init(cd);
        }
    }
    if (!gpu_ok) std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
    if (gpu_ok) {
        std::printf("gpu: %s, validation %s%s\n", g.ctx.caps().name.c_str(), g.ctx.validation_enabled() ? "on" : "off",
                    strict ? " (required)" : "");
        CHECK(!strict || g.ctx.validation_enabled());
        rhi::FrameRingDesc fd;
        fd.frames_in_flight = 2;
        fd.uniform_bytes = 8ull << 20;
        CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    }

    if (files && gpu_ok) {
        render::EffectCache effects(files.get());
        std::printf("ff: GPU vs CPU D3D9 reference (max error in 1/255, tolerance 2):\n");
        for (const RefEffect& re : reference_effects()) {
            const FfResult r = run_ff_effect(g, effects, re);
            std::printf("  %-20s %s  max %.2f over %llu pixels\n", re.name.c_str(), r.ok ? "ok  " : "FAIL", r.max_err,
                        static_cast<unsigned long long>(r.compared));
            CHECK(r.ok && r.compared > 1000);
        }
    }
    if (files) test_per_draw_fxo(*files);
    if (files) run_skinning(gpu_ok ? &g : nullptr, *files);

    if (gpu_ok) {
        g.ring.wait_idle();
        g.ring.shutdown();
        g.up.shutdown();
        g.alloc.shutdown();
        const uint32_t errors = g.ctx.validation_errors(), warnings = g.ctx.validation_warnings();
        std::printf("gpu: validation %s, %u errors, %u warnings\n", g.ctx.validation_enabled() ? "on" : "off", errors, warnings);
        CHECK(errors == 0 && warnings == 0);
        g.ctx.shutdown();
    }
    return 0;
}

}  // namespace

int main() {
    test_reference_model();
    test_prim_vertex_layout();
    test_readers();
    std::printf("cpu: %s\n", g_fail ? "FAILED" : "ok");
    run_gpu_and_data();
    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}

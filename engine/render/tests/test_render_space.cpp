// alamo_render_space_tests - space background scene ordering (no framework; non-zero exit on failure).
//   * Real data (ALAMO_FOC_DATA, skipped without it or without a Vulkan 1.3 device): _MP_SPACE_ENDOR's background
//     scene (SPACE_AND_OPEN.md D.7 3: planets occlude the nebula / star domes by depth). Each planet is drawn alone and
//     then with the whole background scene; inside the planet's disc the full scene must match the planet alone (the
//     additive nebula dome lies behind it). PNGs: logs\render_space_order_*.png.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/lod.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/png.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/shaderxlat/interpreter.hpp"
#include "alamo/assets/vertex_layout.hpp"
#include "alamo/vfs/vfs.hpp"

using namespace alamo;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static std::string env_or_empty(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
};

struct Offscreen {
    rhi::Image color, depth;
    rhi::Buffer readback;
    uint32_t w = 0, h = 0;
    bool create(Gpu& g, uint32_t width, uint32_t height) {
        w = width;
        h = height;
        rhi::ImageDesc cd;
        cd.format = rhi::Format::RGBA8;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        rhi::ImageDesc dd = cd;
        dd.format = rhi::Format::D32F;
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return g.alloc.create_image(cd, color) && g.alloc.create_image(dd, depth) &&
               g.alloc.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback,
                                     readback);
    }
    void destroy(Gpu& g) {
        g.alloc.destroy(color);
        g.alloc.destroy(depth);
        g.alloc.destroy(readback);
    }
    std::vector<uint8_t> pixels() const {
        const auto* p = static_cast<const uint8_t*>(readback.mapped());
        return std::vector<uint8_t>(p, p + static_cast<size_t>(w) * h * 4);
    }
};

static bool render_frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst,
                         render::DrawStats& stats, const render::RecordOptions& ro, std::span<const render::Instance> sky, const float* clear) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.depth = o.depth.handle;
    pt.depth_view = o.depth.view;
    pt.extent = {o.w, o.h};
    std::memcpy(pt.clear, clear, sizeof pt.clear);
    pt.color_after = rhi::ImageState::TransferSrc;
    const bool drawn = r.render(*f, cmd, pt, vm, inst, render::DrawMode::Lit, ro, &stats, nullptr, sky);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {o.w, o.h, 1};
    fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
    const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                          VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    if (acq.wait_value) fs.waits = {&wait, 1};
    if (!g.ring.end_frame(*f, fs)) return false;
    g.ring.wait_idle();
    g.alloc.invalidate(o.readback.alloc);
    return drawn;
}

// Renders until nothing waits on a pipeline / translation (bounded).
static bool settle(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst,
                   const render::RecordOptions& ro, std::span<const render::Instance> sky, const float* clear, render::DrawStats& st) {
    for (int k = 0; k < 200; ++k) {
        if (!render_frame(g, r, o, vm, inst, st, ro, sky, clear)) return false;
        if (k > 1 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0) return true;
        r.wait_translations();
        r.pipelines().wait_idle();
    }
    return false;
}

static void write_png(const std::string& path, uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    CHECK(render::write_png_rgba(path, w, h, rgba));
}

static std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static const char* bucket_name(render::PhaseBucket b) {
    switch (b) {
    case render::PhaseBucket::Terrain: return "Terrain";
    case render::PhaseBucket::TerrainMesh: return "TerrainMesh";
    case render::PhaseBucket::Fissure: return "Fissure";
    case render::PhaseBucket::Occluded: return "Occluded";
    case render::PhaseBucket::Opaque: return "Opaque";
    case render::PhaseBucket::Shadow: return "Shadow";
    case render::PhaseBucket::Water: return "Water";
    case render::PhaseBucket::Transparent: return "Transparent";
    case render::PhaseBucket::Bloom: return "Bloom";
    case render::PhaseBucket::Heat: return "Heat";
    default: return "other";
    }
}

static std::string stem_lower(const std::string& path) {
    std::string s = path;
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static void print_model(const char* what, const render::Instance& in, size_t count) {
    const render::GpuModel* m = in.model;
    const render::Mat3x4& t = in.transform;
    const float sc = std::sqrt(t.m[0][0] * t.m[0][0] + t.m[1][0] * t.m[1][0] + t.m[2][0] * t.m[2][0]);
    std::printf("  %s %s x%zu (first at %.0f %.0f %.0f, scale %.1f)\n", what, m->asset->name.c_str(), count, static_cast<double>(t.m[0][3]),
                static_cast<double>(t.m[1][3]), static_cast<double>(t.m[2][3]), static_cast<double>(sc));
    for (const render::GpuSubmesh& s : m->submeshes) {
        const materials::ResolvedPass* p = s.passes.empty() ? nullptr : s.passes.front().pass;
        std::printf("    %s: %s, %s", s.mesh < m->asset->meshes.size() ? m->asset->meshes[s.mesh].name.c_str() : "?", s.effect_name.c_str(),
                    bucket_name(s.bucket));
        if (p) std::printf(", z test %u write %u, blend %u", p->pso[0].depth_test, p->pso[0].depth_write, p->pso[0].blend_enable);
        std::printf("\n");
    }
}

// Pixels lit (luma > 8) in `px`, with their 6-pixel neighbourhood lit too (skips anti-aliased limbs).
static std::vector<uint8_t> lit_mask(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    auto lit = [&](uint32_t x, uint32_t y) {
        const uint8_t* q = px.data() + 4 * (static_cast<size_t>(y) * w + x);
        return (q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000 > 8;
    };
    std::vector<uint8_t> m(static_cast<size_t>(w) * h, 0);
    for (uint32_t y = 6; y + 6 < h; ++y)
        for (uint32_t x = 6; x + 6 < w; ++x)
            m[static_cast<size_t>(y) * w + x] = lit(x, y) && lit(x - 6, y) && lit(x + 6, y) && lit(x, y - 6) && lit(x, y + 6) ? 1 : 0;
    return m;
}

// Fraction of the mask's pixels whose max channel difference between `a` and `b` exceeds `tol`.
static double masked_diff(const std::vector<uint8_t>& mask, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int tol, size_t& n) {
    size_t bad = 0;
    n = 0;
    for (size_t p = 0; p < mask.size(); ++p) {
        if (!mask[p]) continue;
        int d = 0;
        for (size_t c = 0; c < 3; ++c) d = std::max(d, std::abs(int(a[4 * p + c]) - int(b[4 * p + c])));
        ++n;
        bad += d > tol ? 1u : 0u;
    }
    return n ? static_cast<double>(bad) / static_cast<double>(n) : 0.0;
}

// Nebula.fx (PROJECTILE_NEBULA_DRAW.md 2.3): the translated vertex shader, run by the interpreter on real vertices with
// the register images draws upload, gives colour = vertex RGB x LIGHT_SCALE x (2 n_z^8 + 0.1), n_z = z of the
// normalised view-space normal; alpha = the same factor.
static void check_nebula_vs(render::Renderer& rr, const render::Instance& in, const render::ViewMatrices& vm) {
    const render::GpuModel& gm = *in.model;
    const render::GpuSubmesh* gs = nullptr;
    for (const render::GpuSubmesh& s : gm.submeshes)
        if (upper(s.effect_name) == "NEBULA") gs = &s;
    CHECK(gs && !gs->passes.empty());
    if (!gs || gs->passes.empty()) return;
    const render::GpuPass& gp = gs->passes.front();
    std::printf("  nebula VS: pass %s, %s\n", gp.pass ? gp.pass->name.c_str() : "?", gp.translated ? "translated" : gp.ff ? "fixed function" : "other");
    CHECK(gp.translated && gp.pass);
    if (!gp.translated || !gp.pass) return;
    shaderxlat::Program vsp;
    std::string perr;
    CHECK(shaderxlat::parse(gp.pass->vs_bytecode, vsp, &perr));
    shaderxlat::Interpreter vsi;
    CHECK(vsi.init(vsp, gp.pass->vs_options, &perr));
    materials::VSConstants vsc;
    materials::PSConstants psc;
    CHECK(rr.translated_constants(in, *gs, gp, vm, vsc, psc));
    const materials::TranslatedProgram* tp = rr.translated_mesh_program(gs->effect, *gp.pass);
    CHECK(tp != nullptr);
    if (!tp) return;
    shaderxlat::ConstantState csv;
    for (size_t k = 0; k < 256; ++k) csv.c[k] = {vsc.c[k].x, vsc.c[k].y, vsc.c[k].z, vsc.c[k].w};
    shaderxlat::TranslatedPush push = gp.pass->translated_push;
    const assets::Submesh& sm = gm.asset->submeshes[gs->submesh];
    // World = placement x the mesh's bone (bind pose); view-space normal = view x world (3x3).
    const render::Mat3x4 world = in.transform * gm.asset->skeleton.bind_world[sm.attach_bone];
    double max_err = 0, fmin = 9, fmax = 0;
    const uint32_t step = std::max<uint32_t>(1, sm.vertex_count / 64);
    for (uint32_t vi = 0; vi < sm.vertex_count; vi += step) {
        assets::StaticVertex v;
        std::memcpy(&v, sm.vertices.data() + static_cast<size_t>(vi) * sizeof v, sizeof v);
        std::array<shaderxlat::F4, 16> inputs{};
        for (const auto& d : tp->vs.iface.inputs) {
            shaderxlat::F4 val{0, 0, 0, 0};
            switch (d.location) {
            case 0: val = {v.position[0], v.position[1], v.position[2], 1}; break;
            case 1: val = {v.normal[0], v.normal[1], v.normal[2], 1}; break;
            case 2: val = {v.uv0[0], v.uv0[1], 0, 1}; break;
            case 5: val = {v.color[0] / 255.0f, v.color[1] / 255.0f, v.color[2] / 255.0f, v.color[3] / 255.0f}; break;
            default: break;
            }
            if (d.reg < 16) inputs[d.reg] = val;
        }
        shaderxlat::VsResult r;
        CHECK(vsi.run_vs(inputs, csv, push, r));
        double nw[3], nv[3];
        for (int i = 0; i < 3; ++i)
            nw[i] = double(world.m[i][0]) * v.normal[0] + double(world.m[i][1]) * v.normal[1] + double(world.m[i][2]) * v.normal[2];
        for (int i = 0; i < 3; ++i) nv[i] = double(vm.view.m[i][0]) * nw[0] + double(vm.view.m[i][1]) * nw[1] + double(vm.view.m[i][2]) * nw[2];
        const double len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
        if (len < 1e-6) continue;
        const double nz = nv[2] / len, f = 2.0 * std::pow(nz, 8.0) + 0.1;
        fmin = std::min(fmin, f);
        fmax = std::max(fmax, f);
        // vs_1_1 oD0 is saturated to [0, 1] (D3D9 colour output), so camera-facing sheets of bright texels clamp at 1.
        const auto sat = [](double x) { return std::clamp(x, 0.0, 1.0); };
        const double want[4] = {sat(v.color[0] / 255.0 * f), sat(v.color[1] / 255.0 * f), sat(v.color[2] / 255.0 * f), sat(f)};
        const double got[4] = {r.color0[0], r.color0[1], r.color0[2], r.color0[3]};
        for (int c = 0; c < 4; ++c) max_err = std::max(max_err, std::fabs(got[c] - want[c]));
    }
    std::printf("  nebula VS: brightness factor %.3f..%.3f over the sampled vertices, max |colour - expected| %.5f\n", fmin, fmax, max_err);
    CHECK(max_err < 2e-3);
}

// One space map from its tactical start camera, post off (bloom would spread across the masks):
//   1. inventory of the background scene (In_Background placements + environment skydomes) and of the main scene's
//      distinct models (effect, bucket, depth / blend state);
//   2. per background planet (w_planet_*): its body alone (additive Glow meshes hidden) gives the body mask; the
//      whole background scene must leave the body unchanged (D.7 3: planets occlude the star / nebula domes by depth);
//   3. the main scene over the background (the frame as the game draws it) and the main-scene Nebula.fx props alone;
//      reports how much of each planet body the main scene changes (main-scene objects follow a depth-only clear,
//      RENDER.md 1.2, so they draw over the background scene).
static void space_map_order(Gpu& g, render::Renderer& rr, Offscreen& o, vfs::Vfs& files, render::GameTypeResolver& resolver,
                            assets::AssetCache& cache, jobs::JobSystem& js, const char* map, const std::string& src_dir, bool required) {
    std::string err;
    render::ModelStreamer streamer(rr, cache, js);
    render::MapScene scene;
    if (!scene.load(files, map, &resolver, serial_for, &err)) {
        std::printf("space order %s: not loaded: %s\n", map, err.c_str());
        CHECK(!required);
        return;
    }
    CHECK(scene.start(rr, streamer, cache, {}, &err));
    const render::OrbitCamera cam = scene.tactical_camera(static_cast<float>(o.w) / static_cast<float>(o.h));
    const render::ViewMatrices vm = render::make_view(cam, o.w, o.h, render::MapScene::kNear, render::MapScene::kFar);
    render::RecordOptions ro;
    scene.apply(ro);
    ro.post = false;
    const float clear[4] = {0, 0, 0, 1};
    render::DrawStats st;
    for (int k = 0; k < 3000 && !scene.ready(); ++k) {
        streamer.update();
        scene.update(rr, streamer);
        CHECK(render_frame(g, rr, o, vm, {}, st, ro, scene.sky(), clear));
    }
    CHECK(scene.ready());
    const std::vector<render::Instance> sky(scene.sky().begin(), scene.sky().end());
    const std::vector<render::Instance> main(scene.instances().begin(), scene.instances().end());
    std::printf("space order %s: camera eye (%.0f %.0f %.0f), %zu background / %zu main instances\n", map, static_cast<double>(vm.eye.x),
                static_cast<double>(vm.eye.y), static_cast<double>(vm.eye.z), sky.size(), main.size());
    // 1. Inventory.
    std::vector<size_t> planets;
    for (size_t i = 0; i < sky.size(); ++i) {
        if (!sky[i].model || !sky[i].model->asset) continue;
        print_model("background", sky[i], 1);
        if (upper(sky[i].model->asset->name).find("W_PLANET_") != std::string::npos) planets.push_back(i);
    }
    std::vector<render::Instance> nebulae, others;  // main scene: Nebula.fx props / everything else
    {
        std::vector<const render::GpuModel*> seen;
        for (const render::Instance& in : main) {
            if (!in.model || !in.model->asset) continue;
            bool nebula = false;
            for (const render::GpuSubmesh& s : in.model->submeshes) nebula = nebula || upper(s.effect_name) == "NEBULA";
            (nebula ? nebulae : others).push_back(in);
            if (std::find(seen.begin(), seen.end(), in.model) != seen.end()) continue;
            seen.push_back(in.model);
            size_t count = 0;
            for (const render::Instance& x : main) count += x.model == in.model ? 1u : 0u;
            print_model("main", in, count);
        }
    }
    CHECK(!planets.empty());
    // 2. Background order.
    CHECK(settle(g, rr, o, vm, {}, ro, sky, clear, st));
    const std::vector<uint8_t> background = o.pixels();
    write_png(src_dir + "/logs/render_space_order_" + stem_lower(map) + "_background.png", o.w, o.h, background);
    std::vector<std::vector<uint8_t>> bodies;  // per planet: body mask
    for (size_t pi : planets) {
        const render::GpuModel* m = sky[pi].model;
        std::vector<uint8_t> hidden(m->asset->meshes.size(), 0);
        for (size_t k = 0; k < m->mesh_hidden.size() && k < hidden.size(); ++k) hidden[k] = m->mesh_hidden[k];
        for (const render::GpuSubmesh& s : m->submeshes)
            if (s.bucket != render::PhaseBucket::Opaque && s.mesh < hidden.size()) hidden[s.mesh] = 1;  // Glow discs
        render::Instance body = sky[pi];
        body.mesh_hidden = hidden;
        CHECK(settle(g, rr, o, vm, {}, ro, std::span<const render::Instance>(&body, 1), clear, st));
        const std::vector<uint8_t> alone = o.pixels();
        bodies.push_back(lit_mask(alone, o.w, o.h));
        // The whole background scene vs the planet with its glow alone (same pixels inside the body).
        CHECK(settle(g, rr, o, vm, {}, ro, std::span<const render::Instance>(&sky[pi], 1), clear, st));
        const std::vector<uint8_t> planet = o.pixels();
        size_t n = 0;
        const double frac = masked_diff(bodies.back(), planet, background, 24, n);
        std::printf("space order %s: %s body: %zu px on screen, %.2f%% changed by the rest of the background scene\n", map,
                    stem_lower(m->asset->name).c_str(), n, 100.0 * frac);
        if (n > 2000) CHECK(frac < 0.02);
    }
    // 3. Main scene over the background.
    CHECK(settle(g, rr, o, vm, main, ro, sky, clear, st));
    const std::vector<uint8_t> frame = o.pixels();
    write_png(src_dir + "/logs/render_space_order_" + stem_lower(map) + "_frame.png", o.w, o.h, frame);
    CHECK(settle(g, rr, o, vm, others, ro, sky, clear, st));
    const std::vector<uint8_t> no_nebula = o.pixels();
    write_png(src_dir + "/logs/render_space_order_" + stem_lower(map) + "_frame_no_nebula.png", o.w, o.h, no_nebula);
    if (!nebulae.empty()) {
        CHECK(settle(g, rr, o, vm, nebulae, ro, {}, clear, st));
        write_png(src_dir + "/logs/render_space_order_" + stem_lower(map) + "_nebulae.png", o.w, o.h, o.pixels());
    }
    if (!nebulae.empty()) check_nebula_vs(rr, nebulae.front(), vm);
    for (size_t k = 0; k < planets.size(); ++k) {
        size_t n = 0;
        const double with = masked_diff(bodies[k], background, frame, 24, n);
        const double without = masked_diff(bodies[k], background, no_nebula, 24, n);
        std::printf("space order %s: %s body: main scene changes %.1f%% (%.1f%% without the %zu Nebula.fx props)\n", map,
                    stem_lower(sky[planets[k]].model->asset->name).c_str(), 100.0 * with, 100.0 * without, nebulae.size());
    }
    g.ring.wait_idle();
    scene.stop(rr);
    streamer.destroy_all();
}

static float dist3(const float (&a)[3], const float (&b)[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Camera at distance 1000 looking almost straight down on the origin (near 10 / far 7000): a laser at the origin flying
// along +Y is seen side-on at view depth 1000.
static render::ViewMatrices laser_camera(uint32_t w, uint32_t h, float distance = 1000.0f) {
    render::OrbitCamera cam;
    cam.target = {0, 0, 0};
    cam.yaw = 0.0f;
    cam.pitch = 1.5f;
    cam.distance = distance;
    cam.fov_x = render::tactical_fov_x(55.0f, static_cast<float>(w) / static_cast<float>(h));
    return render::make_view(cam, w, h, 10.0f, 7000.0f);
}

// PROJECTILE_NEBULA_DRAW.md 1.2 / 1.3 worked numbers (CPU geometry).
static void test_laser_geometry() {
    const render::ViewMatrices v = laser_camera(1280, 720);
    float zn = 0, zf = 0;
    render::projection_near_far(v.proj, zn, zf);
    CHECK(std::fabs(zn - 10.0f) < 1e-3f && std::fabs(zf - 7000.0f) < 0.5f);
    CHECK(std::fabs(render::laser_depth_factor(1000.0f, 10.0f, 7000.0f, 8.0f) - 2.1330f) < 1e-3f);
    CHECK(std::fabs(render::laser_depth_factor(6000.0f, 10.0f, 7000.0f, 8.0f) - 7.8558f) < 1e-3f);
    // Beam W 1 / L 6 side-on at d 1000: k = 2.133, length L + 2k = 10.27 (tail tip .. head tip), width 2k = 4.27.
    render::Laser beam;
    beam.kind = render::LaserKind::Beam;
    beam.direction = {0, 1, 0};
    beam.width = 1.0f;
    beam.length = 6.0f;
    render::PrimVertex bv[6];
    CHECK(render::laser_beam_vertices(beam, v, 8.0f, bv));
    const float blen = dist3(bv[0].pos, bv[5].pos), bwid = dist3(bv[1].pos, bv[2].pos), bcore = dist3(bv[1].pos, bv[3].pos);
    std::printf("lasers: beam 1.0/6 at d 1000: length %.2f (expect 10.27), width %.2f (4.27), core %.2f (6)\n", static_cast<double>(blen),
                static_cast<double>(bwid), static_cast<double>(bcore));
    CHECK(std::fabs(blen - 10.266f) < 0.05f && std::fabs(bwid - 4.266f) < 0.05f && std::fabs(bcore - 6.0f) < 0.05f);
    // Centred on the position: the core ends at -3 / +3 along Y, tail tip behind, head tip ahead.
    CHECK(bv[0].pos[1] < -3.0f && bv[5].pos[1] > 3.0f && std::fabs(bv[0].pos[1] + bv[5].pos[1]) < 0.05f);
    CHECK(bv[0].color == 0xFFFFFFFFu && bv[0].uv[0] == 0.25f && bv[0].uv[1] == 0.0f && bv[5].uv[0] == 0.0f && bv[5].uv[1] == 0.25f);
    beam.slot_col = 3;
    CHECK(render::laser_beam_vertices(beam, v, 8.0f, bv) && std::fabs(bv[5].uv[0] - 0.75f) < 1e-6f);  // 4 x 4 atlas cell
    // Far beam (d 6000): k = 7.856 -> 21.7 x 15.7.
    const render::ViewMatrices vfar = laser_camera(1280, 720, 6000.0f);
    beam.slot_col = 0;
    CHECK(render::laser_beam_vertices(beam, vfar, 8.0f, bv));
    std::printf("lasers: beam 1.0/6 at d 6000: length %.2f (21.7), width %.2f (15.7)\n", static_cast<double>(dist3(bv[0].pos, bv[5].pos)),
                static_cast<double>(dist3(bv[1].pos, bv[2].pos)));
    CHECK(std::fabs(dist3(bv[0].pos, bv[5].pos) - 21.71f) < 0.1f && std::fabs(dist3(bv[1].pos, bv[2].pos) - 15.71f) < 0.1f);
    // Kite W 5 / L 10 (turbolaser) side-on at d 1000: k = 5.85, nose 58.5 ahead, tail 5.85 behind, width 11.7, tinted.
    render::Laser kite;
    kite.kind = render::LaserKind::Kite;
    kite.direction = {0, 1, 0};
    kite.width = 5.0f;
    kite.length = 10.0f;
    kite.slot_col = 2;
    const uint8_t green[4] = {139, 230, 104, 255};
    std::memcpy(kite.color, green, 4);
    render::PrimVertex kv[4];
    CHECK(render::laser_kite_vertices(kite, v, 1.2f, kv));
    std::printf("lasers: kite 5/10 at d 1000: nose %.2f ahead (58.5), tail %.2f behind (5.85), width %.2f (11.7)\n",
                static_cast<double>(kv[1].pos[1]), static_cast<double>(-kv[3].pos[1]), static_cast<double>(dist3(kv[0].pos, kv[2].pos)));
    CHECK(std::fabs(kv[1].pos[1] - 58.5f) < 0.2f && std::fabs(kv[3].pos[1] + 5.85f) < 0.05f && std::fabs(dist3(kv[0].pos, kv[2].pos) - 11.7f) < 0.05f);
    CHECK(kv[0].color == 0xFF8BE668u && std::fabs(kv[1].uv[0] - 0.25f) < 1e-6f);  // D3DCOLOR A R G B; 8 x 8 atlas cell (2, 0)
    // Head-on kite: L_perp clamps to 1 -> a 2k diamond (nose k ahead on screen).
    kite.direction = {std::cos(1.5f), 0.0f, std::sin(1.5f)};  // along the view axis
    CHECK(render::laser_kite_vertices(kite, v, 1.2f, kv));
    const float k = 5.0f * render::laser_depth_factor(1000.0f, 10.0f, 7000.0f, 1.2f);
    float o0[3] = {0, 0, 0};
    CHECK(std::fabs(dist3(kv[1].pos, o0) - k) < 0.05f * k);
    // Behind the camera: nothing.
    render::Laser behind = beam;
    behind.position = {std::cos(1.5f) * 2000.0f, 0.0f, std::sin(1.5f) * 2000.0f};
    CHECK(!render::laser_beam_vertices(behind, v, 8.0f, bv));
    // Batch: beams and kites in input order, constants parsed from GameConstants text.
    render::LaserConstants lk;
    const std::string gc = "<GameConstants><Laser_Beam_Z_Scale_Factor> 6.5 </Laser_Beam_Z_Scale_Factor></GameConstants>";
    CHECK(render::parse_laser_constants(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(gc.data()), gc.size()), lk) &&
          lk.beam_z_scale == 6.5f && lk.kite_z_scale == 1.2f);
    render::LaserGeometry geo;
    const render::Laser list[3] = {beam, kite, behind};
    geo.build(list, v, {});
    CHECK(geo.beams == 1 && geo.kites == 1 && geo.skipped == 1 && geo.beam_indices.size() == 12 && geo.kite_indices.size() == 6);
}

// GPU: the same lasers drawn by Renderer::set_lasers with the shipped textures (W_Laser_Pill.tga / p_particle_master.tga)
// next to the port's previous look (W_LASER_*.ALO stretched to W x L x W, head at the position). PNGs:
// logs\render_lasers_before.png / logs\render_lasers_after.png.
static void test_lasers_gpu(Gpu& g, const std::string& src_dir) {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("lasers gpu: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    if (const std::string eaw = env_or_empty("ALAMO_EAW_DATA"); !eaw.empty()) cfg.eaw_root = eaw;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::EffectCache effects(files.get());
    assets::AssetCache cache(*files, effects.model_load_options());
    jobs::JobSystem js;
    render::Renderer rr;
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.effects = &effects;
    rd.js = &js;
    CHECK(rr.init(g.ctx, g.alloc, g.up, rd));
    rr.set_frame_ring(&g.ring);
    rr.set_lighting(render::Lighting::space_default());
    const render::TextureSource tex = [&](std::string_view n, assets::TextureRole r) { return cache.texture(n, r); };
    CHECK(rr.init_lasers(tex));
    Offscreen o;
    CHECK(o.create(g, 960, 540));
    render::LaserConstants lk;
    {
        std::vector<uint8_t> gc;
        if (files->read("Data\\XML\\GameConstants.xml", gc)) CHECK(render::parse_laser_constants(gc, lk));
    }
    CHECK(lk.beam_z_scale == 8.0f && lk.kite_z_scale == 1.2f);
    const render::ViewMatrices v = laser_camera(o.w, o.h, 300.0f);
    // A fan of lasers: red / green beams (small 1/6, medium 1.2/12, large 1.6/20) and green / red turbolaser kites.
    struct Spec {
        render::LaserKind kind;
        float w, l;
        uint8_t col;
        uint8_t rgba[4];
        const char* alo;  // the type's ALO (the old stretched look)
    };
    const Spec specs[] = {{render::LaserKind::Beam, 1.0f, 6.0f, 0, {255, 255, 255, 255}, "W_LASER_SMALL.ALO"},
                          {render::LaserKind::Beam, 1.2f, 12.0f, 3, {255, 255, 255, 255}, "W_LASER_MEDIUMG.ALO"},
                          {render::LaserKind::Beam, 1.6f, 20.0f, 0, {255, 255, 255, 255}, "W_LASER_LARGE.ALO"},
                          {render::LaserKind::Kite, 5.0f, 10.0f, 2, {139, 230, 104, 255}, "W_LASER_LARGEG.ALO"},
                          {render::LaserKind::Kite, 5.0f, 10.0f, 2, {238, 71, 54, 255}, "W_LASER_LARGE.ALO"}};
    std::vector<render::Laser> lasers;
    std::vector<std::pair<const Spec*, render::Laser>> placed;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 3; ++j) {
            const Spec& s = specs[i];
            render::Laser l;
            l.kind = s.kind;
            l.width = s.w;
            l.length = s.l;
            l.slot_col = s.col;
            std::memcpy(l.color, s.rgba, 4);
            l.position = {-72.0f + 36.0f * static_cast<float>(i), -40.0f + 40.0f * static_cast<float>(j), 0.0f};
            const float a = 1.5707963f + 0.35f * static_cast<float>(j - 1);
            l.direction = {std::cos(a), std::sin(a), 0.0f};
            lasers.push_back(l);
            placed.push_back({&s, l});
        }
    const float clear[4] = {0, 0, 0, 1};
    render::RecordOptions ro;
    ro.post = false;
    render::DrawStats st;
    rr.set_lasers(lasers, lk);
    for (int k = 0; k < 200; ++k) {
        CHECK(render_frame(g, rr, o, v, {}, st, ro, {}, clear));
        if (k > 1 && st.laser_pending == 0 && st.laser_beams + st.laser_kites == lasers.size()) break;
        rr.pipelines().wait_idle();
    }
    std::printf("lasers gpu: %u beams + %u kites drawn, %u pending; %zu beam / %zu kite vertices\n", st.laser_beams, st.laser_kites,
                st.laser_pending, rr.last_laser_geometry().beam_vertices.size(), rr.last_laser_geometry().kite_vertices.size());
    CHECK(st.laser_beams == 9 && st.laser_kites == 6 && st.laser_pending == 0);
    const std::vector<uint8_t> after = o.pixels();
    write_png(src_dir + "/logs/render_lasers_after.png", o.w, o.h, after);
    // Lit pixels around each laser: green kites green-dominant, red kites red-dominant.
    auto region_rgb = [&](const std::vector<uint8_t>& px, const render::Laser& l, double (&sum)[3]) {
        const render::Float4 c = v.view_proj * render::Float4{l.position.x, l.position.y, l.position.z, 1.0f};
        const int cx = static_cast<int>((c.x / c.w + 1.0f) * 0.5f * static_cast<float>(o.w));
        const int cy = static_cast<int>((1.0f - c.y / c.w) * 0.5f * static_cast<float>(o.h));
        size_t lit = 0;
        sum[0] = sum[1] = sum[2] = 0;
        for (int y = cy - 30; y <= cy + 30; ++y)
            for (int x = cx - 45; x <= cx + 45; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<int>(o.w) || y >= static_cast<int>(o.h)) continue;
                const uint8_t* q = px.data() + 4 * (static_cast<size_t>(y) * o.w + static_cast<size_t>(x));
                for (int ch = 0; ch < 3; ++ch) sum[ch] += q[ch];
                lit += (q[0] + q[1] + q[2]) > 60 ? 1u : 0u;
            }
        return lit;
    };
    for (size_t i = 0; i < placed.size(); i += 3) {
        double s[3];
        const size_t lit = region_rgb(after, placed[i].second, s);
        std::printf("  laser %zu (%s %.1f/%.0f): %zu lit px, rgb sum %.0f %.0f %.0f\n", i / 3,
                    placed[i].second.kind == render::LaserKind::Beam ? "beam" : "kite", static_cast<double>(placed[i].second.width),
                    static_cast<double>(placed[i].second.length), lit, s[0], s[1], s[2]);
        CHECK(lit > 10);
        if (i / 3 == 3) CHECK(s[1] > s[0] && s[1] > s[2]);
        if (i / 3 == 4) CHECK(s[0] > s[1] && s[0] > s[2]);
    }
    // Before: the type ALOs stretched (the port's previous projectile_transform: Y extent = L along the flight direction,
    // X / Z extent = W, head at the position).
    rr.set_lasers({});
    std::vector<std::unique_ptr<render::GpuModel>> models;
    std::vector<render::Instance> inst;
    for (const auto& [s, l] : placed) {
        std::shared_ptr<const assets::ModelAsset> a = cache.model(std::string("DATA\\ART\\MODELS\\") + s->alo);
        if (!a) continue;
        render::GpuModel* gm = nullptr;
        for (auto& m : models)
            if (m->asset == a) gm = m.get();
        if (!gm) {
            models.push_back(std::make_unique<render::GpuModel>());
            gm = models.back().get();
            CHECK(rr.upload_model(a, tex, *gm));
        }
        assets::Aabb b{{1, 1, 1}, {-1, -1, -1}};
        for (const assets::Submesh& sm : a->submeshes) {
            if (!sm.bounds.valid()) continue;
            const assets::Aabb sb = sm.skinned() || sm.attach_bone >= a->skeleton.bind_world.size()
                                        ? sm.bounds
                                        : assets::transform_aabb(a->skeleton.bind_world[sm.attach_bone], sm.bounds);
            b = b.valid() ? assets::merge(b, sb) : sb;
        }
        const float ex = b.valid() ? std::max(b.max.x - b.min.x, 1e-4f) : 1.0f, ey = b.valid() ? std::max(b.max.y - b.min.y, 1e-4f) : 1.0f,
                    ez = b.valid() ? std::max(b.max.z - b.min.z, 1e-4f) : 1.0f;
        const float dir[3] = {l.direction.x, l.direction.y, l.direction.z}, up[3] = {0, 0, 1};
        const float side[3] = {up[1] * dir[2] - up[2] * dir[1], up[2] * dir[0] - up[0] * dir[2], up[0] * dir[1] - up[1] * dir[0]};
        render::Mat3x4 m;
        const float pos[3] = {l.position.x, l.position.y, l.position.z};
        for (int r = 0; r < 3; ++r) {
            m.m[r][0] = -side[r] * l.width / ex;
            m.m[r][1] = dir[r] * l.length / ey;
            m.m[r][2] = up[r] * l.width / ez;
            m.m[r][3] = pos[r] - dir[r] * 0.5f * l.length;
        }
        render::Instance in;
        in.model = gm;
        in.transform = m;
        inst.push_back(in);
    }
    for (auto& m : models) rr.prepare_pipelines(*m);
    for (int k = 0; k < 200 && !inst.empty(); ++k) {
        for (auto& m : models) rr.make_resident(*m);
        CHECK(render_frame(g, rr, o, v, inst, st, ro, {}, clear));
        if (k > 1 && st.pending_pipeline == 0 && st.translation_pending == 0) break;
        rr.wait_translations();
        rr.pipelines().wait_idle();
    }
    write_png(src_dir + "/logs/render_lasers_before.png", o.w, o.h, o.pixels());
    std::printf("lasers gpu: before (stretched ALOs): %zu instances, %u draws\n", inst.size(), st.draws);
    g.ring.wait_idle();
    for (auto& m : models) rr.destroy_model(*m);
    o.destroy(g);
    rr.shutdown();
}

static void test_space_background_order(Gpu& g, const std::string& src_dir) {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("space order: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    if (const std::string eaw = env_or_empty("ALAMO_EAW_DATA"); !eaw.empty()) cfg.eaw_root = eaw;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::EffectCache effects(files.get());
    assets::AssetCache cache(*files, effects.model_load_options());
    jobs::JobSystem js;
    data::GameDatabase db;
    CHECK(db.load(*files));
    render::GameTypeResolver resolver(db);
    CHECK(resolver.load_render_xml(*files));
    render::Renderer rr;
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.effects = &effects;
    rd.js = &js;
    CHECK(rr.init(g.ctx, g.alloc, g.up, rd));
    rr.set_frame_ring(&g.ring);
    Offscreen o;
    CHECK(o.create(g, 1280, 720));
    space_map_order(g, rr, o, *files, resolver, cache, js, "_MP_SPACE_ENDOR", src_dir, true);
    space_map_order(g, rr, o, *files, resolver, cache, js, "_SPACE_PLANET_ENDOR_01", src_dir, false);
    g.ring.wait_idle();
    o.destroy(g);
    rr.shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    const std::string src_dir = ALAMO_RENDER_SOURCE_DIR;
    test_laser_geometry();
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP gpu: no Vulkan loader\n");
            return 0;
        }
    }
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_space_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        return strict ? 1 : 0;
    }
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 16ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    test_space_background_order(g, src_dir);
    test_lasers_gpu(g, src_dir);
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t ve = g.ctx.validation_errors(), vw = g.ctx.validation_warnings();
    std::printf("gpu: validation %u errors, %u warnings\n", ve, vw);
    CHECK(ve == 0 && vw == 0);
    g.ctx.shutdown();
    std::printf(g_fail ? "alamo_render_space_tests: %d failure(s)\n" : "alamo_render_space_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}

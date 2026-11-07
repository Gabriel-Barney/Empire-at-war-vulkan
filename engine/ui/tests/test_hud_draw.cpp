// alamo_ui_draw tests (P2, GPU; skips cleanly without a Vulkan 1.3 device; requires 0 validation messages when the
// project's validation layers are installed): the 3D world (debug boxes + a space selection-circle instance)
// rendered by render::Renderer, then the HUD pass on the same image after the post chain - brackets, health /
// shield bars, control-group numbers and fly-text projected at constant size, the [port] command-bar / radar
// stand-in frame with blips, the selected-unit panel, credits text (GDI glyphs on Windows) and an ortho "shell"
// model drawn through Renderer::draw from the ShellModels step - at 1024x768, 1920x1080 and 1280x1024, PNGs in
// logs/ui_hud_<w>x<h>.png. Also checks hud_ortho_view against HudOrthoCamera and the selection blob model.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "alamo/assets/texture.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/context.hpp"
#include "alamo/rhi/frame.hpp"
#include "alamo/rhi/loader.hpp"
#include "alamo/rhi/memory.hpp"
#include "alamo/rhi/upload.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/galactic_hud.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_radar.hpp"
#include "alamo/ui/hud_renderer.hpp"
#include "alamo/ui/hud_shell.hpp"
#include "alamo/ui/hud_shell_draw.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/render_bridge.hpp"
#include "alamo/ui/selection_blob.hpp"
#include "alamo/ui/tactical_hud.hpp"
#include "hud_test_data.hpp"
#ifdef ALAMO_UI_HAVE_VFS
#include <cstdlib>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/lighting.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/vfs/vfs.hpp"
#endif
#ifdef ALAMO_UI_HAVE_OBJECTS
#include "alamo/ui/hud_sim_bridge.hpp"
#endif

using namespace alamo;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void test_cpu() {
    // hud_ortho_view == HudOrthoCamera::view_proj (the components / shells cameras).
    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}, std::pair{1280u, 1024u}}) {
        const ui::HudCanvas cv = ui::HudCanvas::make(w, h);
        for (ui::HudCameraId id : {ui::kHudCameraComponents, ui::kHudCameraShells, ui::kHudCameraModels2}) {
            const render::ViewMatrices v = ui::hud_ortho_view(cv, id);
            const auto m = cv.cameras[id].view_proj();
            bool same = true;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) same = same && near(v.view_proj.m[r][c], m[static_cast<size_t>(r)][static_cast<size_t>(c)]);
            CHECK(same && v.width == w && v.height == h);
        }
    }
    // Selection blob model: unit quad with the material's effect and textures.
    terrain::ShadowBlobMaterial mat;
    mat.name = "Selection_Empire_Space";
    mat.texture = "i_selection_cutout_space.tga";
    mat.texture2 = "i_selection_empire.tga";
    mat.shader = "BlobStencilMasked.fx";
    const auto model = ui::selection_blob_model(mat);
    CHECK(model && model->submeshes.size() == 1 && model->submeshes[0].material.effect == "BlobStencilMasked.fx");
    CHECK(model && model->submeshes[0].material.find("Texture0") &&
          model->submeshes[0].material.find("Texture0")->texture == "i_selection_cutout_space.tga" &&
          model->submeshes[0].material.find("Texture1")->texture == "i_selection_empire.tga");
    ui::SelectionBlob b;
    b.center = {10, 20, 30};
    b.radius = 25;
    render::GpuModel dummy;
    const render::Instance in = ui::selection_blob_instance(dummy, b);
    CHECK(in.model == &dummy && near(in.transform.m[0][0], 25) && near(in.transform.m[1][3], 20) &&
          near(in.transform.m[2][3], 30 + ui::kSpaceBlobLift));

#ifdef ALAMO_UI_HAVE_OBJECTS
    // Snapshot bridge: dying / faded objects skipped, selection by binary search, groups, hover, team colour.
    render::InterpolatedFrame fr;
    for (uint32_t i = 1; i <= 4; ++i) {
        render::SnapshotObject o;
        o.id = static_cast<objects::ObjectId>(i);
        o.type = static_cast<objects::TypeIndex>(i % 2);
        o.owner = static_cast<int32_t>(i % 2);
        o.position = {static_cast<float>(i), 2.0f, 3.0f};
        o.health_fraction = 0.25f * static_cast<float>(i);
        o.team_color = {1.0f, 0.5f, 0.0f, 1.0f};
        if (i == 3) o.flags = render::kSnapDying;
        fr.objects.push_back(o);
    }
    const uint32_t sel[2] = {2, 4};
    std::vector<ui::HudUnit> hu;
    ui::hud_units_from_frame(
        fr, sel, [](uint32_t oid) { return static_cast<int8_t>(oid == 4 ? 7 : -1); }, 1,
        [](objects::TypeIndex t) { return t == 0 ? 12.0f : 30.0f; }, hu);
    CHECK(hu.size() == 3 && hu[0].id == 1 && hu[0].hovered && !hu[0].selected && hu[1].selected && hu[2].id == 4);
    CHECK(hu.size() == 3 && hu[2].control_group == 7 && hu[2].radius == 12.0f && hu[0].radius == 30.0f &&
          hu[1].health == 0.5f && hu[0].team_color == (ui::Rgba8{255, 128, 0, 255}) && hu[0].shield < 0.0f);
    // R5 sensor jamming: the snapshot's Is_Jammed ids reach HudUnit::jammed (the radar filter's input).
    fr.jamming.jammed = {2, 4};
    ui::hud_units_from_frame(fr, sel, {}, 0, [](objects::TypeIndex) { return 1.0f; }, hu);
    CHECK(hu.size() == 3 && !hu[0].jammed && hu[1].jammed && hu[2].jammed);
    // R5 stealth: the snapshot's hostile-stealthed ids reach HudUnit::stealthed (same radar filter).
    CHECK(!hu[0].stealthed && !hu[1].stealthed && !hu[2].stealthed);
    fr.jamming.stealthed = {1, 4};
    ui::hud_units_from_frame(fr, sel, {}, 0, [](objects::TypeIndex) { return 1.0f; }, hu);
    CHECK(hu.size() == 3 && hu[0].stealthed && !hu[1].stealthed && hu[2].stealthed);
#endif
}

// ------------------------------------------------------------------------------------------------ GPU
struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    render::Renderer renderer;
    ui::HudRenderer hud;
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
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        rhi::ImageDesc dd = cd;
        dd.format = rhi::Format::D32F;
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        return g.alloc.create_image(cd, color) && g.alloc.create_image(dd, depth) &&
               g.alloc.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     rhi::MemoryUsage::Readback, readback);
    }
    void destroy(Gpu& g) {
        g.alloc.destroy(color);
        g.alloc.destroy(depth);
        g.alloc.destroy(readback);
    }
};

// Unit box (+-0.5), effect-less (unlit stand-in), grey levels per face (render::debug_box_model lives in
// alamo_render_sim, which this test does not link).
std::shared_ptr<assets::ModelAsset> box_model() {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = "HUD_TEST_BOX.ALO";
    assets::Skeleton& sk = m->skeleton;
    sk.names = {"ROOT"};
    sk.parents = {-1};
    sk.visible = {1};
    sk.billboard_mode = {0};
    sk.bind_local = {assets::Mat3x4{}};
    sk.finalize();
    assets::MeshInfo mi;
    mi.name = "BOX";
    mi.submesh_count = 1;
    m->meshes.push_back(mi);
    assets::Submesh sm;
    sm.vertex_format = "alD3dVertNU2C";
    sm.layout = assets::VertexLayout::Static;
    struct Face {
        float n[3], u[3];
        uint8_t grey;
    };
    const Face faces[6] = {{{1, 0, 0}, {0, 1, 0}, 200},  {{-1, 0, 0}, {0, -1, 0}, 200}, {{0, 1, 0}, {-1, 0, 0}, 170},
                           {{0, -1, 0}, {1, 0, 0}, 170}, {{0, 0, 1}, {1, 0, 0}, 255},  {{0, 0, -1}, {-1, 0, 0}, 110}};
    sm.vertex_count = 24;
    sm.vertices.resize(24 * 64);
    std::vector<uint16_t> idx;
    for (int f = 0; f < 6; ++f) {
        const Face& fc = faces[f];
        const float v[3] = {fc.n[1] * fc.u[2] - fc.n[2] * fc.u[1], fc.n[2] * fc.u[0] - fc.n[0] * fc.u[2],
                            fc.n[0] * fc.u[1] - fc.n[1] * fc.u[0]};
        const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            assets::StaticVertex st{};
            for (int a = 0; a < 3; ++a) {
                st.position[a] = 0.5f * (fc.n[a] + su[k] * fc.u[a] + sv[k] * v[a]);
                st.normal[a] = fc.n[a];
            }
            st.color[0] = st.color[1] = st.color[2] = fc.grey;
            st.color[3] = 255;
            std::memcpy(sm.vertices.data() + static_cast<size_t>(f * 4 + k) * 64, &st, 64);
        }
        const auto b = static_cast<uint16_t>(f * 4);
        for (uint16_t i : {uint16_t{0}, uint16_t{1}, uint16_t{2}, uint16_t{0}, uint16_t{2}, uint16_t{3}})
            idx.push_back(static_cast<uint16_t>(b + i));
    }
    sm.index_type = assets::IndexType::U16;
    sm.index_count = static_cast<uint32_t>(idx.size());
    sm.indices.resize(idx.size() * 2);
    std::memcpy(sm.indices.data(), idx.data(), idx.size() * 2);
    sm.bounds = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    sm.sphere = {{0, 0, 0}, 0.8660254f};
    m->submeshes.push_back(std::move(sm));
    m->bounds = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    m->sphere = {{0, 0, 0}, 0.8660254f};
    return m;
}

// Synthetic shell (stage 2): the debug box on a "Empire_Faceplate_ALT0" bone scaled to a 1024 x 120 canvas-unit
// bar whose model origin is the bottom-left corner (the real shells' convention).
std::shared_ptr<assets::ModelAsset> shell_model() {
    auto m = std::make_shared<assets::ModelAsset>(*box_model());
    m->name = "I_TACTICAL_CONTROLS.ALO";
    assets::Skeleton sk;
    sk.names = {"Root", "Empire_Faceplate_ALT0"};
    sk.parents = {-1, 0};
    sk.visible = {1, 1};
    sk.billboard_mode = {0, 0};
    assets::Mat3x4 bar;
    bar.m[0][0] = 1024.0f;
    bar.m[1][1] = 120.0f;
    bar.m[2][2] = 2.0f;
    bar.m[0][3] = 512.0f;
    bar.m[1][3] = 60.0f;
    sk.bind_local = {assets::Mat3x4{}, bar};
    sk.finalize();
    m->skeleton = std::move(sk);
    m->meshes[0].name = "Empire_Faceplate_ALT0";
    m->meshes[0].attach_bone = 1;
    m->submeshes[0].attach_bone = 1;
    m->bounds = {{0, 0, -1}, {1024, 120, 1}};
    m->sphere = {{512, 60, 0}, 520};
    return m;
}

// Ring texture for the (unlit stand-in) selection blob: transparent outside, team-tinted ring.
std::shared_ptr<assets::TextureAsset> ring_texture() {
    auto t = std::make_shared<assets::TextureAsset>();
    t->name = "synthetic_ring";
    t->format = assets::TextureFormat::RGBA8;
    t->width = t->height = 64;
    t->mip_levels = 1;
    t->data.resize(64 * 64 * 4);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
            const float dx = static_cast<float>(x) - 31.5f, dy = static_cast<float>(y) - 31.5f;
            const float r = std::sqrt(dx * dx + dy * dy);
            const bool ring = r > 24.0f && r < 30.0f;
            uint8_t* p = &t->data[(static_cast<size_t>(y) * 64 + x) * 4];
            p[0] = ring ? 80 : 0;
            p[1] = ring ? 255 : 24;
            p[2] = ring ? 80 : 40;
            p[3] = 255;
        }
    assets::TextureLevel l;
    l.width = l.height = 64;
    l.size = t->data.size();
    t->levels.push_back(l);
    return t;
}

#ifdef ALAMO_UI_HAVE_VFS
// Real data (ALAMO_FOC_DATA): CommandBarComponents.xml, the MT_CommandBar atlas (MTD + TGA), MasterTextFile_ENGLISH,
// the real i_tactical_controls.alo shell (lit through the game's effects) and a space scene of debug boxes, at the
// three resolutions -> logs/ui_hud_real_<w>x<h>.png.
void test_gpu_real(Gpu& g, const std::string& src_dir, bool strict) {
    (void)strict;
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real hud: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::EffectCache effects(files.get());
    assets::AssetCache cache(*files, effects.model_load_options());
    jobs::JobSystem js;
    render::Renderer rr;
    render::RendererDesc rrd;
    rrd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rrd.depth_format = VK_FORMAT_D32_SFLOAT;
    rrd.effects = &effects;
    rrd.js = &js;
    CHECK(rr.init(g.ctx, g.alloc, g.up, rrd));
    rr.set_frame_ring(&g.ring);
    rr.set_lighting(render::Lighting::space_default());

    ui::ComponentTable table;
    CHECK(table.load(*files, &err));
    ui::TextureAtlas atlas;
    std::vector<uint8_t> mtd;
    CHECK(files->read("Data\\Art\\Textures\\MT_CommandBar.mtd", mtd) && atlas.parse(mtd, &err));
    const auto atlas_tex = cache.texture("MT_CommandBar.tga");
    CHECK(atlas_tex && !atlas_tex->is_fallback);
    atlas.texture_width = atlas_tex->width;
    atlas.texture_height = atlas_tex->height;
    CHECK(g.hud.set_atlas_texture(*atlas_tex));
    ui::TextDatabase text;
    std::string tlog;
    CHECK(ui::load_master_text(*files, "ENGLISH", text, &tlog));
    std::printf("real hud: %zu components, atlas %zu entries %ux%u, %zu strings\n", table.size(), atlas.size(),
                atlas.texture_width, atlas.texture_height, text.size());

    ui::ShellSet shells;
    const std::string names[] = {"i_main_skirmish"};
    std::vector<std::string> missing;
    shells.load(
        table, [&](std::string_view n) { return cache.model(n); }, names, &missing);
    CHECK(shells.shells().size() == 1 && missing.empty());
    if (shells.empty()) {
        rr.shutdown();
        return;
    }
    shells.set_alt(shells.shells()[0].component, 0);  // Empire faceplate
    shells.set_visible(shells.shells()[0].component, true);
    ui::ShellDrawer drawer;
    const render::TextureSource tex = [&](std::string_view n, assets::TextureRole role) { return cache.texture(n, role); };
    CHECK(drawer.upload(rr, shells, tex, &err));
    drawer.wait_uploads(g.up);
    render::GpuModel box;
    CHECK(rr.upload_model(box_model(), tex, box, nullptr, &err));

    auto raster = ui::make_system_rasterizer();
    ui::FontCache fonts(*raster);
    ui::TacticalHud hud;
    hud.init(table, atlas, fonts, &text);
    shells.apply(hud.layout());
    std::vector<ui::HudTypeInfo> types(2);
    types[0].icon_name = "I_BUTTON_TIE_FIGHTER.TGA";
    types[0].gui_bracket_size = 0;
    types[1].icon_name = "I_BUTTON_TIE_FIGHTER.TGA";
    types[1].gui_bracket_size = 2;
    std::vector<ui::HudUnit> units;
    std::vector<render::Instance> inst;
    for (uint32_t i = 0; i < 6; ++i) {
        ui::HudUnit u;
        u.id = 200 + i;
        u.type = i < 4 ? 0u : 1u;
        u.owner = i == 5 ? 1 : 0;
        u.position = {-250.0f + 100.0f * static_cast<float>(i), 60.0f * static_cast<float>(i % 3) - 60.0f, 0.0f};
        u.radius = i < 4 ? 30.0f : 70.0f;
        u.health = 0.4f + 0.1f * static_cast<float>(i);
        u.shield = i >= 4 ? 0.7f : -1.0f;
        u.selected = i != 5;
        u.control_group = static_cast<int8_t>(i < 2 ? 1 : -1);
        u.team_color = u.owner == 0 ? ui::Rgba8{80, 160, 255, 255} : ui::Rgba8{255, 60, 40, 255};
        units.push_back(u);
        render::Instance in;
        in.model = &box;
        in.transform.m[0][0] = in.transform.m[1][1] = in.transform.m[2][2] = u.radius;
        in.transform.m[0][3] = u.position.x;
        in.transform.m[1][3] = u.position.y;
        inst.push_back(in);
    }
    ui::RadarImage radar;
    {
        std::vector<uint8_t> vis(32 * 32);
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 32; ++x) vis[y * 32 + x] = (x + y) < 34 ? 255 : 0;
        ui::RadarSettings rs;
        CHECK(rs.load(*files, &err));
        radar.build(128, {}, 0, 0, vis, 32, 32, rs.space_fow);
    }
    CHECK(g.hud.set_dynamic_page(ui::kHudPageRadar, radar.width(), radar.height(), radar.rgba(), radar.generation()));
    ui::RadarEvents pings;
    ui::RadarSettings rs;
    rs.load(*files, &err);
    pings.add(rs, "Default_Click", 300, 100);
    std::vector<ui::HudWaypointLine> lines(1);
    lines[0].points = {{-250, -60, 0}, {100, 250, 0}, {400, 250, 0}};
    ui::TacticalCameraController cam;
    cam.set_look_at({0, 0, 0});
    cam.set_distance(900.0f);

    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}, std::pair{1280u, 1024u}}) {
        Offscreen o;
        CHECK(o.create(g, w, h));
        const ui::CameraParams cp = cam.params(static_cast<float>(w) / static_cast<float>(h));
        const render::ViewMatrices vm = ui::to_view_matrices(cp, w, h);
        hud.resize(w, h);
        ui::TacticalHudFrame hf;
        hf.world.view = ui::make_view(cp, w, h);
        hf.map_min_x = hf.map_min_y = -1500.0f;
        hf.map_max_x = hf.map_max_y = 1500.0f;
        hf.credits = 4250;
        hf.faction_alt = 1;  // Empire pads
        hf.shells_loaded = true;
        hf.radar_image = &radar;
        hf.radar_events = &pings;
        hf.radar_settings = &rs;
        hf.waypoints = lines;
        hf.time = 1.0;
        // [port: Gabriel] queued orders (attack, then two moves, then an attack) and the paused look.
        ui::HudOrderQueue oq;
        oq.unit = 1;
        oq.position = {-120, -200, 0};
        oq.steps = {{ui::HudQueuedStep::Kind::Attack, {150, -120, 0}, true},
                    {ui::HudQueuedStep::Kind::Move, {260, 80, 0}, true},
                    {ui::HudQueuedStep::Kind::Move, {40, 160, 0}, true},
                    {ui::HudQueuedStep::Kind::Ability, {-200, 120, 0}, true}};
        hf.order_queues = std::span<const ui::HudOrderQueue>(&oq, 1);
        hf.paused = true;
        hf.providers.abilities = [](const ui::SelectedGroup& grp, std::vector<ui::HudAbility>& out) {
            ui::HudAbility a;
            a.ability_type = 5;
            a.icon = "I_BUTTON_COMMAND_BAR_ATTACK.TGA";
            a.recharge = grp.type == 0 ? 0.35f : 1.0f;
            out.push_back(a);
        };
        hf.providers.build_queue = [](std::vector<ui::HudQueueEntry>& out) {
            ui::HudQueueEntry e;
            e.icon = "I_BUTTON_TIE_FIGHTER.TGA";
            e.progress = 0.6f;
            e.count = 3;
            out.push_back(e);
        };
        // Hover the retreat button long enough for its tooltip.
        const ui::ComponentPlacement& rp = hud.layout().placement(static_cast<uint32_t>(table.find("b_retreat")));
        const ui::Vec2f rpx = hud.canvas().canvas_to_pixel(rp.center.x, rp.center.y);
        hud.build(hf, units, types);
        ui::RoutedInput ri;
        ri.hover_component = hud.hit_test(static_cast<int32_t>(rpx.x), static_cast<int32_t>(rpx.y));
        std::vector<commands::EventData> cmds;
        hud.update_input(ri, false, false, 0.0, cmds);
        hud.update_input(ri, false, false, 1.0, cmds);
        hf.cursor_px = rpx;
        hud.build(hf, units, types);
        CHECK(ri.hover_component == table.find("b_retreat") && hud.stats().tooltip);
        CHECK(hud.stats().queue_markers == 4 && hud.stats().queue_dots > 20);
        CHECK(hud.stats().panel_slots == 2 && hud.stats().ability_buttons == 1 && hud.stats().queue_entries == 1 &&
              hud.stats().buttons > 5 && hud.stats().waypoint_dots > 10);
        const ui::HudCanvas& cv = hud.canvas();
        std::vector<uint8_t> rgba;
        render::DrawStats ds{}, ss{};
        ui::HudDrawStats hs;
        for (int k = 0; k < 64; ++k) {  // settle pipeline compiles / translations (deterministic PNG)
            rhi::Frame* f = g.ring.begin_frame();
            CHECK(f != nullptr);
            if (!f) break;
            VkCommandBuffer cmd = f->cmd();
            const rhi::DeviceFns& fn = g.ctx.vkd();
            const rhi::AcquireResult acq = g.up.record_acquires(cmd);
            rr.make_resident(box);
            drawer.make_resident(rr);
            render::PassTarget pt;
            pt.color = o.color.handle;
            pt.color_view = o.color.view;
            pt.depth = o.depth.handle;
            pt.depth_view = o.depth.view;
            pt.extent = {w, h};
            pt.clear[0] = 0.01f;
            pt.clear[1] = 0.01f;
            pt.clear[2] = 0.03f;
            pt.color_after = rhi::ImageState::TransferSrc;
            render::RecordOptions ro;
            ro.shadows = false;
            ro.space = true;
            ds = {};
            ss = {};
            CHECK(rr.render(*f, cmd, pt, vm, inst, render::DrawMode::Lit, ro, &ds));
            ui::HudTarget ht;
            ht.color = o.color.handle;
            ht.color_view = o.color.view;
            ht.depth = o.depth.handle;
            ht.depth_view = o.depth.view;
            ht.extent = {w, h};
            ht.before = ht.after = rhi::ImageState::TransferSrc;
            CHECK(g.hud.record(
                *f, cmd, ht, cv, hud.draw_list(), &fonts,
                [&](VkCommandBuffer c, const ui::HudFrameStep& step) {
                    if (step.kind == ui::HudStepKind::ShellModels) {
                        render::DrawStats s1;
                        drawer.draw(rr, *f, c, cv, shells, hud.layout(), step.first, &s1);
                        ss.draws += s1.draws;
                        ss.pending_pipeline += s1.pending_pipeline;
                        ss.translation_pending += s1.translation_pending;
                    }
                },
                &hs));
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
            const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                                  VK_ACCESS_2_HOST_READ_BIT);
            rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
            VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            rhi::FrameSubmit fs;
            if (acq.wait_value) fs.waits = {&wait, 1};
            CHECK(g.ring.end_frame(*f, fs));
            g.ring.wait_idle();
            const bool settled = ds.post_pending == 0 && ds.translation_pending == 0 && ds.pending_pipeline == 0 &&
                                 ss.pending_pipeline == 0 && ss.translation_pending == 0 && ss.draws > 0 && box.resident();
            if (settled || k == 63) break;
            rr.wait_translations();
            rr.pipelines().wait_idle();
        }
        g.alloc.invalidate(o.readback.alloc);
        const auto* px = static_cast<const uint8_t*>(o.readback.mapped());
        rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
        std::printf("real hud %ux%u: world %u draws, shell %u draws; hud %u quads, %u batches; %u ability, %u buttons\n", w, h,
                    ds.draws, ss.draws, hs.quads, hs.batches, hud.stats().ability_buttons, hud.stats().buttons);
        CHECK(ss.draws >= 1 && ss.pending_pipeline == 0);
        // The faceplate covers the bottom-left corner region with non-background pixels.
        uint64_t covered = 0, total = 0;
        for (uint32_t y = h - h / 8; y < h; ++y)
            for (uint32_t x = 0; x < std::min(w, h * 4 / 3) / 2; ++x, ++total) {
                const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                if (p[0] + p[1] + p[2] > 60) ++covered;
            }
        std::printf("real hud %ux%u: bottom-left coverage %.1f%%\n", w, h, 100.0 * static_cast<double>(covered) / static_cast<double>(total));
        CHECK(covered * 2 > total);
        char name[64];
        std::snprintf(name, sizeof name, "/logs/ui_hud_real_%ux%u.png", w, h);
        CHECK(render::write_png_rgba(src_dir + name, w, h, rgba));
        std::printf("wrote %s%s\n", src_dir.c_str(), name);
        o.destroy(g);
    }
    g.ring.wait_idle();
    drawer.destroy(rr);
    rr.destroy_model(box);
    rr.collect_released();
    rr.shutdown();
}

// Playtest regression (galactic faceplate missing, trade routes through the radar box): the real
// i_galactic_controls.alo (i_main_commandbar) drawn alone - no HUD quads, so no radar backdrop - on a magenta clear for
// each faction alternate. Per alt: exactly one *_Faceplate_ALT<alt> mesh passes the instance's mesh mask and _ALT test,
// with a MeshAlpha effect and a real (non-fallback) texture, and the faceplate's alpha-255 texels cover the whole
// radar_galactic rectangle (no magenta pixel inside its inner 80 %).
void test_gpu_galactic_faceplate(Gpu& g, const std::string& src_dir) {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("galactic faceplate: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::EffectCache effects(files.get());
    assets::AssetCache cache(*files, effects.model_load_options());
    jobs::JobSystem js;
    render::Renderer rr;
    render::RendererDesc rrd;
    rrd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rrd.depth_format = VK_FORMAT_D32_SFLOAT;
    rrd.effects = &effects;
    rrd.js = &js;
    CHECK(rr.init(g.ctx, g.alloc, g.up, rrd));
    rr.set_frame_ring(&g.ring);
    rr.set_lighting(render::Lighting::space_default());

    ui::ComponentTable table;
    CHECK(table.load(*files, &err));
    ui::TextureAtlas atlas;
    std::vector<uint8_t> mtd;
    CHECK(files->read("Data\\Art\\Textures\\MT_CommandBar.mtd", mtd) && atlas.parse(mtd, &err));
    ui::ShellSet shells;
    const std::string names[] = {"i_main_commandbar"};
    std::vector<std::string> missing;
    shells.load(
        table, [&](std::string_view n) { return cache.model(n); }, names, &missing);
    CHECK(shells.shells().size() == 1 && missing.empty());
    if (shells.empty()) {
        rr.shutdown();
        return;
    }
    const uint32_t comp = shells.shells()[0].component;
    shells.set_visible(comp, true);
    ui::ShellDrawer drawer;
    const render::TextureSource tex = [&](std::string_view n, assets::TextureRole role) { return cache.texture(n, role); };
    CHECK(drawer.upload(rr, shells, tex, &err));
    drawer.wait_uploads(g.up);
    auto raster = ui::make_system_rasterizer();
    ui::FontCache fonts(*raster);
    ui::GalacticHud hud;
    hud.init(table, atlas, fonts, nullptr);
    const uint32_t w = 1280, h = 720;
    hud.resize(w, h);
    shells.apply(hud.layout());
    hud.layout().update(hud.canvas(), table);
    const int32_t radar = table.find("radar_galactic");
    CHECK(radar >= 0);
    if (radar < 0) {
        drawer.destroy(rr);
        rr.collect_released();
        rr.shutdown();
        return;
    }
    const ui::ComponentPlacement& rp = hud.layout().placement(static_cast<uint32_t>(radar));
    CHECK(rp.placed && rp.size.x > 0 && rp.size.y > 0);
    const ui::HudCanvas& cv = hud.canvas();
    const ui::Vec2f p0 = cv.canvas_to_pixel(rp.rect_center.x - 0.4f * rp.size.x, rp.rect_center.y - 0.4f * rp.size.y);
    const ui::Vec2f p1 = cv.canvas_to_pixel(rp.rect_center.x + 0.4f * rp.size.x, rp.rect_center.y + 0.4f * rp.size.y);
    const auto clampi = [](float v, uint32_t hi) {
        return static_cast<uint32_t>(std::fmin(std::fmax(v, 0.0f), static_cast<float>(hi - 1)));
    };
    const uint32_t x0 = clampi(std::fmin(p0.x, p1.x), w), x1 = clampi(std::fmax(p0.x, p1.x), w);
    const uint32_t y0 = clampi(std::fmin(p0.y, p1.y), h), y1 = clampi(std::fmax(p0.y, p1.y), h);
    CHECK(x1 > x0 + 10 && y1 > y0 + 10);
    const ui::HudDrawList empty_list;
    Offscreen o;
    CHECK(o.create(g, w, h));
    for (int32_t alt = 0; alt < 3; ++alt) {
        shells.set_alt(comp, alt);
        std::vector<render::Instance> si;
        drawer.instances(shells, hud.layout(), si);
        CHECK(si.size() == 1);
        if (si.size() != 1) continue;
        const render::Instance& in = si[0];
        const assets::ModelAsset& ma = *in.model->asset;
        uint32_t faceplates = 0;
        for (const render::GpuSubmesh& gs : in.model->submeshes) {
            if (gs.mesh >= ma.meshes.size() || ma.meshes[gs.mesh].name.find("Faceplate") == std::string::npos) continue;
            const bool shown = !(gs.mesh < in.mesh_hidden.size() && in.mesh_hidden[gs.mesh]) && (gs.alt < 0 || gs.alt == in.alt);
            if (!shown) continue;
            ++faceplates;
            CHECK(gs.alt == alt);
            CHECK(gs.effect_name.find("MESHALPHA") != std::string::npos || gs.effect_name.find("MeshAlpha") != std::string::npos);
            std::printf("galactic faceplate alt %d: %s, %s\n", alt, ma.meshes[gs.mesh].name.c_str(), gs.effect_name.c_str());
        }
        CHECK(faceplates == 1);
        // i_galactic_dashboard_wide{,_rebel,_under}: loaded, not the fallback texture.
        uint32_t dashboards = 0;
        for (const render::GpuTexture& t : in.model->textures) {
            std::string lower = t.name;
            for (char& ch : lower) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            if (lower.find("dashboard_wide") == std::string::npos) continue;
            ++dashboards;
            CHECK(!t.fallback);
        }
        CHECK(dashboards >= 3);
        std::vector<uint8_t> rgba;
        render::DrawStats ss{};
        for (int k = 0; k < 64; ++k) {
            rhi::Frame* f = g.ring.begin_frame();
            CHECK(f != nullptr);
            if (!f) break;
            VkCommandBuffer cmd = f->cmd();
            const rhi::DeviceFns& fn = g.ctx.vkd();
            const rhi::AcquireResult acq = g.up.record_acquires(cmd);
            drawer.make_resident(rr);
            render::PassTarget pt;
            pt.color = o.color.handle;
            pt.color_view = o.color.view;
            pt.depth = o.depth.handle;
            pt.depth_view = o.depth.view;
            pt.extent = {w, h};
            pt.clear[0] = 1.0f;  // magenta: anything the faceplate leaves uncovered stays magenta
            pt.clear[1] = 0.0f;
            pt.clear[2] = 1.0f;
            pt.color_after = rhi::ImageState::TransferSrc;
            render::RecordOptions ro;
            ro.shadows = false;
            ro.space = true;
            ro.post = false;
            CHECK(rr.render(*f, cmd, pt, ui::hud_ortho_view(cv, ui::kHudCameraComponents), {}, render::DrawMode::Lit, ro));
            ui::HudTarget ht;
            ht.color = o.color.handle;
            ht.color_view = o.color.view;
            ht.depth = o.depth.handle;
            ht.depth_view = o.depth.view;
            ht.extent = {w, h};
            ht.before = ht.after = rhi::ImageState::TransferSrc;
            ss = {};
            CHECK(g.hud.record(*f, cmd, ht, cv, empty_list, &fonts, [&](VkCommandBuffer c, const ui::HudFrameStep& step) {
                if (step.kind == ui::HudStepKind::ShellModels) {
                    render::DrawStats s1;
                    drawer.draw(rr, *f, c, cv, shells, hud.layout(), step.first, &s1);
                    ss.draws += s1.draws;
                    ss.pending_pipeline += s1.pending_pipeline;
                    ss.translation_pending += s1.translation_pending;
                }
            }));
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
            const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                                  VK_ACCESS_2_HOST_READ_BIT);
            rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
            VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            rhi::FrameSubmit fs;
            if (acq.wait_value) fs.waits = {&wait, 1};
            CHECK(g.ring.end_frame(*f, fs));
            g.ring.wait_idle();
            const bool settled = ss.pending_pipeline == 0 && ss.translation_pending == 0 && ss.draws > 0 && drawer.resident();
            if (settled || k == 63) break;
            rr.wait_translations();
            rr.pipelines().wait_idle();
        }
        g.alloc.invalidate(o.readback.alloc);
        const auto* px = static_cast<const uint8_t*>(o.readback.mapped());
        rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
        uint64_t magenta = 0, total = 0;
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x, ++total) {
                const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                if (p[0] > 200 && p[1] < 60 && p[2] > 200) ++magenta;
            }
        std::printf("galactic faceplate alt %d: %u shell draws, radar box (%u..%u, %u..%u) %llu / %llu magenta pixels\n", alt,
                    ss.draws, x0, x1, y0, y1, static_cast<unsigned long long>(magenta), static_cast<unsigned long long>(total));
        CHECK(ss.draws > 0 && ss.pending_pipeline == 0);
        CHECK(total > 0 && magenta == 0);
        char name[80];
        std::snprintf(name, sizeof name, "/logs/ui_galactic_faceplate_alt%d.png", alt);
        CHECK(render::write_png_rgba(src_dir + name, w, h, rgba));
    }
    o.destroy(g);
    g.ring.wait_idle();
    drawer.destroy(rr);
    rr.collect_released();
    rr.shutdown();
}
#else
void test_gpu_real(Gpu&, const std::string&, bool) { std::printf("real hud: alamo_vfs not configured, skipped\n"); }
void test_gpu_galactic_faceplate(Gpu&, const std::string&) { std::printf("galactic faceplate: alamo_vfs not configured, skipped\n"); }
#endif

void test_gpu(const std::string& src_dir) {
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP gpu: no Vulkan loader\n");
            return;
        }
    }
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_ui_draw_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        CHECK(!strict);
        std::printf("SKIP gpu: no suitable Vulkan 1.3 device\n");
        return;
    }
    std::printf("gpu: %s, validation %s%s\n", g.ctx.caps().name.c_str(), g.ctx.validation_enabled() ? "on" : "off",
                strict ? " (required)" : "");
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 16ull << 20;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.depth_format = VK_FORMAT_D32_SFLOAT;
    const bool rinit = g.renderer.init(g.ctx, g.alloc, g.up, rd);
    CHECK(rinit);
    g.renderer.set_frame_ring(&g.ring);
    ui::HudRendererDesc hd;
    hd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    hd.depth_format = VK_FORMAT_D32_SFLOAT;
    const bool hinit = rinit && g.hud.init(g.ctx, g.alloc, g.up, hd);
    CHECK(hinit);
    const std::vector<uint8_t> atlas_px = hud_test::make_atlas_rgba();
    CHECK(hinit && g.hud.set_atlas_rgba(256, 256, atlas_px));

    // Models: debug box (units + the ortho "shell" stand-in) and the selection blob (space).
    render::GpuModel box, blob;
    std::string err;
    const auto ring = ring_texture();
    const render::TextureSource tex = [&](std::string_view name, assets::TextureRole) -> std::shared_ptr<const assets::TextureAsset> {
        return name.find("cutout") != std::string_view::npos ? ring : nullptr;
    };
    terrain::ShadowBlobMaterial mat;
    mat.name = "Selection_Empire_Space";
    mat.texture = "i_selection_cutout_space.tga";
    mat.texture2 = "i_selection_empire.tga";
    mat.shader = "BlobStencilMasked.fx";
    const bool up_box = hinit && g.renderer.upload_model(box_model(), tex, box, nullptr, &err);
    const bool up_blob = hinit && g.renderer.upload_model(ui::selection_blob_model(mat), tex, blob, nullptr, &err);
    CHECK(up_box && up_blob);

    // HUD data.
    const ui::ComponentTable table = hud_test::load_table();
    const ui::TextureAtlas atlas = hud_test::make_atlas();
    auto raster = ui::make_system_rasterizer();
    ui::FontCache fonts(*raster);
    ui::TacticalHud hud;
    hud.init(table, atlas, fonts);
    // Stage 2: the shell through ShellSet + ShellDrawer (synthetic .alo), the radar page (terrain + fog).
    ui::ShellSet shells;
    const auto shell_asset = shell_model();
    CHECK(shells.load(table, [&](std::string_view) -> std::shared_ptr<const assets::ModelAsset> { return shell_asset; }) == 1);
    shells.apply(hud.layout());
    ui::ShellDrawer shell_drawer;
    CHECK(hinit && shell_drawer.upload(g.renderer, shells, tex, &err) && shell_drawer.model_count() == 1);
    shell_drawer.wait_uploads(g.up);
    ui::RadarImage radar_img;
    {
        std::vector<uint8_t> cls(32 * 32), vis(16 * 16);
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 32; ++x)
                cls[y * 32 + x] = static_cast<uint8_t>((x / 8 + y / 8) % 2 ? ui::Passability::Clear : ui::Passability::Water);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x) vis[y * 16 + x] = x < 8 ? 0 : 255;
        ui::RadarSettings rs;
        const auto pal = rs.palette("Temperate");
        radar_img.build(64, ui::passability_rgba(cls, pal), 32, 32, vis, 16, 16, rs.land_fow);
    }
    CHECK(hinit && g.hud.set_dynamic_page(ui::kHudPageRadar, radar_img.width(), radar_img.height(), radar_img.rgba(),
                                          radar_img.generation()));
    std::vector<ui::HudTypeInfo> types(3);
    types[0].gui_bracket_size = 0;
    types[0].select_box_scale = 30.0f;
    types[1].gui_bracket_size = 2;
    types[1].select_box_scale = 70.0f;
    types[2].gui_bracket_size = 1;
    struct Spawn {
        float x, y, size;
        uint32_t type;
        int32_t owner;
        float health, shield;
        int8_t group;
        bool selected, hovered;
    };
    const Spawn spawns[] = {
        {-260, 120, 40, 0, 0, 1.0f, -1.0f, 1, true, false},  {-180, 160, 40, 0, 0, 0.6f, -1.0f, 1, true, false},
        {60, 40, 90, 1, 0, 0.8f, 0.5f, 2, true, false},     {320, 260, 60, 2, 1, 0.3f, 1.0f, -1, false, true},
        {-40, -260, 40, 0, 0, 0.9f, -1.0f, -1, false, false},
    };
    std::vector<ui::HudUnit> units;
    std::vector<render::Instance> inst;
    uint32_t id = 100;
    for (const Spawn& s : spawns) {
        ui::HudUnit u;
        u.id = id++;
        u.type = s.type;
        u.owner = s.owner;
        u.position = {s.x, s.y, 0.0f};
        u.radius = s.size * 0.75f;
        u.health = s.health;
        u.shield = s.shield;
        u.control_group = s.group;
        u.selected = s.selected;
        u.hovered = s.hovered;
        u.team_color = s.owner == 0 ? ui::Rgba8{80, 160, 255, 255} : ui::Rgba8{255, 60, 40, 255};
        units.push_back(u);
        render::Instance in;
        in.model = &box;
        in.transform.m[0][0] = in.transform.m[1][1] = in.transform.m[2][2] = s.size;
        in.transform.m[0][3] = s.x;
        in.transform.m[1][3] = s.y;
        in.transform.m[2][3] = 0.5f * s.size;
        in.team_color_override = true;
        in.team_color = {u.team_color.r / 255.0f, u.team_color.g / 255.0f, u.team_color.b / 255.0f, 1.0f};
        inst.push_back(in);
    }
    ui::FlyTextList fly;
    ui::FlyText ft;
    ft.anchor = {320, 260, 60};
    ft.text = U"+250 Credits";
    ft.offset_px = {0, -50};
    fly.add(ft);

    // Camera: Space_Mode pose looking at the origin.
    ui::TacticalCameraController cam;
    cam.set_look_at({0, 0, 0});
    cam.set_distance(900.0f);

    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}, std::pair{1280u, 1024u}}) {
        if (!(up_box && up_blob)) break;
        Offscreen o;
        CHECK(o.create(g, w, h));
        const float aspect = static_cast<float>(w) / static_cast<float>(h);
        const ui::CameraParams cp = cam.params(aspect);
        const render::ViewMatrices vm = ui::to_view_matrices(cp, w, h);
        hud.resize(w, h);
        ui::TacticalHudFrame hf;
        hf.world.view = ui::make_view(cp, w, h);
        hf.map_min_x = hf.map_min_y = -600.0f;
        hf.map_max_x = hf.map_max_y = 600.0f;
        hf.credits = 4250;
        hf.shells_loaded = true;
        hf.radar_image = &radar_img;
        hud.build(hf, units, types, &fly);
        CHECK(hud.stats().world.brackets == 4 && hud.stats().world.group_numbers == 3 && hud.stats().world.flytexts == 1);
        CHECK(hud.selection_blobs().size() == 3 && hud.stats().panel_slots == 2 && hud.stats().radar_blips == 5);
        std::vector<render::Instance> frame_inst = inst;
        for (const ui::SelectionBlob& sb : hud.selection_blobs()) frame_inst.push_back(ui::selection_blob_instance(blob, sb));
        const ui::HudCanvas& cv = hud.canvas();
        shell_drawer.make_resident(g.renderer);
        std::vector<render::Instance> shell_inst;
        shell_drawer.instances(shells, hud.layout(), shell_inst);
        CHECK(shell_inst.size() == 1 && shell_inst.size() == 1 && std::fabs(shell_inst[0].transform.m[0][3] + 0.5f * cv.width) < 1e-3f &&
              std::fabs(shell_inst[0].transform.m[1][3] + 0.5f * cv.height) < 1e-3f);

        rhi::Frame* f = g.ring.begin_frame();
        CHECK(f != nullptr);
        if (!f) break;
        VkCommandBuffer cmd = f->cmd();
        const rhi::DeviceFns& fn = g.ctx.vkd();
        const rhi::AcquireResult acq = g.up.record_acquires(cmd);
        g.renderer.make_resident(box);
        g.renderer.make_resident(blob);
        render::PassTarget pt;
        pt.color = o.color.handle;
        pt.color_view = o.color.view;
        pt.depth = o.depth.handle;
        pt.depth_view = o.depth.view;
        pt.extent = {w, h};
        pt.clear[0] = 0.02f;
        pt.clear[1] = 0.03f;
        pt.clear[2] = 0.08f;
        pt.color_after = rhi::ImageState::TransferSrc;
        render::RecordOptions ro;
        ro.shadows = false;
        ro.space = true;
        render::DrawStats ds;
        CHECK(g.renderer.render(*f, cmd, pt, vm, frame_inst, render::DrawMode::Unlit, ro, &ds));
        ui::HudTarget ht;
        ht.color = o.color.handle;
        ht.color_view = o.color.view;
        ht.depth = o.depth.handle;
        ht.depth_view = o.depth.view;
        ht.extent = {w, h};
        ht.before = ht.after = rhi::ImageState::TransferSrc;
        uint32_t shell_draws = 0;
        ui::HudDrawStats hs;
        const bool hud_ok = g.hud.record(
            *f, cmd, ht, cv, hud.draw_list(), &fonts,
            [&](VkCommandBuffer c, const ui::HudFrameStep& step) {
                if (step.kind == ui::HudStepKind::ShellModels) {
                    render::DrawStats ss;
                    if (shell_drawer.draw(g.renderer, *f, c, cv, shells, hud.layout(), step.first, &ss)) shell_draws += ss.draws;
                }
            },
            &hs);
        CHECK(hud_ok);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
        const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                              VK_ACCESS_2_HOST_READ_BIT);
        rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
        VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        rhi::FrameSubmit fs;
        if (acq.wait_value) fs.waits = {&wait, 1};
        CHECK(g.ring.end_frame(*f, fs));
        g.ring.wait_idle();
        g.alloc.invalidate(o.readback.alloc);
        std::printf("gpu %ux%u: world %u draws; hud %u quads, %u batches, %u layers, %u callbacks, %u glyph page uploads, "
                    "shell draws %u\n",
                    w, h, ds.draws, hs.quads, hs.batches, hs.layers, hs.callbacks, hs.page_uploads, shell_draws);
        CHECK(hs.layers == 13 && hs.callbacks == 7 && hs.batches > 0 && shell_draws >= 1);  // 7 non-layer steps
        if (w == 1024) CHECK(hs.page_uploads >= 2);  // glyph page(s) + the radar page on the first frame
        CHECK(ds.draws >= 8);  // 5 boxes + 3 selection blobs
        const auto* px = static_cast<const uint8_t*>(o.readback.mapped());
        auto pixel = [&](float x, float y) {
            const auto ix = static_cast<uint32_t>(std::clamp(x, 0.0f, static_cast<float>(w - 1)));
            const auto iy = static_cast<uint32_t>(std::clamp(y, 0.0f, static_cast<float>(h - 1)));
            return px + (static_cast<size_t>(iy) * w + ix) * 4;
        };
        // The first bracket piece (layer 0, list 0: the selected small unit's left piece) is bright green on screen.
        const auto q = hud.draw_list().items(0, ui::HudList::Alpha);
        CHECK(!q.empty());
        if (!q.empty()) {
            // Left vertical stroke of the '[' piece: 1 canvas unit in from its left edge, mid height.
            const ui::Vec2f p = cv.canvas_to_pixel(q[0].min.x + 1.0f, 0.5f * (q[0].min.y + q[0].max.y));
            const uint8_t* c = pixel(p.x, p.y);
            std::printf("bracket pixel (%.1f, %.1f) = %u %u %u\n", p.x, p.y, c[0], c[1], c[2]);
            CHECK(c[1] > 150 && c[0] < 120 && c[2] < 120);
        }
        // The synthetic shell bar (unlit, white top face) covers the bottom 120 canvas units from the left edge.
        {
            const ui::Vec2f sp = cv.canvas_to_pixel(-0.5f * cv.width + 600.0f, -0.5f * cv.height + 40.0f);
            const uint8_t* c = pixel(sp.x, sp.y);
            std::printf("shell pixel (%.1f, %.1f) = %u %u %u\n", sp.x, sp.y, c[0], c[1], c[2]);
            CHECK(c[0] > 200 && c[1] > 200 && c[2] > 200);
        }
        uint32_t green = 0;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            if (px[i * 4 + 1] > 200 && px[i * 4 + 0] < 100) ++green;
        CHECK(green > 50);
        const std::vector<uint8_t> rgba(px, px + static_cast<size_t>(w) * h * 4);
        char name[64];
        std::snprintf(name, sizeof name, "/logs/ui_hud_%ux%u.png", w, h);
        const std::string png = src_dir + name;
        CHECK(render::write_png_rgba(png, w, h, rgba));
        std::printf("wrote %s\n", png.c_str());
        o.destroy(g);
    }
    g.ring.wait_idle();
    // _ALT selection: the faction-1 mask hides the ALT0 faceplate -> nothing to draw.
    shells.set_alt(shells.shells()[0].component, 1);
    {
        std::vector<render::Instance> si;
        shell_drawer.instances(shells, hud.layout(), si);
        CHECK(si.size() == 1 && si[0].mesh_hidden.size() == 1 && si[0].mesh_hidden[0] == 1);
        // The instance carries the faction alternate, else the renderer's _ALT test drops every _ALT1 / _ALT2 mesh.
        CHECK(si.size() == 1 && si[0].alt == 1);
    }
    shell_drawer.destroy(g.renderer);
    test_gpu_real(g, src_dir, strict);
    test_gpu_galactic_faceplate(g, src_dir);
    if (up_box) g.renderer.destroy_model(box);
    if (up_blob) g.renderer.destroy_model(blob);
    g.renderer.collect_released();
    g.hud.shutdown();
    g.renderer.shutdown();
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    const uint32_t ve = g.ctx.validation_errors(), vw = g.ctx.validation_warnings();
    std::printf("gpu: validation %u errors, %u warnings\n", ve, vw);
    CHECK(ve == 0 && vw == 0);
    g.ctx.shutdown();
}

}  // namespace

int main() {
    test_cpu();
    test_gpu(ALAMO_UI_SOURCE_DIR);
    if (g_fail) {
        std::printf("alamo_ui_draw_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_draw_tests: all passed\n");
    return 0;
}

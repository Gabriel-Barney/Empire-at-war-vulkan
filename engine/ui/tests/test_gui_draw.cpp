// alamo_ui_gui_draw_tests (P4, GPU; skips cleanly without a Vulkan 1.3 device; 0 validation messages required when the
// project's validation layers are installed): GUI dialogs drawn as 2D scene #1 through HudRenderer::record's dialog list
// (frame step Dialogs), image pages for stand-alone textures, and with ALAMO_FOC_DATA the real screens:
//   logs/ui_gui_synthetic_1024x768.png   synthetic skin / atlas (always)
//   logs/ui_gui_main_menu.png            IDD_MAIN_MENU_DIALOG over MenuBack_overlay (1920 x 1080)
//   logs/ui_gui_campaign_select.png      IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG over the single player column (1920 x 1080)
//   logs/ui_gui_load_screen.png          IDD_LAND_BATTLE_LOAD_DIALOG at 60 % (1920 x 1080)
//   logs/ui_gui_galactic_hud.png         galactic command bar + organiser shells, planet labels, build pads, queue,
//                                        fleet pads, objectives (1920 x 1080)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/assets/texture.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/context.hpp"
#include "alamo/rhi/frame.hpp"
#include "alamo/rhi/loader.hpp"
#include "alamo/rhi/memory.hpp"
#include "alamo/rhi/upload.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/galactic_hud.hpp"
#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/gui_info_screens.hpp"
#include "alamo/ui/gui_lobby.hpp"
#include "alamo/ui/mouse_cursor.hpp"
#include "alamo/ui/gui_menus.hpp"
#include "alamo/ui/gui_resource.hpp"
#include "alamo/ui/gui_screens.hpp"
#include "alamo/ui/hud_panels.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_renderer.hpp"
#include "alamo/ui/hud_shell.hpp"
#include "alamo/ui/hud_shell_draw.hpp"
#include "alamo/ui/load_screen.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/render_bridge.hpp"
#ifdef ALAMO_UI_HAVE_VFS
#include <cstdlib>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/vfs/vfs.hpp"
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

struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
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

// One frame: the renderer clears / renders an empty world (post chain included), then the HUD pass draws `hud` and the
// dialog list at the Dialogs step. `settle` repeats the frame until shell pipelines are ready. Returns RGBA pixels.
std::vector<uint8_t> render_frame(Gpu& g, render::Renderer& rr, uint32_t w, uint32_t h, const ui::HudCanvas& cv,
                                  const ui::HudDrawList& hud, const ui::HudDrawList* dialogs, ui::FontCache& fonts,
                                  const ui::HudRenderer::StepCallback& steps, const std::function<bool()>& settled,
                                  ui::HudDrawStats* hs_out = nullptr) {
    std::vector<uint8_t> rgba;
    Offscreen o;
    CHECK(o.create(g, w, h));
    for (int k = 0; k < 64; ++k) {
        rhi::Frame* f = g.ring.begin_frame();
        CHECK(f != nullptr);
        if (!f) break;
        VkCommandBuffer cmd = f->cmd();
        const rhi::DeviceFns& fn = g.ctx.vkd();
        const rhi::AcquireResult acq = g.up.record_acquires(cmd);
        render::PassTarget pt;
        pt.color = o.color.handle;
        pt.color_view = o.color.view;
        pt.depth = o.depth.handle;
        pt.depth_view = o.depth.view;
        pt.extent = {w, h};
        pt.clear[0] = 0.0f;
        pt.clear[1] = 0.0f;
        pt.clear[2] = 0.02f;
        pt.color_after = rhi::ImageState::TransferSrc;
        render::RecordOptions ro;
        ro.shadows = false;
        ro.space = true;
        render::ViewMatrices vm = ui::hud_ortho_view(cv, ui::kHudCameraComponents);
        CHECK(rr.render(*f, cmd, pt, vm, {}, render::DrawMode::Unlit, ro, nullptr));
        ui::HudTarget ht;
        ht.color = o.color.handle;
        ht.color_view = o.color.view;
        ht.depth = o.depth.handle;
        ht.depth_view = o.depth.view;
        ht.extent = {w, h};
        ht.before = ht.after = rhi::ImageState::TransferSrc;
        ui::HudDrawStats hs;
        CHECK(g.hud.record(*f, cmd, ht, cv, hud, &fonts, [&](VkCommandBuffer c, const ui::HudFrameStep& s) {
            if (steps) steps(c, s);
        }, &hs, dialogs));
        if (hs_out) *hs_out = hs;
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
        if (!settled || settled() || k == 63) break;
        rr.wait_translations();
        rr.pipelines().wait_idle();
    }
    g.alloc.invalidate(o.readback.alloc);
    const auto* px = static_cast<const uint8_t*>(o.readback.mapped());
    rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    o.destroy(g);
    return rgba;
}

const uint8_t* pixel(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, ui::Vec2f p) {
    const auto x = static_cast<uint32_t>(std::clamp(p.x, 0.0f, static_cast<float>(w - 1)));
    const auto y = static_cast<uint32_t>(std::clamp(p.y, 0.0f, static_cast<float>(h - 1)));
    return &rgba[(static_cast<size_t>(y) * w + x) * 4];
}

double coverage(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, ui::Vec2f a, ui::Vec2f b) {
    uint64_t n = 0, lit = 0;
    const auto y1 = static_cast<uint32_t>(std::clamp(b.y, 0.0f, static_cast<float>(h)));
    const auto x1 = static_cast<uint32_t>(std::clamp(b.x, 0.0f, static_cast<float>(w)));
    for (auto y = static_cast<uint32_t>(std::max(0.0f, a.y)); y < y1; ++y)
        for (auto x = static_cast<uint32_t>(std::max(0.0f, a.x)); x < x1; ++x, ++n) {
            const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            if (p[0] + p[1] + p[2] > 45) ++lit;
        }
    return n ? static_cast<double>(lit) / static_cast<double>(n) : 0.0;
}

const char* kRc = R"RC(
IDD_SYNTH DIALOGEX 0, 0, 600, 400
STYLE DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP
CAPTION "Dialog"
FONT 8, "Arial", 400, 0, 0x0
BEGIN
    DEFPUSHBUTTON   "Begin",IDC_SYN_OK,20,350,160,24
    PUSHBUTTON      "Cancel",IDC_SYN_CANCEL,400,350,160,24
    LTEXT           "A synthetic dialog drawn through the HUD pass",IDC_STATIC,20,20,560,18
    LISTBOX         IDC_SYN_LIST,20,60,260,200,WS_VSCROLL | WS_TABSTOP
    CONTROL         "",IDC_SYN_SLIDER,"msctls_trackbar32",TBS_BOTH | WS_TABSTOP,320,60,240,15
    CONTROL         "",IDC_SYN_PROGRESS,"msctls_progress32",WS_BORDER,320,100,240,16
    CONTROL         "Check",IDC_SYN_CHECK,"Button",BS_AUTOCHECKBOX | WS_TABSTOP,320,140,200,14
    EDITTEXT        IDC_SYN_EDIT,320,180,240,18,ES_AUTOHSCROLL
    CONTROL         "",IDC_SYN_IMAGE,"PETROGLYPH_DIALOG_IMAGE",WS_TABSTOP,320,220,100,100
END
)RC";

void test_synthetic(Gpu& g, render::Renderer& rr, const std::string& src_dir) {
    ui::DialogScript script;
    CHECK(script.parse(kRc));
    ui::GuiSkin skin;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view("<GUIDialogs><Textures><Default><Frame_Background>not_in_atlas.tga</Frame_Background></Default></Textures></GUIDialogs>"), "x");
    skin.parse(doc.root());
    ui::TextureAtlas atlas;  // empty: every skin piece uses the [port] flat stand-ins
    atlas.texture_width = atlas.texture_height = 1;
    auto raster = ui::make_system_rasterizer();
    ui::FontCache fonts(*raster);
    ui::GuiManager gui;
    gui.init(script, skin, atlas, fonts);
    gui.resize(1024, 768);
    // Image page 16: a 2 x 2 checker for the custom image control.
    const uint8_t img[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    CHECK(g.hud.set_image_rgba(ui::kHudPageImageBase, 2, 2, img));
    ui::GuiImagePages pages;
    gui.set_image_resolver([&](std::string_view t) { return pages.resolve(t); });
    CHECK(pages.page("checker.tga") == ui::kHudPageImageBase);
    pages.set_loaded(ui::kHudPageImageBase, 2, 2);
    ui::GuiDialog* d = gui.open("IDD_SYNTH");
    CHECK(d != nullptr);
    if (!d) return;
    d->set_items("IDC_SYN_LIST", {U"Kuat", U"Corellia", U"Coruscant", U"Mon Calamari"}, 1);
    d->set_value("IDC_SYN_SLIDER", 70);
    d->set_value("IDC_SYN_PROGRESS", 60);
    d->set_checked("IDC_SYN_CHECK", true);
    d->set_text("IDC_SYN_EDIT", U"Save 1");
    d->set_image("IDC_SYN_IMAGE", "checker.tga");
    ui::HudDrawList dialogs, empty;
    gui.build(dialogs, 0.0);
    ui::HudDrawStats hs;
    const std::vector<uint8_t> px = render_frame(g, rr, 1024, 768, gui.canvas(), empty, &dialogs, fonts, {}, {}, &hs);
    std::printf("gui synthetic: %u quads, %u batches, %u glyph uploads\n", hs.quads, hs.batches, hs.page_uploads);
    // Dialog background stand-in inside the frame, black outside it.
    const ui::GuiRect r = d->rect();
    const ui::Vec2f in = gui.canvas().canvas_to_pixel(r.min.x + 5.0f, r.max.y - 300.0f);
    const ui::Vec2f out = gui.canvas().canvas_to_pixel(r.min.x - 20.0f, r.max.y - 300.0f);
    const uint8_t* pi = pixel(px, 1024, 768, in);
    const uint8_t* po = pixel(px, 1024, 768, out);
    std::printf("gui synthetic: inside %u %u %u, outside %u %u %u\n", pi[0], pi[1], pi[2], po[0], po[1], po[2]);
    CHECK(pi[0] >= 12 && pi[2] >= 20 && po[0] + po[1] + po[2] < 12);
    // Image page: top-left texel red, bottom-right yellow.
    const ui::GuiRect ir = d->control_rect(static_cast<size_t>(d->find("IDC_SYN_IMAGE")));
    const uint8_t* tl = pixel(px, 1024, 768, gui.canvas().canvas_to_pixel(ir.min.x + 10.0f, ir.max.y - 10.0f));
    const uint8_t* br = pixel(px, 1024, 768, gui.canvas().canvas_to_pixel(ir.max.x - 10.0f, ir.min.y + 10.0f));
    std::printf("gui synthetic: image tl %u %u %u, br %u %u %u\n", tl[0], tl[1], tl[2], br[0], br[1], br[2]);
    CHECK(tl[0] > 200 && tl[1] < 60 && br[0] > 200 && br[1] > 200 && br[2] < 60);
    // Progress fill (60 %) is blue-ish at 30 % of the bar and dark at 90 %.
    const ui::GuiRect pr = d->control_rect(static_cast<size_t>(d->find("IDC_SYN_PROGRESS")));
    const float cy = 0.5f * (pr.min.y + pr.max.y);
    const uint8_t* p30 = pixel(px, 1024, 768, gui.canvas().canvas_to_pixel(pr.min.x + 0.3f * pr.width(), cy));
    const uint8_t* p90 = pixel(px, 1024, 768, gui.canvas().canvas_to_pixel(pr.min.x + 0.9f * pr.width(), cy));
    CHECK(p30[2] > 200 && p90[2] < 100);
    CHECK(hs.quads > 20 && hs.page_uploads >= 1);
    CHECK(render::write_png_rgba(src_dir + "/logs/ui_gui_synthetic_1024x768.png", 1024, 768, px));
    std::printf("wrote %s/logs/ui_gui_synthetic_1024x768.png\n", src_dir.c_str());
    g.hud.clear_image_page(ui::kHudPageImageBase);
}

#ifdef ALAMO_UI_HAVE_VFS
void test_real(Gpu& g, const std::string& src_dir) {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real gui: ALAMO_FOC_DATA not set, skipped\n");
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

    ui::GuiData gd;
    CHECK(ui::load_gui_data(*files, gd, &err));
    CHECK(gd.script.merge(ui::kPortDialogsRc));  // [port: Gabriel] camera and controls dialog
    CHECK(gd.script.extend(ui::kPortDialogExtensionsRc));  // v-sync / frame cap / anti-aliasing / anisotropy controls
    ui::TextureAtlas atlas;
    std::vector<uint8_t> mtd;
    CHECK(files->read("Data\\Art\\Textures\\MT_CommandBar.mtd", mtd) && atlas.parse(mtd, &err));
    const auto atlas_tex = cache.texture("MT_CommandBar.tga");
    CHECK(atlas_tex && !atlas_tex->is_fallback);
    atlas.texture_width = atlas_tex->width;
    atlas.texture_height = atlas_tex->height;
    CHECK(g.hud.set_atlas_texture(*atlas_tex));
    uint32_t skin_in_atlas = 0, skin_total = 0;
    for (const char* k : {"Frame_Top_Left", "Frame_Background", "Button_Left", "Button_Middle", "Button_Right", "Check_On",
                          "Radio_On", "Dial_Tab", "Scroll_Tab", "Progress_Bar_Middle_On", "Small_Frame_Top_Left", "Combo_Box_Text_Box"}) {
        ++skin_total;
        if (atlas.find(gd.skin.texture("", "", k))) ++skin_in_atlas;
    }
    std::printf("real gui: %u / %u default skin pieces in MT_CommandBar (%zu entries)\n", skin_in_atlas, skin_total, atlas.size());
    ui::TextDatabase text;
    CHECK(ui::load_master_text(*files, "ENGLISH", text));
    data::GameDatabase db;
    db.load(*files);
    const ui::CampaignMenu menu = ui::build_campaign_menu(db);

    // The EmpireAtWar faces come from the game install (StarWarsG.exe embeds them), as the game host does.
    std::vector<std::string> font_roots = files->mod_dirs();
    font_roots.push_back(root);
    font_roots.push_back(files->eaw_root());
    ui::register_game_fonts(font_roots);
    CHECK(ui::find_game_font("EmpireAtWar-Bold") && ui::find_game_font("EmpireAtWar-Medium"));
    auto raster = ui::make_system_rasterizer();
    ui::FontCache fonts(*raster);
    ui::GuiManager gui;
    gui.init(gd.script, gd.skin, atlas, fonts, &text);
    ui::GuiImagePages pages;
    gui.set_image_resolver([&](std::string_view t) { return pages.resolve(t); });
    auto upload_pages = [&]() {
        for (ui::GuiImagePages::Entry& e : pages.entries()) {
            if (e.width != 0 || e.failed) continue;
            const auto tex = cache.texture(e.name);
            if (!tex || tex->is_fallback || !g.hud.set_image_page(e.page, *tex)) {
                std::printf("real gui: image %s not loaded\n", e.name.c_str());
                pages.set_failed(e.page);
            } else {
                pages.set_loaded(e.page, tex->width, tex->height);
            }
        }
    };
    ui::FrontEnd fe;
    fe.init(gui, menu, {});
    const uint32_t W = 1920, H = 1080;
    gui.resize(W, H);
    ui::HudDrawList empty;
    std::function<void(ui::HudDrawList&)> overlay;  // extra items after GuiManager::build (data chart lines)
    auto shoot = [&](const char* name, const std::function<void(double)>& prepare) {
        ui::HudDrawList dl;
        prepare(0.0);
        gui.build(dl, 1.0);  // registers stand-alone textures
        upload_pages();
        dl.clear();
        gui.build(dl, 1.0);
        if (overlay) overlay(dl);
        ui::HudDrawStats hs;
        const std::vector<uint8_t> px = render_frame(g, rr, W, H, gui.canvas(), empty, &dl, fonts, {}, {}, &hs);
        std::printf("real gui %s: %u quads, %u batches; dialogs %s\n", name, hs.quads, hs.batches,
                    gui.open_count() ? gui.open_names().back().c_str() : "-");
        const std::string path = src_dir + "/logs/ui_gui_" + name + ".png";
        CHECK(render::write_png_rgba(path, W, H, px));
        std::printf("wrote %s\n", path.c_str());
        return px;
    };
    // Main menu: column at the right edge over the backdrop; hover the Single Player button.
    {
        fe.show_main_menu();
        gui.set_version_text(U"V 1.121 (test build)");  // GALAXY_MAP_PRESENTATION 5: bottom-left version line
        const std::vector<uint8_t> px = shoot("main_menu", [&](double) {
            ui::GuiDialog* mm = gui.top();
            const ui::GuiRect r = mm->control_rect(static_cast<size_t>(mm->find("IDC_MAIN_MENU_SINGLE_PLAYER_GAMES")));
            const ui::Vec2f p = gui.canvas().canvas_to_pixel(0.5f * (r.min.x + r.max.x), 0.5f * (r.min.y + r.max.y));
            ui::InputFrame f;
            f.width = W;
            f.height = H;
            f.mouse_x = static_cast<int32_t>(p.x);
            f.mouse_y = static_cast<int32_t>(p.y);
            ui::RoutedInput ri;
            std::vector<ui::GuiEvent> ev;
            gui.update(f, ri, 1.0, ev);
        });
        // Button column (Tutorial .. Exit) is skinned; the logo button sits on the backdrop's painted logo.
        ui::GuiDialog* mm = gui.top();
        const ui::GuiRect t0 = mm->control_rect(static_cast<size_t>(mm->find("IDC_MAIN_MENU_TUTORIAL")));
        const ui::GuiRect t1 = mm->control_rect(static_cast<size_t>(mm->find("IDC_MAIN_MENU_EXIT2")));
        const double cov = coverage(px, W, H, gui.canvas().canvas_to_pixel(t0.min.x, t0.max.y), gui.canvas().canvas_to_pixel(t1.max.x, t1.min.y));
        std::printf("real gui main_menu: button column coverage %.1f%%\n", 100.0 * cov);
        CHECK(cov > 0.3);
        size_t loaded = 0;
        for (const ui::GuiImagePages::Entry& e : pages.entries()) loaded += e.width > 0 ? 1u : 0u;
        CHECK(loaded >= 2);  // MenuBack_overlay + MenuBack_overlay_panel
    }
    // Campaign selection (custom campaign staging) with the real campaign list.
    {
        std::vector<ui::MenuAction> acts;
        auto clicked = [&](std::string_view dialog, std::string_view control) {
            std::vector<ui::GuiEvent> e(1);
            e[0].kind = ui::GuiEvent::Kind::Clicked;
            e[0].dialog = std::string(dialog);
            e[0].control = std::string(control);
            fe.update(e, ui::InputFrame{}, acts);
        };
        fe.show_main_menu();
        gui.set_version_text(U"V 1.121 (test build)");  // GALAXY_MAP_PRESENTATION 5: bottom-left version line
        clicked("IDD_MAIN_MENU_DIALOG", "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES");
        clicked("IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG", "IDC_CUSTOM_CAMPAIGN_BUTTON");
        CHECK(fe.top() == "IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG");
        const std::vector<uint8_t> px = shoot("campaign_select", [&](double) {});
        const ui::GuiRect r = gui.top()->rect();
        const double cov = coverage(px, W, H, gui.canvas().canvas_to_pixel(r.min.x, r.max.y), gui.canvas().canvas_to_pixel(r.max.x, r.min.y));
        std::printf("real gui campaign_select: dialog coverage %.1f%%\n", 100.0 * cov);
        CHECK(cov > 0.3);
    }
    // Load screen at 60 %.
    {
        gui.close_all();
        ui::LoadScreen ls;
        ui::LoadScreenInfo info;
        info.planet_name = U"Tatooine";
        info.terrain = U"Desert";
        info.weather = U"Sandstorm";
        info.natives = U"Jawas";
        info.planet_icon = "load_tatooine.tga";
        info.planet_overlay = "load_overlay.tga";
        info.weather_icon = "load_weather_wind.tga";
        info.map_icon = "load_desert.tga";
        info.native_icon = "load_jawa.tga";
        info.history = {U"Outer Rim desert world.", U"Home of the Hutt crime lords."};
        ls.set_progress_source([] { return 0.6f; });
        CHECK(ls.open(gui, ui::LoadScreenKind::LandBattle, info));
        std::vector<ui::LoadScreenEvent> lev;
        ls.update({}, lev);
        const std::vector<uint8_t> px = shoot("load_screen", [&](double) {});
        const ui::GuiRect r = gui.top()->rect();
        const double cov = coverage(px, W, H, gui.canvas().canvas_to_pixel(r.min.x, r.max.y), gui.canvas().canvas_to_pixel(r.max.x, r.min.y));
        std::printf("real gui load_screen: dialog coverage %.1f%%\n", 100.0 * cov);
        CHECK(cov > 0.3 && !gui.top()->get("IDC_BEGIN_BUTTON")->visible);
        ls.close();
    }
    // Battle end (P4 completion): losses with real unit icons, heroes, bounty, countdown.
    {
        gui.close_all();
        gui.set_backdrop({});
        ui::GameScreens gs;
        gs.init(gui);
        ui::BattleEndInfo be;
        be.victory = true;
        be.planet = U"Kuat";
        be.time = U"Battle time 12:34";
        be.bounty = 1500;
        be.your_losses = {{"I_BUTTON_TIE_FIGHTER.TGA", U"TIE Fighter Squadron", 3}, {"I_BUTTON_TIE_BOMBER.TGA", U"TIE Bomber Squadron", 1},
                          {"i_button_interdictor_cruiser.tga", U"Interdictor", 1}};
        be.enemy_losses = {{"i_button_x_wing.tga", U"X-Wing Squadron", 4}, {"i_button_y_wing.tga", U"Y-Wing Squadron", 2},
                           {"i_button_corellian_corvette.tga", U"Corellian Corvette", 1}, {"i_button_mon_calimari_cruiser.tga", U"Mon Calamari Cruiser", 1},
                           {"i_button_skipray_blastboat.tga", U"Skipray Blastboat Long Range Attack Squadron Commander Edition", 12}};
        be.your_heroes = {"i_button_darth_vader.tga"};
        be.countdown = 15.0f;
        CHECK(gs.show_battle_end(be));
        const std::vector<uint8_t> px = shoot("battle_end", [&](double) {});
        const ui::GuiRect r = gui.top()->rect();
        const double cov = coverage(px, W, H, gui.canvas().canvas_to_pixel(r.min.x, r.max.y), gui.canvas().canvas_to_pixel(r.max.x, r.min.y));
        std::printf("real gui battle_end: dialog coverage %.1f%%\n", 100.0 * cov);
        CHECK(cov > 0.3);
        gs.close_all();
    }
    // LAN staging (multiplayer UI): host + AI + a joining player, map list, options.
    {
        gui.close_all();
        ui::LobbyScreens lob;
        lob.init(gui);
        lob.show_multiplayer_menu();
        ui::StagingState st;
        st.host = true;
        st.factions = {U"Rebel", U"Empire", U"Underworld"};
        st.colors = {U"Red", U"Blue", U"Green", U"Yellow"};
        st.color_values = {{200, 40, 30, 255}, {40, 90, 220, 255}, {40, 180, 60, 255}, {230, 200, 40, 255}};
        st.teams = {U"1", U"2", U"3", U"4"};
        st.maps = {U"Kuat (2)", U"Mon Calamari (4)", U"Hoth (2)", U"Coruscant (8)"};
        st.map = 1;
        st.options = {{U"Credits", U"10000"}, {U"Pop cap", U"20"}, {U"Heroes", U"On"}};
        ui::StagingSlot me, ai, other, open;
        me.state = ui::SlotState::Human;
        me.name = U"Gabriel";
        me.local = true;
        me.faction = 1;
        ai.state = ui::SlotState::AI;
        ai.faction = 2;
        ai.color = 1;
        ai.team = 1;
        other.state = ui::SlotState::Human;
        other.name = U"Wedge";
        other.accepted = true;
        other.color = 2;
        other.team = 1;
        st.slots = {me, ai, other, open, open};
        CHECK(lob.show_staging(st));
        for (const char* l : {"Gabriel: hello", "Wedge: ready when you are"}) lob.add_chat(ui::utf8_to_utf32(l));
        const std::vector<uint8_t> px = shoot("lan_staging", [&](double) {});
        const ui::GuiRect r = gui.top()->rect();
        const double cov = coverage(px, W, H, gui.canvas().canvas_to_pixel(r.min.x, r.max.y), gui.canvas().canvas_to_pixel(r.max.x, r.min.y));
        std::printf("real gui lan_staging: dialog coverage %.1f%%\n", 100.0 * cov);
        CHECK(cov > 0.3);
        lob.close_all();
        gui.set_backdrop({});
    }
    // [port: Gabriel] the camera and controls dialog over the options column.
    {
        gui.close_all();
        ui::HotkeyTable keys;
        ui::MenuProviders mp;
        mp.hotkeys = &keys;
        ui::FrontEnd fe2;
        fe2.init(gui, menu, mp);
        fe2.show_main_menu();
        std::vector<ui::MenuAction> acts;
        auto clicked = [&](std::string_view dialog, std::string_view control) {
            std::vector<ui::GuiEvent> e(1);
            e[0].kind = ui::GuiEvent::Kind::Clicked;
            e[0].dialog = std::string(dialog);
            e[0].control = std::string(control);
            fe2.update(e, ui::InputFrame{}, acts);
        };
        clicked("IDD_MAIN_MENU_DIALOG", "IDC_OPTIONS");
        CHECK(gui.top() && gui.top()->get("IDC_MAIN_OPTIONS_MODS")->visible);
        clicked("IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_MODS");
        CHECK(fe2.top() == ui::kPortControlsDialog);
        const std::vector<uint8_t> px = shoot("port_controls", [&](double) {});
        const ui::GuiRect r = gui.top()->rect();
        const double cov = coverage(px, W, H, gui.canvas().canvas_to_pixel(r.min.x, r.max.y), gui.canvas().canvas_to_pixel(r.max.x, r.min.y));
        std::printf("real gui port_controls: dialog coverage %.1f%%\n", 100.0 * cov);
        CHECK(cov > 0.3);
        // QA D8 / U11: the advanced video page and the pause menu at full size.
        gui.close_all();
        gui.open("IDD_VIDEO_OPTIONS_DIALOG");
        clicked("IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED");
        CHECK(fe2.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
        shoot("advanced_video", [&](double) {});
        for (const char* id : {"IDC_PORT_PARTICLE_DETAIL", "IDC_PORT_MESH_DETAIL", "IDC_PORT_TEXTURE_DETAIL", "IDC_PORT_WATER_DETAIL", "IDC_PORT_SHADER_DETAIL",
                               "IDC_PORT_SHADOW_DETAIL", "IDC_PORT_ENVIRONMENT_DETAIL"}) {
            const int32_t k = gui.top() ? gui.top()->find(id) : -1;
            CHECK(k >= 0 && gui.combo_rows(*gui.top(), static_cast<size_t>(k)) == static_cast<int32_t>(gui.top()->get(id)->items.size()));  // every level reachable
        }
        if (ui::GuiControl* tex = gui.top() ? gui.top()->get("IDC_PORT_TEXTURE_DETAIL") : nullptr) {  // a list with the longest level names
            tex->open = true;
            shoot("advanced_video_texture_open", [&](double) {});
            tex->open = false;
        }
        gui.close_all();
        fe2.show_pause_menu();
        shoot("pause_menu", [&](double) {});
        gui.close_all();
        gui.set_backdrop({});
    }
    // [port: Gabriel playtest] video options: one-line labels are fitted to their controls (no label runs into the
    // Off / Low value next to it, no Max leaves the dialog) and the device field is not a second "Video Resolution".
    {
        ui::FrontEnd fe3;
        ui::MenuProviders vp;
        vp.video = [] {
            ui::VideoSettings v;
            v.resolutions = {U"1280 x 720", U"1920 x 1080"};
            v.resolution = 1;
            return v;
        };
        fe3.init(gui, menu, vp);
        fe3.show_main_menu();
        std::vector<ui::MenuAction> acts;
        auto clicked = [&](std::string_view dialog, std::string_view control) {
            std::vector<ui::GuiEvent> e(1);
            e[0].kind = ui::GuiEvent::Kind::Clicked;
            e[0].dialog = std::string(dialog);
            e[0].control = std::string(control);
            fe3.update(e, ui::InputFrame{}, acts);
        };
        clicked("IDD_MAIN_MENU_DIALOG", "IDC_OPTIONS");
        clicked("IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO");
        CHECK(fe3.top() == "IDD_VIDEO_OPTIONS_DIALOG");
        ui::GuiDialog* vd = gui.top();
        if (vd && fe3.top() == "IDD_VIDEO_OPTIONS_DIALOG") {
            CHECK(vd->get("IDC_DEVICE_DESC_STATIC") && vd->get("IDC_DEVICE_DESC_STATIC")->text.empty());
            shoot("video_options", [&](double) {});
            // The two named-level lists open, one at a time (a list covers the rows under it): every level name fits its list.
            for (const auto& [id, shot] : {std::pair{"IDC_PORT_ANTI_ALIASING", "video_options_aa_open"}, std::pair{"IDC_PORT_DETAIL_LEVEL", "video_options_quality_open"}}) {
                ui::GuiControl* list = vd->get(id);
                CHECK(list != nullptr && list->items.size() >= 4);
                if (!list) continue;
                CHECK(gui.combo_rows(*vd, static_cast<size_t>(vd->find(id))) == static_cast<int32_t>(list->items.size()));  // no level cut off
                list->open = true;
                shoot(shot, [&](double) {});
                list->open = false;
            }
            // Every text glyph of a static lies inside its control rect (x) and inside the dialog.
            ui::HudDrawList dl;
            gui.build(dl, 1.0);
            const ui::GuiRect dr = vd->rect();
            // The video dialog is the top of the stack (main menu, options, video): its layer only.
            const uint32_t top_layer = static_cast<uint32_t>(std::min<size_t>(gui.open_count() - 1, 11));
            uint32_t outside = 0, glyphs = 0;
            for (uint32_t l = top_layer; l <= top_layer; ++l)
                for (const ui::HudQuad& q : dl.items(l, ui::HudList::Text)) {
                    ++glyphs;
                    if (q.min.x < dr.min.x - 1.0f || q.max.x > dr.max.x + 1.0f) ++outside;
                }
            // One static at a time (the others hidden): its glyphs stay inside its rect horizontally.
            uint32_t overflow = 0;
            const char* ids[] = {"IDC_ANTI_ALIAS_STATIC", "IDC_ANTI_ALIAS_OFF_STATIC", "IDC_ANTI_ALIAS_MAX_STATIC",
                                 "IDC_GAMMA_LABEL_STATIC", "IDC_GAMMA_LOW_STATIC", "IDC_GAMMA_HIGH_STATIC"};
            std::vector<uint8_t> was(vd->size());
            for (size_t k = 0; k < vd->size(); ++k) was[k] = (*vd)[k].visible ? 1 : 0;
            for (const char* id : ids) {
                const int32_t k = vd->find(id);
                if (k < 0) continue;
                for (size_t j = 0; j < vd->size(); ++j) (*vd)[j].visible = j == static_cast<size_t>(k);
                const ui::GuiRect r = vd->control_rect(static_cast<size_t>(k));
                ui::HudDrawList one;
                gui.build(one, 1.0);
                for (const ui::HudQuad& q : one.items(top_layer, ui::HudList::Text))
                    if (q.min.x < r.min.x - 1.0f || q.max.x > r.max.x + 1.0f) ++overflow;
            }
            for (size_t k = 0; k < vd->size(); ++k) (*vd)[k].visible = was[k] != 0;
            std::printf("real gui video_options: %u glyph quads, %u outside the dialog, %u outside their control\n", glyphs,
                        outside, overflow);
            CHECK(glyphs > 0 && outside == 0 && overflow == 0);
        }
        gui.close_all();
        gui.set_backdrop({});
    }
    // P4 completion 2: holocron pages, buy units, story, planet dialog with real icons (Icon_Name from the object XMLs).
    {
        std::map<std::string, std::string> icons;  // upper-case object name -> Icon_Name
        std::vector<uint8_t> list;
        if (files->read("Data\\XML\\GameObjectFiles.xml", list)) {
            const data::XmlDocument ld = data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(list.data()), list.size()), "g");
            for (data::XmlNode f : ld.root().children("File")) {
                std::vector<uint8_t> bytes;
                if (!files->read("Data\\XML\\" + std::string(f.text_trimmed()), bytes)) continue;
                const data::XmlDocument od = data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "o");
                for (data::XmlNode o : od.root().children()) {
                    const data::XmlNode ic = o.child("Icon_Name");
                    if (!ic) continue;
                    std::string key(o.attribute("Name"));
                    for (char& ch : key) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
                    icons.emplace(key, std::string(ic.text_trimmed()));
                }
            }
        }
        auto icon_of = [&](std::string_view type) {
            std::string key(type);
            for (char& ch : key) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
            const auto it = icons.find(key);
            return it == icons.end() ? std::string() : it->second;
        };
        std::printf("real gui: %zu object icons\n", icons.size());
        CHECK(icons.size() > 500);
        gui.close_all();
        ui::InfoScreens is;
        is.init(gui);
        size_t found = 0, total = 0;
        ui::TechTreeInfo tt;
        tt.dialog = "IDD_TECH_TREE_EMPIRE";
        tt.item = [&](std::string_view type, ui::TechTreeItem& out) {
            ++total;
            out.icon = icon_of(type);
            if (out.icon.empty()) return false;
            ++found;
            out.state = total % 3 == 0 ? ui::TechState::Locked : total % 3 == 1 ? ui::TechState::Researched : ui::TechState::Available;
            return true;
        };
        is.set_tech_tree(tt);
        CHECK(is.show_info(ui::InfoPage::TechTree));
        std::printf("real gui tech_tree: %zu / %zu items with icons\n", found, total);
        CHECK(found * 2 > total);
        auto check_cov = [&](const std::vector<uint8_t>& px, const char* name) {
            const ui::GuiRect r = gui.top()->rect();
            const double cov = coverage(px, W, H, gui.canvas().canvas_to_pixel(r.min.x, r.max.y), gui.canvas().canvas_to_pixel(r.max.x, r.min.y));
            std::printf("real gui %s: dialog coverage %.1f%%\n", name, 100.0 * cov);
            CHECK(cov > 0.3);
        };
        check_cov(shoot("tech_tree", [&](double) {}), "tech_tree");
        // Data chart with three players over 20 cycles.
        ui::DataChartInfo dc;
        const ui::Rgba8 cols[3] = {{220, 40, 30, 255}, {60, 120, 255, 255}, {60, 200, 80, 255}};
        const char32_t* names[3] = {U"Empire", U"Rebels", U"Underworld"};
        for (int p = 0; p < 3; ++p) {
            ui::ChartSeries s;
            s.name = names[p];
            s.color = cols[p];
            for (int k = 0; k < 20; ++k) s.values.push_back(1000.0f + 400.0f * static_cast<float>(p) + 150.0f * static_cast<float>(k) * static_cast<float>(1 + p % 2) + 300.0f * std::sin(0.7f * static_cast<float>(k + p)));
            dc.economy.push_back(s);
        }
        dc.first_cycle = 1;
        is.set_data_chart(dc);
        CHECK(is.show_info(ui::InfoPage::DataChart));
        size_t dots = 0;
        overlay = [&](ui::HudDrawList& l) { dots = is.build_overlay(l); };
        check_cov(shoot("data_chart", [&](double) {}), "data_chart");
        overlay = {};
        CHECK(dots > 500);
        // Summary (production) rows with hero icons.
        ui::SummaryInfo sm;
        const char* heroes[3] = {"Darth_Vader", "Boba_Fett", "Emperor_Palpatine"};
        const char* planets[4] = {"Coruscant", "Kuat", "Byss", "Carida"};
        for (int r = 0; r < 4; ++r) {
            ui::SummaryProductionRow row;
            row.name = ui::utf8_to_utf32(planets[r]);
            row.space_icon = icon_of("Star_Destroyer");
            row.ground_icon = icon_of("Imperial_Stormtrooper_Squad");
            row.space_text = U"35%";
            row.ground_text = U"20%";
            for (int h = 0; h <= r % 3; ++h) row.heroes.push_back(icon_of(heroes[h]));
            row.pop = U"8";
            sm.production.push_back(row);
        }
        sm.pop_total = U"32 / 40";
        is.set_summary(sm);
        CHECK(is.show_info(ui::InfoPage::SummaryProduction));
        check_cov(shoot("summary", [&](double) {}), "summary");
        is.close_all();
        // Skirmish buy units, story, planet dialog.
        ui::BuyUnitsState bs;
        bs.credits = 5000;
        for (const char* t : {"Imperial_Stormtrooper_Squad", "Imperial_Armor_Group", "Imperial_Artillery_Corp", "Imperial_Light_Scout_Squad"})
            bs.units.push_back({t, ui::utf8_to_utf32(t), 600, t[9] == 'S' ? 2u : 0u, icon_of(t), true});
        CHECK(is.show_buy_units(bs));
        check_cov(shoot("buy_units", [&](double) {}), "buy_units");
        is.close_all();
        ui::StoryDialogInfo sd;
        sd.title = U"The hunt for the Rebel base";
        sd.lines = {U"Admiral Piett reports Rebel activity in the Hoth system.", U"Send probe droids to locate the base.",
                    U"Reward: 2000 credits."};
        sd.image = icon_of("Darth_Vader");
        CHECK(is.show_story(sd));
        check_cov(shoot("story", [&](double) {}), "story");
        is.close_all();
        ui::PlanetDialogInfo pd;
        pd.name = U"Kuat";
        pd.credits = 125;
        pd.political = {icon_of("Darth_Vader")};
        pd.starbase = {icon_of("Star_Destroyer"), icon_of("Star_Destroyer")};
        pd.companies = 4;
        pd.fleets = 2;
        pd.producing = U"Star Destroyer";
        pd.progress = 0.4f;
        CHECK(is.show_planet(pd));
        check_cov(shoot("planet_dialog", [&](double) {}), "planet_dialog");
        is.close_all();
    }
    // Galactic HUD: shells, labels, pads, queue, fleets, objectives.
    {
        gui.close_all();
        ui::ComponentTable table;
        CHECK(table.load(*files, &err));
        ui::ShellSet shells;
        std::vector<std::string> missing;
        const std::string names[] = {"i_main_commandbar", "i_main_organize", "pending_battle_shell", "story_pending_battle_shell"};
        shells.load(table, [&](std::string_view n) { return cache.model(n); }, names, &missing);
        for (const std::string& m : missing) std::printf("real galactic: missing shell %s\n", m.c_str());
        CHECK(shells.shells().size() >= 2);  // the attack-mode banners live in the EaW GameData models only
        for (const ui::ShellSet::Shell& s : shells.shells()) {
            shells.set_alt(s.component, 0);  // Empire
            shells.set_visible(s.component, s.model_name.find("attack") == std::string::npos);  // banners: GalacticHud decides
            std::printf("real galactic: shell %s %zu bones, %zu meshes\n", s.model_name.c_str(), s.info.bones.size(), s.info.meshes.size());
        }
        ui::ShellDrawer drawer;
        const render::TextureSource tex = [&](std::string_view n, assets::TextureRole role) { return cache.texture(n, role); };
        CHECK(drawer.upload(rr, shells, tex, &err));
        drawer.wait_uploads(g.up);
        ui::GalacticHud hud;
        hud.init(table, atlas, fonts, &text);
        hud.resize(W, H);
        shells.apply(hud.layout());
        hud.layout().update(hud.canvas(), table);
        // Planets of a real campaign.
        const data::CampaignDef* camp = db.find_campaign("Sandbox_Equal_Footing_Empire");
        if (!camp && !db.campaigns().empty()) camp = &db.campaigns().front();
        std::vector<ui::GalacticPlanetInfo> planets;
        ui::Vec3 lo{1e9f, 1e9f, 0}, hi{-1e9f, -1e9f, 0};
        if (camp) {
            for (const std::string& loc : camp->locations) {
                const uint32_t pi = db.planets().find(loc);
                if (pi == kInvalidPlanet || loc == "Galaxy_Core_Art_Model") continue;
                ui::GalacticPlanetInfo p;
                p.planet = 1000 + pi;
                p.position = {db.planets().position[pi].x, db.planets().position[pi].y, db.planets().position[pi].z};
                p.radius = 12.0f;
                p.name = ui::utf16_to_utf32(text.text("TEXT_PLANET_" + loc));
                if (p.name.empty() || p.name.front() == U'T') p.name = ui::utf8_to_utf32(loc);
                p.credits = db.planets().credit_value[pi];
                const size_t k = planets.size();
                p.owner_color = k % 3 == 0 ? ui::Rgba8{255, 80, 60, 255} : k % 3 == 1 ? ui::Rgba8{80, 160, 255, 255} : ui::Rgba8{200, 200, 80, 255};
                p.political_control = static_cast<float>(k % 5) / 4.0f;
                p.ground_level = static_cast<int32_t>(k % 4);
                p.space_level = static_cast<int32_t>((k + 1) % 5);
                p.conflict = k == 2;
                p.hero_empire = k == 3;
                lo.x = std::min(lo.x, p.position.x);
                lo.y = std::min(lo.y, p.position.y);
                hi.x = std::max(hi.x, p.position.x);
                hi.y = std::max(hi.y, p.position.y);
                planets.push_back(p);
            }
        }
        std::printf("real galactic: campaign %s, %zu planets\n", camp ? camp->name.c_str() : "-", planets.size());
        CHECK(planets.size() >= 4);
        if (planets.size() > 1) planets[1].selected = true;
        if (planets.size() > 1) planets[1].allegiance = 1;  // GALACTIC_DIALOGS 5.3 emblem (Empire)
        ui::GalacticCameraController cam;
        cam.set_look_at({0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.0f});
        ui::GalacticHudFrame f;
        f.view = ui::make_view(cam.params(static_cast<float>(W) / static_cast<float>(H)), W, H);
        f.planets = planets;
        f.credits = 8650;
        f.pop = 12;
        f.pop_cap = 40;
        f.day = 17;
        f.tech = 3;
        f.faction_alt = 1;
        f.shells_loaded = true;
        f.time = 1.0;
        f.holocron_flash = true;
        f.organize_open = true;  // planet close-up with a selected planet
        f.providers.build_options = [](uint32_t, std::vector<ui::GalacticBuildOption>& out) {
            for (const char* icon : {"I_BUTTON_TIE_FIGHTER.TGA", "I_BUTTON_TIE_BOMBER.TGA", "I_BUTTON_ACCLAMATOR.TGA", "i_button_victory_class.tga"}) {
                ui::GalacticBuildOption o;
                o.icon = icon;
                o.cost = 500;
                o.space = icon[9] == 'T';  // 5.1: the TIE options on blue (space) pads, the rest amber
                out.push_back(o);
            }
            ui::GalacticBuildOption s;
            s.icon = "I_BUTTON_EMPIRE_BARRACKS.TGA";
            s.structure = true;
            out.push_back(s);
        };
        f.providers.build_queue = [](std::vector<ui::HudQueueEntry>& out) {
            ui::HudQueueEntry e;
            e.icon = "I_BUTTON_TIE_FIGHTER.TGA";
            e.progress = 0.4f;
            e.count = 2;
            out.push_back(e);
            e.icon = "I_BUTTON_EMPIRE_BARRACKS.TGA";
            e.count = 1;
            e.land = true;  // 5.2: the ground queue on the orange domes
            out.push_back(e);
        };
        f.providers.fleets = [](uint32_t, std::vector<ui::GalacticFleet>& out) {
            ui::GalacticFleet fl;
            fl.fleet = 5000;
            fl.units = {{5001, "i_button_victory_class.tga", 1, false}, {5002, "I_BUTTON_TIE_FIGHTER.TGA", 3, true}};
            out.push_back(fl);
        };
        f.providers.objectives = [](std::vector<ui::HudObjective>& out) {
            out.push_back({"", U"Capture Kuat to unlock the Star Destroyer", ui::HudObjective::State::Active});
            out.push_back({"", U"Build a level 2 space station", ui::HudObjective::State::Complete});
            out.push_back({"", U"Keep the Emperor alive", ui::HudObjective::State::Failed});
        };
        hud.build(f);
        // The HUD's shell wishes (organiser / banners), applied as the host does.
        for (const ui::GalacticShellVisibility& v : hud.shell_visibility()) shells.set_visible(v.component, v.visible);
        shells.apply(hud.layout());
        hud.build(f);
        // Test-only planet dots (no planet models in this frame).
        ui::HudDrawList dl = hud.draw_list();
        for (const ui::GalacticPlanetInfo& p : planets) {
            float sx = 0, sy = 0;
            if (!ui::project(f.view, p.position, sx, sy)) continue;
            const ui::Vec2f c = hud.canvas().pixel_to_canvas(sx, sy);
            ui::HudQuad q;
            q.min = {c.x - 5, c.y - 5};
            q.max = {c.x + 5, c.y + 5};
            q.color = {170, 190, 230, 255};
            dl.add(0, ui::HudList::Alpha, q);
        }
        const ui::GalacticHudStats& s = hud.stats();
        std::printf("real galactic: %u labels, %u icons, %u buttons, %u build, %u queue, %u fleet, %u objectives, %u hit boxes\n",
                    s.labels, s.label_icons, s.buttons, s.build_buttons, s.queue_entries, s.fleet_slots, s.objectives, s.hit_boxes);
        CHECK(s.labels >= 4 && s.build_buttons >= 4 && s.objectives == 3 && s.buttons > 5);
        // The shell step needs the frame, so these frames are recorded here rather than through render_frame.
        using ShellStep = std::function<void(rhi::Frame&, VkCommandBuffer, uint32_t, render::DrawStats&)>;
        auto render_shells = [&](const ui::HudDrawList& list, const ShellStep& draw_shells, const std::function<void()>& resident,
                                 render::DrawStats& ss, const ui::HudDrawList* cursor_list = nullptr) {
            std::vector<uint8_t> out;
            Offscreen o;
            CHECK(o.create(g, W, H));
            for (int k = 0; k < 64; ++k) {
                rhi::Frame* fr = g.ring.begin_frame();
                if (!fr) break;
                VkCommandBuffer cmd = fr->cmd();
                const rhi::DeviceFns& fn = g.ctx.vkd();
                const rhi::AcquireResult acq = g.up.record_acquires(cmd);
                resident();
                render::PassTarget pt;
                pt.color = o.color.handle;
                pt.color_view = o.color.view;
                pt.depth = o.depth.handle;
                pt.depth_view = o.depth.view;
                pt.extent = {W, H};
                pt.clear[2] = 0.03f;
                pt.color_after = rhi::ImageState::TransferSrc;
                render::RecordOptions ro;
                ro.shadows = false;
                ro.space = true;
                CHECK(rr.render(*fr, cmd, pt, ui::to_view_matrices(cam.params(static_cast<float>(W) / static_cast<float>(H)), W, H), {},
                                render::DrawMode::Unlit, ro, nullptr));
                ui::HudTarget ht;
                ht.color = o.color.handle;
                ht.color_view = o.color.view;
                ht.depth = o.depth.handle;
                ht.depth_view = o.depth.view;
                ht.extent = {W, H};
                ht.before = ht.after = rhi::ImageState::TransferSrc;
                ss = {};
                CHECK(g.hud.record(*fr, cmd, ht, hud.canvas(), list, &fonts, [&](VkCommandBuffer c, const ui::HudFrameStep& step) {
                    if (step.kind == ui::HudStepKind::ShellModels) draw_shells(*fr, c, step.first, ss);
                }, nullptr, nullptr, cursor_list));
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {W, H, 1};
                fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
                const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                                      VK_ACCESS_2_HOST_READ_BIT);
                rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
                VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
                rhi::FrameSubmit fs;
                if (acq.wait_value) fs.waits = {&wait, 1};
                CHECK(g.ring.end_frame(*fr, fs));
                g.ring.wait_idle();
                if ((ss.draws > 0 && ss.pending_pipeline == 0 && ss.translation_pending == 0) || k == 63) break;
                rr.wait_translations();
                rr.pipelines().wait_idle();
            }
            g.alloc.invalidate(o.readback.alloc);
            const auto* p = static_cast<const uint8_t*>(o.readback.mapped());
            out.assign(p, p + static_cast<size_t>(W) * H * 4);
            o.destroy(g);
            return out;
        };
        auto accumulate = [](render::DrawStats& acc, const render::DrawStats& s1) {
            acc.draws += s1.draws;
            acc.pending_pipeline += s1.pending_pipeline;
            acc.translation_pending += s1.translation_pending;
        };
        auto galactic_shells = [&](rhi::Frame& fr, VkCommandBuffer c, uint32_t half, render::DrawStats& acc) {
            render::DrawStats s1;
            drawer.draw(rr, fr, c, hud.canvas(), shells, hud.layout(), half, &s1);
            accumulate(acc, s1);
        };
        render::DrawStats ss{};
        const std::vector<uint8_t> px2 = render_shells(dl, galactic_shells, [&] { drawer.make_resident(rr); }, ss);
        std::printf("real galactic: shell draws %u (pending %u)\n", ss.draws, ss.pending_pipeline);
        CHECK(ss.draws >= 1 && ss.pending_pipeline == 0);
        const double cov = coverage(px2, W, H, {0.0f, static_cast<float>(H) * 0.85f}, {static_cast<float>(W) * 0.5f, static_cast<float>(H)});
        std::printf("real galactic: bottom-left coverage %.1f%%\n", 100.0 * cov);
        CHECK(cov > 0.3);
        const std::string path = src_dir + "/logs/ui_gui_galactic_hud.png";
        CHECK(render::write_png_rgba(path, W, H, px2));
        std::printf("wrote %s\n", path.c_str());

        // GALACTIC_DIALOGS 1: the battle-pending banner (pending_battle_shell) over the same HUD, plus the screen-text lines.
        {
            ui::GalacticHudFrame pf = f;
            pf.battle.active = true;
            pf.battle.space = true;
            pf.battle.planet = planets.size() > 1 ? planets[1].name : U"Kuat";
            pf.battle.left = {"I_BUTTON_ACCLAMATOR.TGA", 9, -1};
            pf.battle.right = {"i_button_victory_class.tga", 21, -1};
            const ui::GalacticScreenText lines[2] = {
                {U"The mission holocron has been updated.", ui::Rgba8{255, 255, 255, 255}, 255},
                {ui::auto_resolve_text(&text, true, true, 2, U"Empire", pf.battle.planet, 4), ui::kAutoResolveTextColor, 255}};
            pf.screen_text = lines;
            hud.build(pf);
            for (const ui::GalacticShellVisibility& v : hud.shell_visibility()) shells.set_visible(v.component, v.visible);
            shells.apply(hud.layout());
            hud.build(pf);
            std::printf("real galactic: battle prompt %d (on shell %d), %u graphs, %u screen lines\n", hud.stats().battle_prompt ? 1 : 0,
                        hud.stats().battle_prompt_shell ? 1 : 0, hud.stats().battle_graphs, hud.stats().screen_text_lines);
            CHECK(hud.stats().battle_prompt && hud.stats().battle_graphs == 2 && hud.stats().screen_text_lines == 2);
            render::DrawStats ps{};
            const std::vector<uint8_t> px3 = render_shells(hud.draw_list(), galactic_shells, [&] { drawer.make_resident(rr); }, ps);
            const std::string ppath = src_dir + "/logs/ui_gui_galactic_prompt.png";
            CHECK(render::write_png_rgba(ppath, W, H, px3));
            std::printf("wrote %s\n", ppath.c_str());
            hud.build(f);
            for (const ui::GalacticShellVisibility& v : hud.shell_visibility()) shells.set_visible(v.component, v.visible);
            shells.apply(hud.layout());
            hud.build(f);
        }

        // P4 completion: hero roster (Empire frame), black market shell and the encyclopedia pop-up over the galactic HUD.
        ui::ShellSet pshells;
        const std::string pnames[] = {"evil_hero_frame", "market_frame", "hack_shell"};
        missing.clear();
        pshells.load(table, [&](std::string_view n) { return cache.model(n); }, pnames, &missing);
        for (const std::string& m : missing) std::printf("real panels: missing shell %s\n", m.c_str());
        CHECK(pshells.shells().size() == 3);
        for (const ui::ShellSet::Shell& psh : pshells.shells()) {
            pshells.set_alt(psh.component, 0);
            pshells.set_visible(psh.component, psh.model_name != "i_hack.alo");
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
            for (const ui::ShellBone& b : psh.info.bones) {
                bx0 = std::min(bx0, b.x);
                bx1 = std::max(bx1, b.x);
                by0 = std::min(by0, b.y);
                by1 = std::max(by1, b.y);
            }
            float mx0 = 1e9f, mx1 = -1e9f;
            for (const ui::ShellMesh& m : psh.info.meshes) {
                const ui::ShellBone* bone = m.bone > 0 && m.bone - 1 < psh.info.bones.size() ? &psh.info.bones[m.bone - 1] : nullptr;
                const float ox = bone ? bone->x : 0.0f;
                mx0 = std::min(mx0, ox + m.min.x);
                mx1 = std::max(mx1, ox + m.max.x);
            }
            std::printf("real panels: shell %s %zu bones (x %.0f..%.0f, y %.0f..%.0f), %zu meshes (x %.0f..%.0f)\n", psh.model_name.c_str(),
                        psh.info.bones.size(), static_cast<double>(bx0), static_cast<double>(bx1), static_cast<double>(by0),
                        static_cast<double>(by1), psh.info.meshes.size(), static_cast<double>(mx0), static_cast<double>(mx1));
        }
        ui::ShellDrawer pdrawer;
        CHECK(pdrawer.upload(rr, pshells, tex, &err));
        pdrawer.wait_uploads(g.up);
        ui::HeroPanel heroes;
        ui::BlackMarketPanel market;
        CHECK(heroes.init(table, atlas, fonts, &text) && market.init(table, atlas, fonts, &text));
        heroes.resize(W, H);
        market.resize(W, H);
        pshells.apply(heroes.layout());
        pshells.apply(market.layout());
        const ui::HeroEntry hero_list[3] = {{6001, "i_button_darth_vader.tga", 0.8f, -1.0f, true, 0, {}, alamo::commands::kNoObject, alamo::commands::kNoObject, {}},
                                            {6002, "i_button_boba_fett.tga", 0.35f, -1.0f, false, 0, {}, alamo::commands::kNoObject, alamo::commands::kNoObject, {}},
                                            {6003, "i_button_captain_firmus_piett.tga", 0.0f, 0.6f, false, 0, {}, alamo::commands::kNoObject, alamo::commands::kNoObject, {}}};
        ui::HeroPanelFrame hf;
        hf.open = true;
        hf.good = false;
        hf.heroes = hero_list;
        hf.shells_loaded = true;
        hf.faction_alt = 1;
        hf.time = 1.0;
        heroes.build(hf);
        const ui::BlackMarketItem bm_items[3] = {
            {401, "i_button_EV_TIE_Defender.tga", U"TIE Defender plans", U"Unlocks the TIE Defender at your shipyards.", 3000, true, "i_button_e_tech_upgrade_"},
            {402, "i_button_broadside.tga", U"Hull plating", U"", 1200, true, "i_button_e_tech_upgrade_"},
            {403, "i_button_victory_class.tga", U"Ion cannon upgrade", U"", 2500, false, ""}};
        ui::BlackMarketFrame bf;
        bf.open = true;
        bf.planet = 1001;
        bf.title = U"Black Market";
        bf.subtitle = U"Credits: 8650";
        bf.items = bm_items;
        bf.shells_loaded = true;
        bf.faction_alt = 1;
        bf.time = 1.0;
        market.build(bf);
        ui::EncyclopediaPanel enc;
        enc.init(table, atlas, fonts, &text);
        enc.update(table.find("market_00"), 0, 0.0);
        enc.update(table.find("market_00"), 0, 5.0);  // past Long_Encyclopedia_Delay
        ui::EncyclopediaEntry ee;
        ee.header = U"TIE Defender plans";
        ee.icon = "i_button_EV_TIE_Defender.tga";
        ee.cost = U"3000";
        ee.rows = {{U"Build time", U"45 s", {}}, {U"Tech level", U"4", {}}, {{}, {}, U"Black market item"}};
        ee.description = U"Stolen designs for the Empire's most advanced starfighter. Grants the TIE Defender to every shipyard.";
        ui::HudDrawList pdl = dl;
        for (const ui::HudDrawList* src : {&heroes.draw_list(), &market.draw_list()})
            for (uint32_t layer = 0; layer < ui::kHudLayers; ++layer)
                for (uint32_t li = 0; li < ui::kHudSubLists; ++li)
                    for (const ui::HudQuad& q : src->items(layer, static_cast<ui::HudList>(li))) pdl.add(layer, static_cast<ui::HudList>(li), q);
        const ui::Vec2f enc_size = enc.build(pdl, hud.canvas(), ee, {static_cast<float>(W) * 0.62f, static_cast<float>(H) * 0.35f});
        std::printf("real panels: hero hits %zu, market hits %zu, encyclopedia %.0f x %.0f units\n", heroes.hit_boxes().size(),
                    market.hit_boxes().size(), static_cast<double>(enc_size.x), static_cast<double>(enc_size.y));
        CHECK(heroes.hit_boxes().size() == 3 && market.hit_boxes().size() >= 4 && enc_size.x > 0);
        render::DrawStats ps{};
        const std::vector<uint8_t> px3 = render_shells(
            pdl,
            [&](rhi::Frame& fr, VkCommandBuffer c, uint32_t half, render::DrawStats& acc) {
                galactic_shells(fr, c, half, acc);
                render::DrawStats s2;
                pdrawer.draw(rr, fr, c, heroes.canvas(), pshells, heroes.layout(), half, &s2);
                accumulate(acc, s2);
            },
            [&] {
                drawer.make_resident(rr);
                pdrawer.make_resident(rr);
            },
            ps);
        std::printf("real panels: shell draws %u (pending %u)\n", ps.draws, ps.pending_pipeline);
        CHECK(ps.draws > ss.draws && ps.pending_pipeline == 0);
        // The market panel's rectangle (its hit boxes) is covered by art.
        ui::Vec2f lo2{1e9f, 1e9f}, hi2{-1e9f, -1e9f};
        for (const ui::HudHitBox& h : market.hit_boxes()) {
            lo2 = {std::min(lo2.x, h.min.x), std::min(lo2.y, h.min.y)};
            hi2 = {std::max(hi2.x, h.max.x), std::max(hi2.y, h.max.y)};
        }
        const double mcov = coverage(px3, W, H, hud.canvas().canvas_to_pixel(lo2.x, hi2.y), hud.canvas().canvas_to_pixel(hi2.x, lo2.y));
        std::printf("real panels: market coverage %.1f%%\n", 100.0 * mcov);
        CHECK(mcov > 0.3);
        const std::string ppath = src_dir + "/logs/ui_gui_panels.png";
        CHECK(render::write_png_rgba(ppath, W, H, px3));
        std::printf("wrote %s\n", ppath.c_str());
        // P4 completion 2: hack shell, a fleet being dragged onto a planet, and the mouse cursor (POINTER_DROP) drawn at
        // the Cursor step.
        {
            for (const ui::ShellSet::Shell& psh : pshells.shells())
                pshells.set_visible(psh.component, psh.model_name == "i_hack.alo" || psh.model_name == "i_main_hero_frame_empire.alo");
            ui::HackPanel hack;
            CHECK(hack.init(table, atlas, fonts, &text));
            hack.resize(W, H);
            pshells.apply(hack.layout());
            ui::HackFrame hk;
            hk.open = true;
            hk.target = 7001;
            hk.planet = 1001;
            hk.title = U"Hack: shield generator";
            hk.lines = {U"Disables the planetary shield", U"for the next battle.", U"Requires a slicer in orbit."};
            hk.cost = U"750";
            hk.shells_loaded = true;
            hk.faction_alt = 1;
            hk.time = 1.0;
            hack.build(hk);
            // Drag fleet slot q1_0001 towards a planet.
            const int32_t qc = table.find("q1_0001");
            ui::Vec2f qp{-1, -1};
            for (const ui::HudHitBox& h : hud.hit_boxes())
                if (h.component == qc) qp = hud.canvas().canvas_to_pixel(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y));
            CHECK(qp.x >= 0);
            // A planet in the middle of the screen (not the selected one).
            float tx = 0, ty = 0;
            for (const ui::GalacticPlanetInfo& p : planets) {
                float px = 0, py = 0;
                if (p.selected || !ui::project(f.view, p.position, px, py)) continue;
                if (px > static_cast<float>(W) * 0.35f && px < static_cast<float>(W) * 0.6f && py > static_cast<float>(H) * 0.15f &&
                    py < static_cast<float>(H) * 0.5f) {
                    tx = px;
                    ty = py;
                    break;
                }
            }
            CHECK(tx > 0);
            std::vector<commands::EventData> dcmds;
            ui::RoutedInput ri;
            ri.world.mouse_x = static_cast<int32_t>(qp.x);
            ri.world.mouse_y = static_cast<int32_t>(qp.y);
            ri.hover_component = qc;
            ri.owner[static_cast<size_t>(ui::Button::Left)] = ui::PointerTarget::Hud;
            ri.hud = {{ui::HudPointerEvent::Kind::Press, ui::Button::Left, qc, ri.world.mouse_x, ri.world.mouse_y}};
            hud.update_input(ri, false, false, 1.0, dcmds);
            ri.hud.clear();
            ri.hover_component = -1;
            ri.world.mouse_x = static_cast<int32_t>(tx);
            ri.world.mouse_y = static_cast<int32_t>(ty);
            hud.update_input(ri, false, false, 1.1, dcmds);
            CHECK(hud.drag().active && hud.drag().over_planet >= 0);
            hud.build(f);
            ui::HudDrawList hdl = hud.draw_list();
            for (const ui::HudDrawList* src : {&heroes.draw_list(), &hack.draw_list()})
                for (uint32_t layer = 0; layer < ui::kHudLayers; ++layer)
                    for (uint32_t li = 0; li < ui::kHudSubLists; ++li)
                        for (const ui::HudQuad& q : src->items(layer, static_cast<ui::HudList>(li))) hdl.add(layer, static_cast<ui::HudList>(li), q);
            // Cursor from the shipped MousePointers.xml; frame textures from the game data (image pages).
            std::vector<uint8_t> mp;
            CHECK(files->read("Data\\XML\\MousePointers.xml", mp));
            const data::XmlDocument mdoc = data::XmlDocument::parse(std::string_view(reinterpret_cast<const char*>(mp.data()), mp.size()), "m");
            ui::CursorTable ct;
            ct.parse(mdoc.root());
            ct.resolve_frames([&](std::string_view n) {
                const auto t = cache.texture(n);
                return t && !t->is_fallback;
            });
            ui::MouseCursor mc;
            mc.init(ct, &atlas, [&](std::string_view t) { return pages.resolve(t); });
            ui::CursorContext cc;
            cc.fleet_drag = true;
            cc.drop_on_planet = hud.drag().over_planet >= 0;
            cc.drop_valid = hud.drag().valid;
            mc.update(ui::choose_pointer(cc), 1.0);
            ui::HudDrawList cdl;
            mc.build(cdl, hud.canvas(), {tx, ty}, 1.2);  // registers the image page
            upload_pages();
            cdl.clear();
            const bool cursor_drawn = mc.build(cdl, hud.canvas(), {tx, ty}, 1.2);
            std::printf("real cursor: %s, texture %s, %zu frames, drawn %d\n", ui::pointer_name(mc.pointer()), mc.texture(1.2).c_str(),
                        ct.find(mc.pointer()) ? ct.find(mc.pointer())->frames.size() : 0u, cursor_drawn ? 1 : 0);
            CHECK(cursor_drawn && mc.pointer() == ui::Pointer::Drop);
            render::DrawStats hs{};
            const std::vector<uint8_t> px4 = render_shells(
                hdl,
                [&](rhi::Frame& fr, VkCommandBuffer c, uint32_t half, render::DrawStats& acc) {
                    galactic_shells(fr, c, half, acc);
                    render::DrawStats s2;
                    pdrawer.draw(rr, fr, c, hack.canvas(), pshells, hack.layout(), half, &s2);
                    accumulate(acc, s2);
                },
                [&] {
                    drawer.make_resident(rr);
                    pdrawer.make_resident(rr);
                },
                hs, &cdl);
            // The cursor's pixels: its quad rectangle is lit.
            const auto cq = cdl.items(ui::kHudLayers - 1, ui::HudList::Alpha);
            std::printf("real cursor: target px %.0f %.0f, drag quads %zu, cursor quads %zu", static_cast<double>(tx), static_cast<double>(ty),
                        hdl.items(ui::kHudLayers - 1, ui::HudList::Alpha).size(), cq.size());
            if (!cq.empty())
                std::printf(" at %.1f %.1f .. %.1f %.1f page %u", static_cast<double>(cq[0].min.x), static_cast<double>(cq[0].min.y),
                            static_cast<double>(cq[0].max.x), static_cast<double>(cq[0].max.y), static_cast<unsigned>(cq[0].page));
            std::printf("\n");
            if (!cq.empty()) {
                const double ccov = coverage(px4, W, H, hud.canvas().canvas_to_pixel(cq[0].min.x, cq[0].max.y),
                                             hud.canvas().canvas_to_pixel(cq[0].max.x, cq[0].min.y));
                std::printf("real cursor: coverage %.1f%%\n", 100.0 * ccov);
                CHECK(ccov > 0.05);
            }
            std::printf("real hack: shell draws %u (pending %u), hack hits %zu\n", hs.draws, hs.pending_pipeline, hack.hit_boxes().size());
            CHECK(hs.pending_pipeline == 0 && hack.hit_boxes().size() >= 2);
            const std::string hpath = src_dir + "/logs/ui_gui_hack_cursor.png";
            CHECK(render::write_png_rgba(hpath, W, H, px4));
            std::printf("wrote %s\n", hpath.c_str());
            // Release over the planet -> MoveObjectToObject for the dragged unit.
            ri.owner = {};
            ri.hud = {{ui::HudPointerEvent::Kind::Release, ui::Button::Left, -1, ri.world.mouse_x, ri.world.mouse_y}};
            hud.update_input(ri, false, false, 1.3, dcmds);
            CHECK(dcmds.size() == 1 && dcmds[0].type() == commands::EventType::MoveObjectToObject && dcmds[0].u("object") == 5001);
        }
        g.ring.wait_idle();
        pdrawer.destroy(rr);
        drawer.destroy(rr);
    }
    g.ring.wait_idle();
    rr.collect_released();
    rr.shutdown();
}
#else
void test_real(Gpu&, const std::string&) { std::printf("real gui: alamo_vfs not configured, skipped\n"); }
#endif

void run(const std::string& src_dir) {
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
    cd.app_name = "alamo_ui_gui_draw_tests";
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
    ui::HudRendererDesc hd;
    hd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    hd.depth_format = VK_FORMAT_D32_SFLOAT;
    const bool hinit = g.hud.init(g.ctx, g.alloc, g.up, hd);
    CHECK(hinit);
    if (hinit) {
        render::Renderer rr;
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.depth_format = VK_FORMAT_D32_SFLOAT;
        CHECK(rr.init(g.ctx, g.alloc, g.up, rd));
        rr.set_frame_ring(&g.ring);
        test_synthetic(g, rr, src_dir);
        g.ring.wait_idle();
        rr.shutdown();
        test_real(g, src_dir);
    }
    g.ring.wait_idle();
    g.hud.shutdown();
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
    run(ALAMO_UI_SOURCE_DIR);
    if (g_fail) {
        std::printf("alamo_ui_gui_draw_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_gui_draw_tests: all passed\n");
    return 0;
}

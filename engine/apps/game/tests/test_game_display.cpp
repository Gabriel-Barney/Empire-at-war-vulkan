// alamo_game_display_tests: the video options host helpers (app/game_display.hpp) - the resolution list's mode text
// (the original's "%d x %d (%dHz)"), the list selection by size and refresh rate, the video options file round trip, and
// the advanced detail options' mapping onto the renderer's frame options. Nothing here changes the display.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../app/game_display.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/render/renderer.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace alamo;
using game::DisplayMode;

void labels_and_index() {
    CHECK(game::display_mode_label({1920, 1080, 60}) == "1920 x 1080 (60Hz)");
    CHECK(game::display_mode_label({800, 600, 144}) == "800 x 600 (144Hz)");
    CHECK(game::display_mode_label({1024, 768, 0}) == "1024 x 768");
    const std::vector<DisplayMode> m = {{1280, 720, 60}, {1280, 720, 120}, {1920, 1080, 60}, {1920, 1080, 144}, {1920, 1080, 165}};
    CHECK(game::display_mode_index(m, 1920, 1080, 144) == 3);
    CHECK(game::display_mode_index(m, 1920, 1080, 75) == 4);   // rate not offered: the highest rate of that size
    CHECK(game::display_mode_index(m, 1920, 1080) == 4);
    CHECK(game::display_mode_index(m, 1600, 900, 60) == 1);    // no such size: the largest mode under it
    CHECK(game::display_mode_index(m, 640, 480, 60) == -1);
    CHECK(game::display_mode_index(m, 1920, 1080, 143) == 3);  // QA U10: a folded rate selects its twin, not the highest
    CHECK(game::display_mode_index(m, 1280, 720, 59) == 0);
}

// QA U10: 119 / 120 and the like collapse to the higher rate per size; other rates and sizes stay.
void collapse_rates() {
    std::vector<DisplayMode> m = {{1920, 1080, 59},  {1920, 1080, 60},  {1920, 1080, 75},  {2560, 1440, 119},
                                  {3840, 2160, 75},  {3840, 2160, 100}, {3840, 2160, 119}, {3840, 2160, 120},
                                  {3840, 2160, 144}, {3840, 2160, 160}};
    game::collapse_refresh_rates(m);
    const std::vector<DisplayMode> want = {{1920, 1080, 60}, {1920, 1080, 75},  {2560, 1440, 119}, {3840, 2160, 75},
                                           {3840, 2160, 100}, {3840, 2160, 120}, {3840, 2160, 144}, {3840, 2160, 160}};
    CHECK(m == want);
    std::vector<DisplayMode> none = {{800, 600, 0}, {800, 600, 60}};
    game::collapse_refresh_rates(none);
    CHECK(none.size() == 2);
}

void real_modes() {
    // The machine's own list: sorted, distinct, every entry with a refresh rate the driver reported (or 0).
    const std::vector<DisplayMode> m = game::display_modes();
    std::printf("display modes: %zu", m.size());
    if (!m.empty()) std::printf(" (%s .. %s)", game::display_mode_label(m.front()).c_str(), game::display_mode_label(m.back()).c_str());
    std::printf("\n");
    for (size_t i = 1; i < m.size(); ++i) {
        const DisplayMode& a = m[i - 1];
        const DisplayMode& b = m[i];
        CHECK(a.width < b.width || (a.width == b.width && (a.height < b.height || (a.height == b.height && a.hz < b.hz))));
    }
#ifdef _WIN32
    // Every rate the list offers at the desktop's resolution is one the primary display accepts (CDS_TEST only: the
    // display is not switched) - the list item apply_display_mode would set.
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) {
        uint32_t rates = 0;
        for (const DisplayMode& d : m) {
            if (d.width != cur.dmPelsWidth || d.height != cur.dmPelsHeight || d.hz == 0) continue;
            DEVMODEW want = cur;
            want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
            want.dmDisplayFrequency = d.hz;
            const LONG rc = ChangeDisplaySettingsExW(nullptr, &want, nullptr, CDS_FULLSCREEN | CDS_TEST, nullptr);
            if (rc != DISP_CHANGE_SUCCESSFUL) std::printf("  %s: CDS_TEST %ld\n", game::display_mode_label(d).c_str(), static_cast<long>(rc));
            CHECK(rc == DISP_CHANGE_SUCCESSFUL);
            ++rates;
        }
        std::printf("desktop %lux%lu: %u refresh rates listed, each checked with CDS_TEST\n", cur.dmPelsWidth, cur.dmPelsHeight, rates);
        // QA U10: no size offers two rates one apart.
        for (size_t i = 1; i < m.size(); ++i)
            CHECK(!(m[i - 1].width == m[i].width && m[i - 1].height == m[i].height && m[i - 1].hz != 0 && m[i - 1].hz + 1 == m[i].hz));
    }
#endif
}

// t12: a resolution change waiting on "keep these settings?" is not stored; Yes stores it; No / timeout stores the old mode.
void profile_resolution_confirmation() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "alamo_game_display_tests_confirm";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "video_options.txt").string();
    const std::vector<game::DisplayMode> modes = {{1280, 720, 60}, {1920, 1080, 60}, {2560, 1440, 144}};
    game::VideoProfile profile;
    profile.mode = modes[1];
    CHECK(game::save_video_profile(path, profile));
    ui::VideoSettings v;
    v.resolution = 2;
    v.vsync = false;
    v.unconfirmed = true;  // tried, the dialog is open
    CHECK(!game::update_video_profile(profile, v, modes) && profile.mode == modes[1] && profile.vsync);
    game::VideoProfile on_disk;
    CHECK(game::load_video_profile(path, on_disk) && on_disk.mode == modes[1]);
    // No / timeout: the previous resolution comes back confirmed and is what the file holds.
    v.unconfirmed = false;
    v.resolution = 1;
    CHECK(game::update_video_profile(profile, v, modes) && game::save_video_profile(path, profile));
    CHECK(game::load_video_profile(path, on_disk) && on_disk.mode == modes[1] && !on_disk.vsync);
    // Yes: the new one is stored.
    v.resolution = 2;
    CHECK(game::update_video_profile(profile, v, modes) && game::save_video_profile(path, profile));
    CHECK(game::load_video_profile(path, on_disk) && on_disk.mode == modes[2]);
    // An index outside the list leaves the stored mode.
    v.resolution = 7;
    CHECK(game::update_video_profile(profile, v, modes) && profile.mode == modes[2]);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void profile_round_trip() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "alamo_game_display_tests";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "video_options.txt").string();
    game::VideoProfile p;
    p.mode = {2560, 1440, 165};
    p.vsync = false;
    p.vsync_mode = ui::VsyncMode::Mailbox;
    p.frame_cap = ui::kFrameCapDisplay;
    p.hardware_mouse = true;
    p.detail = 40;
    p.gamma = 70;
    p.advanced.anti_aliasing = ui::AntiAliasing::Taa;
    p.advanced.ambient_occlusion = true;
    p.advanced.anisotropy = 8;
    p.advanced.mesh = 25;
    p.advanced.bloom = false;
    CHECK(game::save_video_profile(path, p));
    game::VideoProfile q;
    CHECK(game::load_video_profile(path, q));
    CHECK(q.mode == p.mode);
    CHECK(!q.vsync && q.vsync_mode == ui::VsyncMode::Mailbox && q.frame_cap == ui::kFrameCapDisplay && q.hardware_mouse);
    CHECK(q.detail == 40 && q.gamma == 70 && q.anti_alias == 0);
    CHECK(q.advanced == p.advanced);
    // A partial file (hand-written) keeps the other values.
    {
        std::ofstream f(path, std::ios::trunc);
        f << "# comment\r\nanti_aliasing = taa\r\nanisotropy=16\n";
    }
    game::VideoProfile r;
    CHECK(game::load_video_profile(path, r));
    CHECK(r.advanced.anti_aliasing == ui::AntiAliasing::Taa && r.advanced.anisotropy == 16);
    CHECK(r.mode.width == 0 && r.vsync && !r.advanced.ambient_occlusion && r.advanced.bloom);
    CHECK(!game::load_video_profile((dir / "missing.txt").string(), r));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void detail_options() {
    render::RecordOptions o;
    game::apply_detail_options(o, ui::AdvancedVideoSettings{});  // defaults: the view's own frame options
    // Soft shadows default on in the dialog: the checkbox now reaches the renderer (playtest 3 t10: it could only turn them off).
    CHECK(o.lod_detail == 1.0f && o.shadows && o.soft_shadows && o.bloom_strength < 0.0f && o.heat_amount < 0.0f);
    CHECK(o.shader_detail == 3 && o.water_detail == 2);  // slider 100 = the renderer's maximum
    render::RecordOptions soft;
    soft.soft_shadows = true;
    ui::AdvancedVideoSettings no_soft;
    no_soft.soft_shadows = false;
    game::apply_detail_options(soft, no_soft);
    CHECK(!soft.soft_shadows);
    game::apply_detail_options(soft, ui::AdvancedVideoSettings{});  // and back on
    CHECK(soft.soft_shadows);
    ui::AdvancedVideoSettings s;
    s.mesh = 50;
    s.shadow = 0;
    s.bloom = false;
    s.heat_distortion = false;
    render::RecordOptions p;
    game::apply_detail_options(p, s);
    CHECK(p.lod_detail == 0.5f && !p.shadows && !p.soft_shadows && p.bloom_strength == 0.0f && p.heat_amount == 0.0f);
    render::RecordOptions space;
    space.shadows = false;  // a view without shadows never gets soft shadows
    game::apply_detail_options(space, ui::AdvancedVideoSettings{});
    CHECK(!space.shadows && !space.soft_shadows);
}

void detail_mappings() {
    // t11: the remaining sliders bind to real renderer parameters; 100 / default = today's look.
    CHECK(game::shader_level(100) == 3 && game::shader_level(0) == 0 && game::shader_level(50) == 2 && game::shader_level(17) == 1);
    CHECK(game::water_level(100) == 2 && game::water_level(50) == 1 && game::water_level(0) == 0 && game::water_level(24) == 0);
    CHECK(game::texture_reduction(100) == 0 && game::texture_reduction(50) == 1 && game::texture_reduction(0) == 2);
    CHECK(game::particle_detail(100) == 1.0f && game::particle_detail(0) == 0.0f && game::particle_detail(250) == 1.0f);
    CHECK(game::environment_fade_scale(100) == 1.0f && std::fabs(game::environment_fade_scale(0) - 0.6f) < 1e-6f);
    CHECK(game::gamma_value(50) == 1.0f && game::gamma_value(100) == 2.0f && game::gamma_value(0) == 0.5f);
    ui::AdvancedVideoSettings adv;
    CHECK(!game::taa_active(adv, 0) && game::taa_active(adv, 1));  // the slider alone switches the resolve on
    adv.anti_aliasing = ui::AntiAliasing::Taa;
    CHECK(game::taa_active(adv, 0));
    render::RecordOptions o;
    game::apply_aa_options(o, adv, 0);
    CHECK(o.taa && o.taa_weight < 0.0f);  // the tuned default
    game::apply_aa_options(o, adv, 100);
    CHECK(o.taa && std::fabs(o.taa_weight - 0.04f) < 1e-6f);
    adv.anti_aliasing = ui::AntiAliasing::Off;
    game::apply_aa_options(o, adv, 0);
    CHECK(!o.taa);
    // t19 / D9: the slider and the combo are one setting in the profile, including one written before they were.
    {
        game::VideoProfile aa;
        ui::VideoSettings vs;
        vs.anti_alias = 40;
        CHECK(game::update_video_profile(aa, vs, {}) && aa.advanced.anti_aliasing == ui::AntiAliasing::Taa);
        vs.anti_alias = 0;
        CHECK(game::update_video_profile(aa, vs, {}) && aa.advanced.anti_aliasing == ui::AntiAliasing::Off);
        game::sync_anti_alias(aa);
        CHECK(aa.anti_alias == 0 && aa.advanced.anti_aliasing == ui::AntiAliasing::Off);
        aa.advanced.anti_aliasing = ui::AntiAliasing::Taa;  // old file: combo on, slider 0
        game::sync_anti_alias(aa);
        CHECK(aa.anti_alias == ui::kAntiAliasDefaultSlider && aa.advanced.anti_aliasing == ui::AntiAliasing::Taa);
        aa.anti_alias = 30;  // old file: combo off, slider above 0
        aa.advanced.anti_aliasing = ui::AntiAliasing::Off;
        game::sync_anti_alias(aa);
        CHECK(aa.anti_alias == 30 && aa.advanced.anti_aliasing == ui::AntiAliasing::Taa);
    }
    // The overall Detail slider as a preset: the seven sliders follow, the checks / AA / anisotropy stay.
    ui::AdvancedVideoSettings custom;
    custom.bloom = false;
    custom.anisotropy = 8;
    custom.anti_aliasing = ui::AntiAliasing::Taa;
    custom.mesh = 10;
    const ui::AdvancedVideoSettings low = game::detail_preset(custom, 30);
    CHECK(low.particle == 30 && low.mesh == 30 && low.texture == 30 && low.water == 30 && low.shader == 30 && low.shadow == 30 &&
          low.environment == 30);
    CHECK(!low.bloom && low.anisotropy == 8 && low.anti_aliasing == ui::AntiAliasing::Taa && low.soft_shadows == custom.soft_shadows);
    CHECK(game::detail_preset(custom, 400).mesh == 100);
    // t43: and the seven give the Detail slider its level, a preset exactly.
    game::VideoProfile dp;
    dp.detail = 100;
    dp.advanced = low;
    game::sync_detail(dp);
    CHECK(dp.detail == 30);
    dp.advanced.mesh = 100;  // custom mix: the rounded mean
    game::sync_detail(dp);
    CHECK(dp.detail == 40);
}
}  // namespace

int main() {
    labels_and_index();
    collapse_rates();
    real_modes();
    profile_round_trip();
    profile_resolution_confirmation();
    detail_options();
    detail_mappings();
    std::printf(g_fail ? "alamo_game_display_tests: %d FAILED\n" : "alamo_game_display_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}

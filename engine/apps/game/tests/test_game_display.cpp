// alamo_game_display_tests: the video options host helpers (app/game_display.hpp) - the resolution list's mode text
// (the original's "%d x %d (%dHz)"), the list selection by size and refresh rate, the video options file round trip, and
// the advanced detail options' mapping onto the renderer's frame options. Nothing here changes the display.
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "../app/game_display.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/test/support.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using ::alamo::test::g_fail;

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
    p.windowed = false;    // UI5 t01
    p.widescreen = false;
    p.detail = 40;
    p.gamma = 70;
    p.anti_alias = 63;
    p.advanced.ambient_occlusion = true;
    p.advanced.anisotropy = 8;
    p.advanced.mesh = 25;
    p.advanced.bloom = false;
    p.advanced.render_scale = 75;
    CHECK(game::save_video_profile(path, p));
    game::VideoProfile q;
    CHECK(game::load_video_profile(path, q));
    CHECK(q.mode == p.mode);
    CHECK(!q.vsync && q.vsync_mode == ui::VsyncMode::Mailbox && q.frame_cap == ui::kFrameCapDisplay && q.hardware_mouse);
    CHECK(q.detail == 40 && q.gamma == 70 && q.anti_alias == 63);
    CHECK(q.advanced == p.advanced);
    CHECK(!q.windowed && !q.widescreen);
    {  // UI5 t01: Widescreen off lists modes no wider than 4:3; all when none is
        const std::vector<game::DisplayMode> all{{1024, 768, 60}, {1280, 1024, 60}, {1920, 1080, 60}, {2560, 1440, 144}};
        const std::vector<game::DisplayMode> narrow = game::video_list_modes(all, false);
        CHECK(narrow.size() == 2 && narrow[0].width == 1024 && narrow[1].width == 1280);
        CHECK(game::video_list_modes(all, true).size() == 4);
        const std::vector<game::DisplayMode> wide{{1920, 1080, 60}};
        CHECK(game::video_list_modes(wide, false).size() == 1);
        ui::VideoSettings v;
        v.windowed = true;
        v.widescreen = true;
        game::VideoProfile r;
        r.windowed = r.widescreen = false;
        CHECK(game::update_video_profile(r, v, all) && r.windowed && r.widescreen);
    }
    // A partial file (hand-written) keeps the other values.
    {
        std::ofstream f(path, std::ios::trunc);
        f << "# comment\r\nanti_aliasing = taa\r\nanisotropy=16\nrender_scale=5\n";
    }
    game::VideoProfile r;
    CHECK(game::load_video_profile(path, r));
    CHECK(r.anti_alias == ui::kAntiAliasDefaultSlider && r.advanced.anisotropy == 16);
    CHECK(r.advanced.render_scale == 25);  // clamped to render::kRenderScaleMin
    CHECK(game::render_scale_value(75) == 0.75f && game::render_scale_value(100) == 1.0f && game::render_scale_value(0) == 0.25f);
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

// A list of named levels (ui::LevelChoice) tells the truth when each level's value has a different effect in the renderer's own
// mapping `f` from its neighbours', the level starts exactly where that effect starts, and every stored value 0..100 has the
// effect of the level it falls in.
template <size_t N, class F>
void effect(const ui::LevelChoice (&levels)[N], F f) {
    const std::span<const ui::LevelChoice> list(levels);
    for (size_t i = 0; i < N; ++i) {
        CHECK(f(levels[i].value) == f(levels[i].from));
        if (i > 0) CHECK(f(levels[i].from) != f(levels[i - 1].value) && f(levels[i].from - 1) == f(levels[i - 1].value));
    }
    for (int32_t v = 0; v <= 100; ++v) {
        const size_t k = ui::level_index(list, v);
        if (f(v) != f(levels[k].value)) std::printf("value %d: effect %d, but its level %zu (value %d) gives %d\n", v, f(v), k, levels[k].value, f(levels[k].value));
        CHECK(f(v) == f(levels[k].value));
    }
}

void detail_mappings() {
    // t11: the remaining sliders bind to real renderer parameters; 100 / default = today's look.
    CHECK(game::shader_level(100) == 3 && game::shader_level(0) == 0 && game::shader_level(50) == 2 && game::shader_level(17) == 1);
    CHECK(game::water_level(100) == 2 && game::water_level(50) == 1 && game::water_level(0) == 0 && game::water_level(24) == 0);
    CHECK(game::texture_reduction(100) == 0 && game::texture_reduction(50) == 1 && game::texture_reduction(0) == 2);
    CHECK(game::particle_detail(100) == 1.0f && game::particle_detail(0) == 0.0f && game::particle_detail(250) == 1.0f);
    CHECK(game::environment_fade_scale(100) == 1.0f && std::fabs(game::environment_fade_scale(0) - 0.6f) < 1e-6f);
    CHECK(game::gamma_value(50) == 1.0f && game::gamma_value(100) == 2.0f && game::gamma_value(0) == 0.5f);
    CHECK(!game::taa_active(0) && game::taa_active(1));  // any strength above 0 switches the resolve on
    render::RecordOptions o;
    game::apply_aa_options(o, 100);
    CHECK(o.taa && std::fabs(o.taa_weight - 0.04f) < 1e-6f);
    game::apply_aa_options(o, ui::kAntiAliasDefaultSlider);
    CHECK(o.taa && std::fabs(o.taa_weight - 0.1f) < 0.002f);  // Medium = the tuned default weight
    game::apply_aa_options(o, 0);
    CHECK(!o.taa && o.taa_weight < 0.0f);
    // Anti-aliasing is one value in the profile: the Video dialog's list sets it, and a profile written when the Advanced dialog
    // had its own box ("anti_aliasing=taa") still turns it on.
    {
        game::VideoProfile aa;
        ui::VideoSettings vs;
        vs.anti_alias = 40;
        CHECK(game::update_video_profile(aa, vs, {}) && aa.anti_alias == 40 && game::taa_active(aa.anti_alias));
        vs.anti_alias = 0;
        CHECK(game::update_video_profile(aa, vs, {}) && aa.anti_alias == 0 && !game::taa_active(aa.anti_alias));
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "alamo_game_display_aa";
        std::filesystem::create_directories(dir);
        const std::string path = (dir / "video_options.txt").string();
        const auto load = [&](const char* text) {
            {
                std::ofstream f(path, std::ios::trunc);
                f << text;
            }
            game::VideoProfile r;
            CHECK(game::load_video_profile(path, r));
            return r.anti_alias;
        };
        CHECK(load("anti_aliasing=taa\nanti_alias=0\n") == ui::kAntiAliasDefaultSlider);  // old file: box on, slider 0
        CHECK(load("anti_aliasing=off\nanti_alias=30\n") == 30);                           // old file: box off, slider above 0
        CHECK(load("anti_aliasing=taa\nanti_alias=20\n") == 20);                           // strength kept
        CHECK(load("anti_alias=0\n") == 0);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    // The named levels of the dialogs tell the truth: every level a list offers does something different from its neighbours in the
    // renderer's own mapping, and a stored value falls in the level the renderer treats it as (all 101 values, every option).
    {
        effect(ui::kLevels4, [](int32_t v) { return game::shader_level(v); });
        effect(ui::kLevels3, [](int32_t v) { return game::water_level(v); });
        effect(ui::kTextureChoices, [](int32_t v) { return static_cast<int32_t>(game::texture_reduction(v)); });
        effect(ui::kShadowChoices, [](int32_t v) {
            ui::AdvancedVideoSettings s;
            s.shadow = v;
            render::RecordOptions r;
            game::apply_detail_options(r, s);
            return r.shadows ? 1 : 0;
        });
        for (const ui::LevelChoice& c : ui::kAntiAliasChoices) CHECK(game::taa_active(c.value) == (c.value > 0));
        // The four quality levels are four different looks.
        std::array<int, 4> shader{}, tex{};
        for (size_t i = 0; i < 4; ++i) {
            shader[i] = game::shader_level(ui::kQualityPresets[i]);
            tex[i] = static_cast<int>(game::texture_reduction(ui::kQualityPresets[i]));
        }
        CHECK(shader == (std::array<int, 4>{0, 1, 2, 3}) && tex == (std::array<int, 4>{2, 1, 0, 0}));
    }
    // The overall quality level as a preset: the seven options follow, the checks / anisotropy stay.
    ui::AdvancedVideoSettings custom;
    custom.bloom = false;
    custom.anisotropy = 8;
    custom.mesh = 10;
    const ui::AdvancedVideoSettings low = game::detail_preset(custom, 30);
    CHECK(low.particle == 30 && low.mesh == 30 && low.texture == 30 && low.water == 30 && low.shader == 30 && low.shadow == 30 &&
          low.environment == 30);
    CHECK(!low.bloom && low.anisotropy == 8 && low.soft_shadows == custom.soft_shadows);
    CHECK(game::detail_preset(custom, 400).mesh == 100);
    for (size_t i = 0; i < ui::kQualityCount; ++i) CHECK(ui::quality_level(game::detail_preset(custom, ui::kQualityPresets[i])) == i);
    CHECK(ui::quality_level(custom) == ui::kQualityCustom);  // mesh 10 (Low) amid Ultra
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

void auto_detect() {
    constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    const auto gpu = [](game::GpuKind k, uint64_t gib, float aniso) {
        game::GpuClass g;
        g.kind = k;
        g.memory_bytes = gib * kGiB;
        g.sampler_anisotropy = aniso > 1.0f;
        g.max_anisotropy = aniso;
        return g;
    };
    using game::GpuKind;
    const game::AutoVideoChoice big = game::recommend_video(gpu(GpuKind::Discrete, 12, 16.0f));
    CHECK(big.quality == 3 && big.advanced.anisotropy == 16 && big.advanced.ambient_occlusion && big.anti_alias == ui::kAntiAliasChoices[3].value);
    CHECK(ui::quality_level(big.advanced) == 3);
    const game::AutoVideoChoice mid = game::recommend_video(gpu(GpuKind::Discrete, 4, 16.0f));
    CHECK(mid.quality == 2 && mid.advanced.anisotropy == 8 && !mid.advanced.ambient_occlusion && ui::quality_level(mid.advanced) == 2);
    const game::AutoVideoChoice small = game::recommend_video(gpu(GpuKind::Discrete, 2, 16.0f));
    CHECK(small.quality == 1 && small.advanced.anisotropy == 4 && !small.advanced.soft_shadows && ui::quality_level(small.advanced) == 1);
    CHECK(game::recommend_video(gpu(GpuKind::Integrated, 8, 16.0f)).quality == 1);
    const game::AutoVideoChoice weak = game::recommend_video(gpu(GpuKind::Integrated, 1, 16.0f));
    CHECK(weak.quality == 0 && weak.anti_alias == 0 && weak.advanced.anisotropy == 2 && ui::quality_level(weak.advanced) == 0);
    const game::AutoVideoChoice soft = game::recommend_video(gpu(GpuKind::Other, 16, 1.0f));
    CHECK(soft.quality == 0 && soft.advanced.anisotropy == 0 && !soft.advanced.ambient_occlusion);
    // Filtering never exceeds the device's limit, and is off without the feature.
    CHECK(game::recommend_video(gpu(GpuKind::Discrete, 12, 4.0f)).advanced.anisotropy == 4);
    CHECK(game::recommend_video(gpu(GpuKind::Discrete, 12, 0.0f)).advanced.anisotropy == 0);
    // Every recommended anisotropy is a listed choice.
    for (game::GpuKind k : {GpuKind::Other, GpuKind::Integrated, GpuKind::Discrete})
        for (uint64_t gib : {0u, 1u, 2u, 4u, 8u})
            for (float a : {0.0f, 2.0f, 8.0f, 16.0f}) {
                const int32_t v = game::recommend_video(gpu(k, gib, a)).advanced.anisotropy;
                bool listed = false;
                for (int32_t c : ui::kAnisotropyChoices) listed = listed || c == v;
                CHECK(listed);
            }
    // Stored in the profile: the options, the anti-aliasing strength and the Detail value; mode, v-sync and gamma stay.
    game::VideoProfile p;
    p.vsync = false;
    p.gamma = 61;
    p.mode = {1920, 1080, 144};
    game::apply_auto_video(p, mid);
    CHECK(p.advanced == mid.advanced && p.anti_alias == mid.anti_alias && p.detail == ui::kQualityPresets[2]);
    CHECK(!p.vsync && p.gamma == 61 && p.mode.width == 1920);
}
// ui1 t02: the options file round trip and the game speed index.
void options_round_trip() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "alamo_game_display_tests";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "game_options.txt").string();
    game::OptionsProfile p;
    p.audio.master = 0.25f;
    p.audio.music = 0.0f;
    p.audio.subtitles = false;
    p.gameplay.game_speed = 75;
    p.gameplay.strategic_speed = 0;
    p.gameplay.alternate_mouse = true;
    p.gameplay.abilities_autofire = true;
    p.gameplay.ask_autoresolve = false;
    p.gameplay.difficulty = 2;
    p.camera.tactical_fov_deg = 70.0f;
    p.camera.queue_mods = 2;
    p.tactical_fov_set = true;
    ui::HotkeyTable t;
    ui::HotkeyBinding b = t.binding(20);
    b.key = 'Q';
    t.bind(20, b);
    t.save(p.keys);
    CHECK(game::save_options_profile(path, p));
    game::OptionsProfile q;
    CHECK(game::load_options_profile(path, q));
    CHECK(std::fabs(q.audio.master - 0.25f) < 1e-4f && q.audio.music == 0.0f && q.audio.sfx == 1.0f && !q.audio.subtitles);
    CHECK(q.gameplay.game_speed == 75 && q.gameplay.strategic_speed == 0 && q.gameplay.alternate_mouse &&
          q.gameplay.abilities_autofire && !q.gameplay.ask_autoresolve && q.gameplay.difficulty == 2 && q.gameplay.absolute_scroll);
    CHECK(std::fabs(q.camera.tactical_fov_deg - 70.0f) < 1e-3f && q.camera.queue_mods == 2 && q.tactical_fov_set);
    CHECK(q.keys == p.keys);
    ui::HotkeyTable u;
    CHECK(u.load(q.keys) && u.binding(20).key == 'Q');
    CHECK(!game::load_options_profile((dir / "missing_options.txt").string(), q));
    CHECK(game::game_speed_index(0) == 0 && game::game_speed_index(50) == 2 && game::game_speed_index(100) == 4 &&
          game::game_speed_index(62) == 2 && game::game_speed_index(63) == 3 && game::game_speed_index(-5) == 0);
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
    auto_detect();
    options_round_trip();
    std::printf(g_fail ? "alamo_game_display_tests: %d FAILED\n" : "alamo_game_display_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}

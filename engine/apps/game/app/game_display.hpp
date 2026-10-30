// alamo_game host: display modes for the video options dialog (IDD_VIDEO_OPTIONS_DIALOG's IDC_RESOLUTION_LIST) and the
// window resize its Apply / OK performs. Windows only (EnumDisplaySettings / SetWindowPos); kept out of game_main.cpp so
// <windows.h> stays in this one translation unit.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/ui/gui_menus.hpp"

namespace alamo::render {
struct RecordOptions;
}

namespace alamo::game {

struct DisplayMode {
    uint32_t width = 0, height = 0;
    uint32_t hz = 0;  // refresh rate; 0 = unknown / the driver default
    friend bool operator==(const DisplayMode&, const DisplayMode&) = default;
};

// The display modes of the monitor showing the window `hwnd` (HWND; null = the primary display): distinct width x
// height x refresh rate (32-bit colour, at least 800 x 600), ascending by width, height, then refresh rate - the
// original lists every adapter mode with its rate (IDC_RESOLUTION_LIST). Empty when the query fails.
// Near-duplicate rates are folded (collapse_refresh_rates).
std::vector<DisplayMode> display_modes(void* hwnd = nullptr);
// QA U10 [port]: Windows reports 119.88 Hz as 119 next to a true 120 Hz (59 / 60, 23 / 24 likewise). A rate whose next
// integer is also offered at the same size is dropped; the higher one stays. `modes` is sorted as display_modes sorts.
void collapse_refresh_rates(std::vector<DisplayMode>& modes);
// The list item text, the original's format "%d x %d (%dHz)": "1920 x 1080 (60Hz)" ("1920 x 1080" when hz is 0).
std::string display_mode_label(const DisplayMode& m);
// Index in `modes` of `w` x `h` at `hz` or `hz` + 1 (a folded rate, so a monitor on 119 selects 120), else at the highest
// rate of that size, else of the largest mode not bigger
// than it, else -1.
int32_t display_mode_index(const std::vector<DisplayMode>& modes, uint32_t w, uint32_t h, uint32_t hz = 0);
// Applies a display mode from the video options [port: windowed / borderless, no exclusive full screen]: the window's
// monitor is switched to the mode's refresh rate (and, for a mode that does not fit the work area - the window then
// covers the monitor borderless - to the mode's resolution) with a temporary ChangeDisplaySettingsEx (CDS_FULLSCREEN:
// Windows restores the user's mode when the process ends; restore_display_mode() does it at once), then the window's
// client area is resized (set_window_client_size). A rate the monitor does not offer at its current resolution leaves
// the rate alone. `log` gets one line describing what changed. False when the window resize failed.
bool apply_display_mode(void* hwnd, const DisplayMode& m, std::string* log = nullptr);
// Puts back the monitor mode apply_display_mode changed (no-op when it changed nothing).
void restore_display_mode();

// [port] The video options kept between runs (the original keeps them in its registry profile, MAIN_LOOP.md §2 step 9;
// the port writes a key = value text file, %LOCALAPPDATA%/alamo/video_options.txt). `mode` 0 x 0 = not stored.
struct VideoProfile {
    DisplayMode mode;
    bool vsync = true, hardware_mouse = false;
    ui::VsyncMode vsync_mode = ui::VsyncMode::Fifo;
    int32_t frame_cap = 0;
    int32_t detail = 100, anti_alias = 0, gamma = 50;  // IDD_VIDEO_OPTIONS_DIALOG sliders (kept for the dialog)
    ui::AdvancedVideoSettings advanced;
};
// Copies an applied ApplyVideo's settings into the stored profile; false (profile untouched) while `v.unconfirmed` - a
// resolution change waiting on the "keep these settings?" dialog (ui::VideoSettings::unconfirmed): the Yes answer, or the
// revert to the previous mode on No / timeout, comes as a later ApplyVideo with unconfirmed false and stores then. `modes`
// is the list the resolution index refers to.
// The Anti-aliasing slider also sets p.advanced.anti_aliasing (t19 / D9, ui::anti_aliasing_from_slider).
bool update_video_profile(VideoProfile& p, const ui::VideoSettings& v, const std::vector<DisplayMode>& modes);
// t19 / D9: a profile from before the slider and the AA combo were one setting: either one on means TAA, and both then say so.
void sync_anti_alias(VideoProfile& p);
// t43: the stored Detail slider is the level of the seven advanced sliders (ui::advanced_detail_level); applied with them and
// to a profile saved before the two agreed.
void sync_detail(VideoProfile& p);
std::string default_video_profile_path();  // "" when %LOCALAPPDATA% is unset
// Missing keys keep `p`'s values; false when the file cannot be read.
bool load_video_profile(const std::string& path, VideoProfile& p);
bool save_video_profile(const std::string& path, const VideoProfile& p);
// The original's advanced detail options that map onto the renderer's frame options, applied on top of the view's own
// options: Mesh detail -> RecordOptions::lod_detail (slider / 100, RENDER.md §5), Shadow detail 0 -> no shadow volumes
// [inferred: the dialog has no separate Shadow volumes check], Bloom (off: strength 0), Heat distortions (off: distortion
// amount 0), Soft shadows -> RecordOptions::soft_shadows (the checkbox now turns them on as well as off; they need shadows;
// playtest 3 fixed the darkened frame in Renderer::render), Shader detail -> RecordOptions::shader_detail (0..3, 0 = no
// shadow volumes + hard darken, caps water), Water detail -> RecordOptions::water_detail (0..2 reflection / refraction
// targets) [M: the sliders map to the integer options by rounding, see shader_level / water_level].
void apply_detail_options(render::RecordOptions& o, const ui::AdvancedVideoSettings& s);
// The other settings the renderer can bind to [M: the base game's slider-to-value tables are not recovered; these are
// documented choices, all 100 / default = today's look]:
//   shader_level(slider)  0..3  = round(slider * 3 / 100);   water_level(slider) 0..2 = round(slider * 2 / 100)
//   texture_reduction(slider)   top mips skipped by Renderer::set_texture_reduction: 2 - round(slider * 2 / 100) (100: none, 0: two)
//   particle_detail(slider)     ParticleWorld::set_detail: slider / 100 (ANIM_PARTICLES B.4: the option is 0..1)
//   environment_fade_scale(slider)  Object_Fade_Begin / End scale for the battle camera: 0.6 + 0.4 * slider / 100
//   gamma_value(slider)         Options > Video > Gamma, default 50 = 1.0 (identity), 2^((slider - 50) / 50): 0.5 .. 2; > 1 brightens
//   taa_active(adv, aa_slider)  the AA combo says TAA, or the Anti-aliasing slider is above 0
//   apply_aa_options(o, adv, aa_slider)  RecordOptions::taa / taa_weight: slider 0 with TAA picked keeps the tuned default (0.1);
//                                        slider 1..100 lerps the current-frame weight 0.2 -> 0.04 (smoother, more history)
int shader_level(int32_t slider);
int water_level(int32_t slider);
uint32_t texture_reduction(int32_t slider);
float particle_detail(int32_t slider);
float environment_fade_scale(int32_t slider);
float gamma_value(int32_t slider);
bool taa_active(const ui::AdvancedVideoSettings& s, int32_t aa_slider);
void apply_aa_options(render::RecordOptions& o, const ui::AdvancedVideoSettings& s, int32_t aa_slider);
// The overall Detail slider (IDD_VIDEO_OPTIONS_DIALOG) as a preset [M]: a changed Detail slider sets the seven advanced
// detail sliders (particle, mesh, texture, water, shader, shadow, environment) to its value; the checks (soft shadows, heat,
// bloom), the AA mode and anisotropy are left as they are.
ui::AdvancedVideoSettings detail_preset(const ui::AdvancedVideoSettings& s, int32_t detail);
// Per-monitor DPI awareness (v2) for the process, so window sizes, the client area and the display modes are all physical
// pixels; without it a scaled display (150 %) stretches the window and a mode the size of the display no longer fits.
// Call before the first window. No-op off Windows or when the call is refused.
void enable_dpi_awareness();
// Resizes the window `hwnd` (HWND) so its client area is w x h; false when the call fails. A framed window that fits
// the work area of its monitor stays framed (kept on screen); a size that does not fit (e.g. the display's own
// resolution) becomes a borderless window covering that monitor, at most the monitor's size [port: the original
// switches to fullscreen]. A later size that fits restores the frame. The window's WM_SIZE then reaches the host's
// resize path (Window::take_resized).
bool set_window_client_size(void* hwnd, uint32_t w, uint32_t h);
// Refresh rate (Hz) of the monitor showing the window `hwnd` (HWND): its current display mode's frequency; 0 when unknown
// (the query fails, or the driver reports the hardware default as 0 / 1).
uint32_t monitor_refresh_hz(void* hwnd);
// Frame rate cap [port: Gabriel]: wait() once per frame (after present) holds the loop to `fps` frames per second. Each
// frame's deadline is the last one plus the period, so the rate holds without drift; a frame that misses its deadline
// starts a new schedule (no catch-up burst). It sleeps on a high-resolution waitable timer up to ~1 ms before the
// deadline and spins (yielding) for the rest. fps 0 = no cap.
class FrameLimiter {
public:
    FrameLimiter();
    ~FrameLimiter();
    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;
    void set_fps(uint32_t fps);
    uint32_t fps() const { return fps_; }
    void wait();

private:
    uint32_t fps_ = 0;
    int64_t period_ns_ = 0, deadline_ns_ = 0;
    void* timer_ = nullptr;  // HANDLE
};
// Hides / shows the OS arrow over this thread's windows (ShowCursor, kept balanced: only state changes call it). The
// host hides it while the game draws its own cursor (no hardware mouse).
void set_os_cursor_hidden(bool hidden);

}  // namespace alamo::game

// alamo_game host: display modes for the video options dialog (IDD_VIDEO_OPTIONS_DIALOG's IDC_RESOLUTION_LIST) and the
// window resize its Apply / OK performs. Windows only (EnumDisplaySettings / SetWindowPos); kept out of game_main.cpp so
// <windows.h> stays in this one translation unit.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/gui_menus.hpp"
#include "alamo/ui/hotkeys.hpp"

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
// UI5 t04: the Windows clipboard as UTF-32 text for the dialog edit boxes (GuiManager::set_clipboard). Empty when it
// holds no text or cannot be opened.
std::u32string clipboard_text();
void set_clipboard_text(std::u32string_view text);

// ui1 t02 [port] The other options kept between runs (the original keeps them in its registry profile with the video
// options): audio volumes and subtitles (IDD_AUDIO_OPTIONS_DIALOG), the gameplay dialog's values, the port's camera
// settings (FOVs, queue modifier; tactical_fov_set = the player moved the tactical FOV) and the key bindings (the
// HotkeyTable profile: KeySettingsCount + one entry per slot, written under "key." names). A key = value text file,
// %LOCALAPPDATA%/alamo/game_options.txt. Missing keys keep `p`'s values; load is false when the file cannot be read.
struct OptionsProfile {
    ui::AudioVolumes audio;
    ui::GameplaySettings gameplay;
    ui::CameraSettings camera;
    bool tactical_fov_set = false;
    ui::HotkeyProfile keys;  // empty: the bindings were never stored
};
std::string default_options_profile_path();  // "" when %LOCALAPPDATA% is unset
bool load_options_profile(const std::string& path, OptionsProfile& p);
bool save_options_profile(const std::string& path, const OptionsProfile& p);
// ui1 t02 (MAIN_LOOP.md 1.5): the game speed sliders (0..100, default 50) as speed table indices 0..4 (default 2).
uint32_t game_speed_index(int32_t slider);

// [port] The video options kept between runs (the original keeps them in its registry profile, MAIN_LOOP.md §2 step 9;
// the port writes a key = value text file, %LOCALAPPDATA%/alamo/video_options.txt). `mode` 0 x 0 = not stored.
struct VideoProfile {
    DisplayMode mode;
    bool vsync = true, hardware_mouse = false;
    ui::VsyncMode vsync_mode = ui::VsyncMode::Fifo;
    int32_t frame_cap = 0;
    int32_t detail = 100, anti_alias = 0, gamma = 50;  // IDD_VIDEO_OPTIONS_DIALOG values 0..100 (anti_alias: TAA strength, 0 = off)
    ui::AdvancedVideoSettings advanced;
};
// Copies an applied ApplyVideo's settings into the stored profile; false (profile untouched) while `v.unconfirmed` - a
// resolution change waiting on the "keep these settings?" dialog (ui::VideoSettings::unconfirmed): the Yes answer, or the
// revert to the previous mode on No / timeout, comes as a later ApplyVideo with unconfirmed false and stores then. `modes`
// is the list the resolution index refers to.
// Anti-aliasing is the one value p.anti_alias (the Video dialog's list); a profile file from before it was also an Advanced
// dialog box (key "anti_aliasing=taa") loads as Medium strength when its own strength was 0.
bool update_video_profile(VideoProfile& p, const ui::VideoSettings& v, const std::vector<DisplayMode>& modes);
// t43: the stored Detail value is the level of the seven advanced options (ui::advanced_detail_level); applied with them and
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
//   taa_active(strength)        the Anti-aliasing strength is above 0 (the list's Low / Medium / High: 30 / 63 / 100)
//   apply_aa_options(o, strength)  RecordOptions::taa / taa_weight: strength 1..100 lerps the current-frame weight 0.2 -> 0.04
//                                  (smoother, more history; Medium = 0.1, the tuned default); off leaves the weight unset
int shader_level(int32_t slider);
int water_level(int32_t slider);
uint32_t texture_reduction(int32_t slider);
float particle_detail(int32_t slider);
float environment_fade_scale(int32_t slider);
float gamma_value(int32_t slider);
bool taa_active(int32_t strength);
void apply_aa_options(render::RecordOptions& o, int32_t strength);
// The overall Detail Level (IDD_VIDEO_OPTIONS_DIALOG: Low / Medium / High / Ultra, ui::kQualityPresets) as a preset [M]: a
// quality level picked there (VideoSettings::detail_picked) sets the seven advanced detail options (particle, mesh, texture,
// water, shader, shadow, environment) to its value; the checks (soft shadows, heat, bloom), anti-aliasing and anisotropy are
// left as they are.
ui::AdvancedVideoSettings detail_preset(const ui::AdvancedVideoSettings& s, int32_t detail);
// Auto Detect (the Video and Advanced dialogs' Auto Detect button, ui::MenuAction::Kind::AutoDetectVideo) [port: the original's
// detection is not recovered; the choice below is a documented guess from the GPU class and its video memory]. Discrete GPUs
// get Medium under 3 GiB, High under 6 GiB and Ultra from there; an integrated GPU Medium with 2 GiB or more of memory it can
// use, else Low; anything else (software or virtual) Low. Resolution, v-sync, frame cap and gamma are left alone.
enum class GpuKind : uint8_t { Other, Integrated, Discrete };
struct GpuClass {
    GpuKind kind = GpuKind::Other;
    uint64_t memory_bytes = 0;      // largest device-local heap
    bool sampler_anisotropy = false;
    float max_anisotropy = 1.0f;    // the device's limit; the recommended filtering never exceeds it
};
struct AutoVideoChoice {
    size_t quality = 0;             // index into ui::kQualityPresets
    int32_t anti_alias = 0;         // strength, one of ui::kAntiAliasChoices
    ui::AdvancedVideoSettings advanced;  // the seven options at the quality level, plus the checks, AO and anisotropy
};
AutoVideoChoice recommend_video(const GpuClass& gpu);
// Stores the choice in the profile: the advanced options, the Anti-aliasing strength and the Detail value (sync_detail).
void apply_auto_video(VideoProfile& p, const AutoVideoChoice& c);
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

// modc t08: starts this executable again with the command-line arguments `args` (argv[1..] already quoted: the mod
// picker's relaunch with other MODPATH= / STEAMMOD= arguments), in the current working folder. False + err when the
// process could not be created; the caller then keeps running.
bool relaunch_self(const std::string& args, std::string* err = nullptr);

}  // namespace alamo::game

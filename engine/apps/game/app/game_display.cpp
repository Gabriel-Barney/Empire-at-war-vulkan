#include "game_display.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "alamo/render/renderer.hpp"

#include <windows.h>

namespace alamo::game {

namespace {
// The GDI device name of the monitor showing `hwnd` (the primary display when null or unknown).
std::wstring monitor_device(void* hwnd) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    const HMONITOR mon = hwnd ? MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST)
                              : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return {};
    return mi.szDevice;
}
std::wstring changed_device;  // the monitor apply_display_mode switched (restore_display_mode puts it back)
}  // namespace

std::vector<DisplayMode> display_modes(void* hwnd) {
    std::vector<DisplayMode> out;
    const std::wstring dev = monitor_device(hwnd);
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(dev.empty() ? nullptr : dev.c_str(), i, &dm); ++i) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 800 || dm.dmPelsHeight < 600) continue;
        const uint32_t hz = dm.dmDisplayFrequency > 1 ? static_cast<uint32_t>(dm.dmDisplayFrequency) : 0u;
        out.push_back({static_cast<uint32_t>(dm.dmPelsWidth), static_cast<uint32_t>(dm.dmPelsHeight), hz});
    }
    std::sort(out.begin(), out.end(), [](const DisplayMode& a, const DisplayMode& b) {
        return a.width != b.width ? a.width < b.width : a.height != b.height ? a.height < b.height : a.hz < b.hz;
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    collapse_refresh_rates(out);
    return out;
}

void collapse_refresh_rates(std::vector<DisplayMode>& modes) {
    std::vector<DisplayMode> kept;
    kept.reserve(modes.size());
    for (size_t i = 0; i < modes.size(); ++i) {
        const DisplayMode& m = modes[i];
        const bool twin = i + 1 < modes.size() && modes[i + 1].width == m.width && modes[i + 1].height == m.height && modes[i + 1].hz == m.hz + 1;
        if (m.hz == 0 || !twin) kept.push_back(m);
    }
    modes = std::move(kept);
}

std::string display_mode_label(const DisplayMode& m) {
    std::string s = std::to_string(m.width) + " x " + std::to_string(m.height);
    if (m.hz) s += " (" + std::to_string(m.hz) + "Hz)";  // the original's "%d x %d (%dHz)"
    return s;
}

int32_t display_mode_index(const std::vector<DisplayMode>& modes, uint32_t w, uint32_t h, uint32_t hz) {
    int32_t best = -1, same_size = -1;
    for (size_t i = 0; i < modes.size(); ++i) {
        const DisplayMode& m = modes[i];
        if (m.width == w && m.height == h) {
            if (hz && (m.hz == hz || m.hz == hz + 1)) return static_cast<int32_t>(i);  // + 1: collapse_refresh_rates' fold
            same_size = static_cast<int32_t>(i);  // ascending rate: the last one is the highest
        }
        if (m.width <= w && m.height <= h) best = static_cast<int32_t>(i);
    }
    return same_size >= 0 ? same_size : best;
}

bool apply_display_mode(void* hwnd, const DisplayMode& m, std::string* log) {
    HWND wnd = static_cast<HWND>(hwnd);
    if (!wnd || m.width == 0 || m.height == 0) return false;
    const std::wstring dev = monitor_device(hwnd);
    DEVMODEW cur{}, reg{};
    cur.dmSize = reg.dmSize = sizeof cur;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    std::string what;
    if (!dev.empty() && EnumDisplaySettingsW(dev.c_str(), ENUM_CURRENT_SETTINGS, &cur) &&
        EnumDisplaySettingsW(dev.c_str(), ENUM_REGISTRY_SETTINGS, &reg) && GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        // A mode that does not fit the work area (of the user's own desktop resolution) becomes a borderless window over
        // the monitor: the monitor takes the mode's resolution too. A smaller (framed) mode runs on the user's desktop
        // resolution and takes only the rate.
        RECT r{0, 0, static_cast<LONG>(m.width), static_cast<LONG>(m.height)};
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, static_cast<DWORD>(GetWindowLongPtrW(wnd, GWL_EXSTYLE)));
        const LONG bars_w = (mi.rcMonitor.right - mi.rcMonitor.left) - (mi.rcWork.right - mi.rcWork.left);
        const LONG bars_h = (mi.rcMonitor.bottom - mi.rcMonitor.top) - (mi.rcWork.bottom - mi.rcWork.top);
        const bool fits = r.right - r.left <= static_cast<LONG>(reg.dmPelsWidth) - bars_w && r.bottom - r.top <= static_cast<LONG>(reg.dmPelsHeight) - bars_h;
        DEVMODEW want = cur;
        want.dmPelsWidth = reg.dmPelsWidth;
        want.dmPelsHeight = reg.dmPelsHeight;
        want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        if (!fits) {
            want.dmPelsWidth = m.width;
            want.dmPelsHeight = m.height;
        }
        if (m.hz) want.dmDisplayFrequency = m.hz;
        const bool differs = want.dmPelsWidth != cur.dmPelsWidth || want.dmPelsHeight != cur.dmPelsHeight ||
                             want.dmDisplayFrequency != cur.dmDisplayFrequency;
        if (differs) {
            const bool user_mode = want.dmPelsWidth == reg.dmPelsWidth && want.dmPelsHeight == reg.dmPelsHeight &&
                                   want.dmDisplayFrequency == reg.dmDisplayFrequency;
            const LONG test = user_mode ? DISP_CHANGE_SUCCESSFUL : ChangeDisplaySettingsExW(dev.c_str(), &want, nullptr, CDS_FULLSCREEN | CDS_TEST, nullptr);
            const LONG rc = test != DISP_CHANGE_SUCCESSFUL ? test
                            : user_mode ? ChangeDisplaySettingsExW(dev.c_str(), nullptr, nullptr, 0, nullptr)  // back to the user's mode
                                        : ChangeDisplaySettingsExW(dev.c_str(), &want, nullptr, CDS_FULLSCREEN, nullptr);
            if (rc == DISP_CHANGE_SUCCESSFUL) {
                changed_device = user_mode ? std::wstring() : dev;
                what = "display " + std::to_string(want.dmPelsWidth) + "x" + std::to_string(want.dmPelsHeight) + " @ " +
                       std::to_string(want.dmDisplayFrequency) + " Hz";
            } else {
                what = "display mode " + std::to_string(want.dmPelsWidth) + "x" + std::to_string(want.dmPelsHeight) + " @ " +
                       std::to_string(want.dmDisplayFrequency) + " Hz refused (" + std::to_string(rc) + ")";
            }
        } else {
            what = "display unchanged (" + std::to_string(cur.dmDisplayFrequency) + " Hz)";
        }
    }
    const bool ok = set_window_client_size(hwnd, m.width, m.height);
    if (log) *log = "window " + std::to_string(m.width) + "x" + std::to_string(m.height) + (ok ? " applied" : " refused") + (what.empty() ? "" : ", " + what);
    return ok;
}

void restore_display_mode() {
    if (changed_device.empty()) return;
    ChangeDisplaySettingsExW(changed_device.c_str(), nullptr, nullptr, 0, nullptr);
    changed_device.clear();
}

void enable_dpi_awareness() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

bool set_window_client_size(void* hwnd, uint32_t w, uint32_t h) {
    HWND wnd = static_cast<HWND>(hwnd);
    if (!wnd || w == 0 || h == 0) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& mon = mi.rcMonitor;
    const RECT& work = mi.rcWork;
    const auto ex = static_cast<DWORD>(GetWindowLongPtrW(wnd, GWL_EXSTYLE));
    const auto visible = static_cast<DWORD>(GetWindowLongPtrW(wnd, GWL_STYLE)) & WS_VISIBLE;
    constexpr DWORD kFramed = WS_OVERLAPPEDWINDOW;  // the style rhi::Window creates a resizable window with
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    if (!AdjustWindowRectEx(&r, kFramed, FALSE, ex)) return false;
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = 0, y = 0;
    DWORD style = kFramed;
    if (ww <= work.right - work.left && wh <= work.bottom - work.top) {
        // Framed; kept on the work area when the new size would leave it [port].
        RECT cur{};
        GetWindowRect(wnd, &cur);
        x = std::clamp(static_cast<int>(cur.left), static_cast<int>(work.left), static_cast<int>(work.right) - ww);
        y = std::clamp(static_cast<int>(cur.top), static_cast<int>(work.top), static_cast<int>(work.bottom) - wh);
    } else {
        // Borderless on the monitor, centred, never larger than it [port].
        style = WS_POPUP;
        ww = std::min(static_cast<int>(w), static_cast<int>(mon.right - mon.left));
        wh = std::min(static_cast<int>(h), static_cast<int>(mon.bottom - mon.top));
        x = mon.left + (mon.right - mon.left - ww) / 2;
        y = mon.top + (mon.bottom - mon.top - wh) / 2;
    }
    SetWindowLongPtrW(wnd, GWL_STYLE, static_cast<LONG_PTR>(style | visible));
    return SetWindowPos(wnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
}

uint32_t monitor_refresh_hz(void* hwnd) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!hwnd || !GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) return 0;
    return static_cast<uint32_t>(dm.dmDisplayFrequency);
}

namespace {
int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

FrameLimiter::FrameLimiter() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

FrameLimiter::~FrameLimiter() {
    if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
}

void FrameLimiter::set_fps(uint32_t fps) {
    if (fps == fps_) return;
    fps_ = fps;
    period_ns_ = fps ? 1'000'000'000LL / fps : 0;
    deadline_ns_ = 0;  // a new schedule from the next frame
}

void FrameLimiter::wait() {
    if (period_ns_ <= 0) return;
    const int64_t now = now_ns();
    const int64_t target = deadline_ns_ + period_ns_;
    if (deadline_ns_ == 0 || target <= now) {  // first frame, or the deadline was missed: restart from now
        deadline_ns_ = now;
        return;
    }
    constexpr int64_t kSpinNs = 1'000'000;
    if (target - now > kSpinNs) {
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -((target - now - kSpinNs) / 100);  // relative, 100 ns units
            if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
        } else {
            Sleep(static_cast<DWORD>((target - now - kSpinNs) / 1'000'000));
        }
    }
    while (now_ns() < target) std::this_thread::yield();
    deadline_ns_ = target;
}

void set_os_cursor_hidden(bool hidden) {
    static bool is_hidden = false;
    if (hidden == is_hidden) return;
    is_hidden = hidden;
    ShowCursor(hidden ? FALSE : TRUE);
}

// ---- [port] video options file ------------------------------------------------------------------------------------
std::string default_video_profile_path() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la) return {};
    return (std::filesystem::path(la) / "alamo" / "video_options.txt").string();
}

namespace {
struct ProfileInt {
    const char* key;
    int32_t VideoProfile::*field;
};
struct ProfileAdvInt {
    const char* key;
    int32_t ui::AdvancedVideoSettings::*field;
};
struct ProfileAdvBool {
    const char* key;
    bool ui::AdvancedVideoSettings::*field;
};
constexpr ProfileInt kProfileInts[] = {{"frame_cap", &VideoProfile::frame_cap}, {"detail", &VideoProfile::detail},
                                       {"anti_alias", &VideoProfile::anti_alias}, {"gamma", &VideoProfile::gamma}};
constexpr ProfileAdvInt kProfileAdvInts[] = {
    {"particle_detail", &ui::AdvancedVideoSettings::particle}, {"mesh_detail", &ui::AdvancedVideoSettings::mesh},
    {"texture_detail", &ui::AdvancedVideoSettings::texture},   {"water_detail", &ui::AdvancedVideoSettings::water},
    {"shader_detail", &ui::AdvancedVideoSettings::shader},     {"shadow_detail", &ui::AdvancedVideoSettings::shadow},
    {"environment_detail", &ui::AdvancedVideoSettings::environment}, {"anisotropy", &ui::AdvancedVideoSettings::anisotropy}};
constexpr ProfileAdvBool kProfileAdvBools[] = {{"soft_shadows", &ui::AdvancedVideoSettings::soft_shadows},
                                               {"heat_distortion", &ui::AdvancedVideoSettings::heat_distortion},
                                               {"bloom", &ui::AdvancedVideoSettings::bloom},
                                               {"ambient_occlusion", &ui::AdvancedVideoSettings::ambient_occlusion}};
}  // namespace

bool update_video_profile(VideoProfile& p, const ui::VideoSettings& v, const std::vector<DisplayMode>& modes) {
    if (v.unconfirmed) return false;
    if (v.resolution >= 0 && static_cast<size_t>(v.resolution) < modes.size()) p.mode = modes[static_cast<size_t>(v.resolution)];
    p.vsync = v.vsync;
    p.vsync_mode = v.vsync_mode;
    p.frame_cap = v.frame_cap;
    p.hardware_mouse = v.hardware_mouse;
    p.detail = v.detail;
    p.anti_alias = v.anti_alias;
    p.gamma = v.gamma;
    return true;
}

void sync_detail(VideoProfile& p) { p.detail = ui::advanced_detail_level(p.advanced); }

bool load_video_profile(const std::string& path, VideoProfile& p) {
    std::ifstream f(path);
    if (path.empty() || !f) return false;
    std::map<std::string, std::string> kv;
    for (std::string line; std::getline(f, line);) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        auto trim = [](std::string t) {
            while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\t')) t.pop_back();
            size_t b = 0;
            while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
            return t.substr(b);
        };
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    auto num = [&](const char* k, int32_t& o) {
        if (auto it = kv.find(k); it != kv.end() && !it->second.empty()) o = static_cast<int32_t>(std::strtol(it->second.c_str(), nullptr, 10));
    };
    auto flag = [&](const char* k, bool& o) {
        if (auto it = kv.find(k); it != kv.end()) o = it->second == "1" || it->second == "true" || it->second == "on";
    };
    int32_t w = static_cast<int32_t>(p.mode.width), h = static_cast<int32_t>(p.mode.height), hz = static_cast<int32_t>(p.mode.hz);
    num("width", w);
    num("height", h);
    num("refresh_hz", hz);
    p.mode = {static_cast<uint32_t>(std::max(0, w)), static_cast<uint32_t>(std::max(0, h)), static_cast<uint32_t>(std::max(0, hz))};
    flag("vsync", p.vsync);
    flag("hardware_mouse", p.hardware_mouse);
    if (auto it = kv.find("vsync_mode"); it != kv.end()) p.vsync_mode = it->second == "mailbox" ? ui::VsyncMode::Mailbox : ui::VsyncMode::Fifo;
    for (const ProfileInt& e : kProfileInts) num(e.key, p.*e.field);
    for (const ProfileAdvInt& e : kProfileAdvInts) num(e.key, p.advanced.*e.field);
    for (const ProfileAdvBool& e : kProfileAdvBools) flag(e.key, p.advanced.*e.field);
    // A profile from before anti-aliasing was one setting also has "anti_aliasing=taa|off" (the Advanced dialog's box) next to the
    // strength (the Video dialog's slider): either one on meant TAA. A box on over strength 0 is Medium, the old default.
    if (auto it = kv.find("anti_aliasing"); it != kv.end() && it->second == "taa" && p.anti_alias <= 0) p.anti_alias = ui::kAntiAliasDefaultSlider;
    return true;
}

bool save_video_profile(const std::string& path, const VideoProfile& p) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# alamo_game video options (written by the Video / Advanced video dialogs)\n";
    f << "width=" << p.mode.width << "\nheight=" << p.mode.height << "\nrefresh_hz=" << p.mode.hz << "\n";
    f << "vsync=" << (p.vsync ? 1 : 0) << "\nvsync_mode=" << (p.vsync_mode == ui::VsyncMode::Mailbox ? "mailbox" : "fifo") << "\n";
    f << "hardware_mouse=" << (p.hardware_mouse ? 1 : 0) << "\n";
    for (const ProfileInt& e : kProfileInts) f << e.key << "=" << p.*e.field << "\n";
    for (const ProfileAdvInt& e : kProfileAdvInts) f << e.key << "=" << p.advanced.*e.field << "\n";
    for (const ProfileAdvBool& e : kProfileAdvBools) f << e.key << "=" << (p.advanced.*e.field ? 1 : 0) << "\n";
    return static_cast<bool>(f);
}

namespace {
int32_t slider_clamp(int32_t v) { return std::clamp(v, 0, 100); }
}  // namespace

int shader_level(int32_t slider) { return (slider_clamp(slider) * 3 + 50) / 100; }
int water_level(int32_t slider) { return (slider_clamp(slider) * 2 + 50) / 100; }
uint32_t texture_reduction(int32_t slider) { return static_cast<uint32_t>(2 - (slider_clamp(slider) * 2 + 50) / 100); }
float particle_detail(int32_t slider) { return static_cast<float>(slider_clamp(slider)) / 100.0f; }
float environment_fade_scale(int32_t slider) { return 0.6f + 0.4f * static_cast<float>(slider_clamp(slider)) / 100.0f; }
float gamma_value(int32_t slider) { return std::exp2((static_cast<float>(slider_clamp(slider)) - 50.0f) / 50.0f); }

bool taa_active(int32_t strength) { return strength > 0; }

void apply_aa_options(render::RecordOptions& o, int32_t strength) {
    o.taa = taa_active(strength);
    // Off keeps the renderer's tuned default weight for a later switch-on. On: the share of the current frame falls with the
    // strength (more history = smoother edges, a little more softness / ghosting).
    o.taa_weight = o.taa ? 0.2f - 0.16f * static_cast<float>(slider_clamp(strength)) / 100.0f : -1.0f;
}

ui::AdvancedVideoSettings detail_preset(const ui::AdvancedVideoSettings& s, int32_t detail) {
    ui::AdvancedVideoSettings r = s;
    ui::set_advanced_detail(r, slider_clamp(detail));
    return r;
}

void apply_detail_options(render::RecordOptions& o, const ui::AdvancedVideoSettings& s) {
    o.lod_detail = static_cast<float>(std::clamp(s.mesh, 0, 100)) / 100.0f;
    if (s.shadow <= 0) o.shadows = false;
    o.soft_shadows = s.soft_shadows && o.shadows;  // the checkbox turns them on as well as off (playtest 3)
    o.shader_detail = shader_level(s.shader);
    o.water_detail = water_level(s.water);
    if (!s.bloom) o.bloom_strength = 0.0f;
    if (!s.heat_distortion) o.heat_amount = 0.0f;
}

}  // namespace alamo::game

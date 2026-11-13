#include "game_display.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
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

std::u32string clipboard_text() {
    std::u32string out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            for (size_t k = 0; w[k] != 0; ++k) {
                char32_t c = static_cast<char16_t>(w[k]);
                if (c >= 0xD800 && c < 0xDC00 && w[k + 1] >= 0xDC00 && w[k + 1] < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char16_t>(w[k + 1]) - 0xDC00u);
                    ++k;
                }
                out.push_back(c);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void set_clipboard_text(std::u32string_view text) {
    std::u16string w;
    for (char32_t c : text) {
        if (c >= 0x10000) {
            w.push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
            w.push_back(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            w.push_back(static_cast<char16_t>(c));
        }
    }
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(char16_t))) {
        if (auto* p = static_cast<char16_t*>(GlobalLock(h))) {
            std::copy(w.begin(), w.end(), p);
            p[w.size()] = 0;
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

std::vector<DisplayMode> video_list_modes(const std::vector<DisplayMode>& modes, bool widescreen) {
    if (widescreen) return modes;
    std::vector<DisplayMode> out;
    for (const DisplayMode& m : modes)
        if (static_cast<uint64_t>(m.width) * 3u <= static_cast<uint64_t>(m.height) * 4u) out.push_back(m);
    return out.empty() ? modes : out;
}

bool apply_display_mode(void* hwnd, const DisplayMode& m, std::string* log, bool windowed) {
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
        if (!fits || !windowed) {  // UI5 t01: full screen always takes the mode's resolution
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
    const bool ok = set_window_client_size(hwnd, m.width, m.height, !windowed);
    if (log) *log = std::string(windowed ? "window " : "full screen ") + std::to_string(m.width) + "x" + std::to_string(m.height) +
                    (ok ? " applied" : " refused") + (what.empty() ? "" : ", " + what);
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

bool set_window_client_size(void* hwnd, uint32_t w, uint32_t h, bool borderless) {
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
    if (!borderless && ww <= work.right - work.left && wh <= work.bottom - work.top) {
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

void* create_os_cursor(const uint8_t* bgra, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y) {
    if (!bgra || width == 0 || height == 0 || width > 256 || height > 256) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = static_cast<LONG>(width);
    bi.bV5Height = -static_cast<LONG>(height);  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    std::memcpy(bits, bgra, static_cast<size_t>(width) * height * 4u);
    HBITMAP mask = CreateBitmap(static_cast<int>(width), static_cast<int>(height), 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = FALSE;
    ii.xHotspot = static_cast<DWORD>(std::clamp<int32_t>(hot_x, 0, static_cast<int32_t>(width) - 1));
    ii.yHotspot = static_cast<DWORD>(std::clamp<int32_t>(hot_y, 0, static_cast<int32_t>(height) - 1));
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON cur = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return cur;
}

void set_os_cursor_image(void* hwnd, void* cursor) {
    static void* last = nullptr;
    static bool set = false;
    if (set && cursor == last) return;
    set = true;
    last = cursor;
    HCURSOR h = cursor ? static_cast<HCURSOR>(cursor) : LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    if (hwnd) SetClassLongPtrW(static_cast<HWND>(hwnd), GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(h));
    SetCursor(h);
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
    p.windowed = v.windowed;
    p.widescreen = v.widescreen;
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
    flag("windowed", p.windowed);
    flag("widescreen", p.widescreen);
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
    f << "windowed=" << (p.windowed ? 1 : 0) << "\nwidescreen=" << (p.widescreen ? 1 : 0) << "\n";
    for (const ProfileInt& e : kProfileInts) f << e.key << "=" << p.*e.field << "\n";
    for (const ProfileAdvInt& e : kProfileAdvInts) f << e.key << "=" << p.advanced.*e.field << "\n";
    for (const ProfileAdvBool& e : kProfileAdvBools) f << e.key << "=" << (p.advanced.*e.field ? 1 : 0) << "\n";
    return static_cast<bool>(f);
}

// ---- ui1 t02 [port] other options file --------------------------------------------------------------------------
std::string default_options_profile_path() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la) return {};
    return (std::filesystem::path(la) / "alamo" / "game_options.txt").string();
}

namespace {
struct OptInt {
    const char* key;
    int32_t ui::GameplaySettings::*field;
};
struct OptBool {
    const char* key;
    bool ui::GameplaySettings::*field;
};
struct OptVolume {
    const char* key;
    float ui::AudioVolumes::*field;
};
constexpr OptInt kOptInts[] = {{"game_speed", &ui::GameplaySettings::game_speed},
                               {"strategic_speed", &ui::GameplaySettings::strategic_speed},
                               {"scroll_speed", &ui::GameplaySettings::scroll_speed},
                               {"difficulty", &ui::GameplaySettings::difficulty}};
constexpr OptBool kOptBools[] = {{"ask_autoresolve", &ui::GameplaySettings::ask_autoresolve},
                                 {"delay_tooltips", &ui::GameplaySettings::delay_tooltips},
                                 {"absolute_scroll", &ui::GameplaySettings::absolute_scroll},
                                 {"alternate_mouse", &ui::GameplaySettings::alternate_mouse},
                                 {"right_button_scroll", &ui::GameplaySettings::right_button_scroll},
                                 {"both_buttons_rotate", &ui::GameplaySettings::both_buttons_rotate},
                                 {"simplified_colors", &ui::GameplaySettings::simplified_colors},
                                 {"abilities_autofire", &ui::GameplaySettings::abilities_autofire}};
constexpr OptVolume kOptVolumes[] = {{"volume_master", &ui::AudioVolumes::master},
                                     {"volume_sfx", &ui::AudioVolumes::sfx},
                                     {"volume_speech", &ui::AudioVolumes::speech},
                                     {"volume_music", &ui::AudioVolumes::music}};
constexpr std::string_view kKeyPrefix = "key.";

std::string opt_float(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", static_cast<double>(v));
    return buf;
}
}  // namespace

bool load_options_profile(const std::string& path, OptionsProfile& p) {
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
    auto find = [&](const char* k) -> const std::string* {
        const auto it = kv.find(k);
        return it != kv.end() && !it->second.empty() ? &it->second : nullptr;
    };
    auto flag = [](const std::string& v) { return v == "1" || v == "true" || v == "on"; };
    for (const OptInt& e : kOptInts)
        if (const std::string* v = find(e.key)) p.gameplay.*e.field = static_cast<int32_t>(std::strtol(v->c_str(), nullptr, 10));
    p.gameplay.difficulty = std::clamp(p.gameplay.difficulty, 0, 2);
    for (const OptBool& e : kOptBools)
        if (const std::string* v = find(e.key)) p.gameplay.*e.field = flag(*v);
    for (const OptVolume& e : kOptVolumes)
        if (const std::string* v = find(e.key)) p.audio.*e.field = std::clamp(std::strtof(v->c_str(), nullptr), 0.0f, 1.0f);
    if (const std::string* v = find("subtitles")) p.audio.subtitles = flag(*v);
    if (const std::string* v = find("galactic_fov"))
        p.camera.galactic_fov_deg = std::clamp(std::strtof(v->c_str(), nullptr), ui::kGalacticFovMinDeg, ui::kGalacticFovMaxDeg);
    if (const std::string* v = find("tactical_fov"))
        p.camera.tactical_fov_deg = std::clamp(std::strtof(v->c_str(), nullptr), ui::kTacticalFovMinDeg, ui::kTacticalFovMaxDeg);
    if (const std::string* v = find("tactical_fov_set")) p.tactical_fov_set = flag(*v);
    if (const std::string* v = find("queue_mods")) p.camera.queue_mods = static_cast<uint8_t>(std::strtoul(v->c_str(), nullptr, 10) & 0xFFu);
    for (const auto& [k, v] : kv)
        if (k.size() > kKeyPrefix.size() && std::string_view(k).substr(0, kKeyPrefix.size()) == kKeyPrefix && !v.empty())
            p.keys[k.substr(kKeyPrefix.size())] = static_cast<int32_t>(std::strtol(v.c_str(), nullptr, 10));
    return true;
}

bool save_options_profile(const std::string& path, const OptionsProfile& p) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# alamo_game options (audio, gameplay, camera and controls, keyboard dialogs)\n";
    for (const OptInt& e : kOptInts) f << e.key << "=" << p.gameplay.*e.field << "\n";
    for (const OptBool& e : kOptBools) f << e.key << "=" << (p.gameplay.*e.field ? 1 : 0) << "\n";
    for (const OptVolume& e : kOptVolumes) f << e.key << "=" << opt_float(p.audio.*e.field) << "\n";
    f << "subtitles=" << (p.audio.subtitles ? 1 : 0) << "\n";
    f << "galactic_fov=" << opt_float(p.camera.galactic_fov_deg) << "\n";
    f << "tactical_fov=" << opt_float(p.camera.tactical_fov_deg) << "\n";
    f << "tactical_fov_set=" << (p.tactical_fov_set ? 1 : 0) << "\n";
    f << "queue_mods=" << static_cast<unsigned>(p.camera.queue_mods) << "\n";
    for (const auto& [k, v] : p.keys) f << kKeyPrefix << k << "=" << v << "\n";
    return static_cast<bool>(f);
}

uint32_t game_speed_index(int32_t slider) {
    // The nearest of the five positions 0, 25, 50, 75, 100.
    return static_cast<uint32_t>((std::clamp(slider, 0, 100) + 12) / 25);
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

AutoVideoChoice recommend_video(const GpuClass& gpu) {
    constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    AutoVideoChoice c;
    int32_t aniso = 0;
    bool soft_shadows = false, ao = false;
    switch (gpu.kind) {
        case GpuKind::Discrete:
            if (gpu.memory_bytes >= 6 * kGiB) { c.quality = 3; c.anti_alias = ui::kAntiAliasChoices[3].value; aniso = 16; soft_shadows = true; ao = true; }
            else if (gpu.memory_bytes >= 3 * kGiB) { c.quality = 2; c.anti_alias = ui::kAntiAliasChoices[2].value; aniso = 8; soft_shadows = true; }
            else { c.quality = 1; c.anti_alias = ui::kAntiAliasChoices[1].value; aniso = 4; }
            break;
        case GpuKind::Integrated:
            c.quality = gpu.memory_bytes >= 2 * kGiB ? 1 : 0;
            c.anti_alias = 0;
            aniso = 2;
            break;
        case GpuKind::Other: break;  // Low, everything off
    }
    // Filtering the device cannot do is not recommended: the largest listed level at or under its limit.
    if (!gpu.sampler_anisotropy) aniso = 0;
    while (aniso > 0 && static_cast<float>(aniso) > gpu.max_anisotropy) aniso /= 2;
    if (aniso < 2) aniso = 0;
    ui::set_advanced_detail(c.advanced, ui::kQualityPresets[c.quality]);
    c.advanced.soft_shadows = soft_shadows;
    c.advanced.heat_distortion = true;
    c.advanced.bloom = true;
    c.advanced.ambient_occlusion = ao;
    c.advanced.anisotropy = aniso;
    return c;
}

void apply_auto_video(VideoProfile& p, const AutoVideoChoice& c) {
    p.advanced = c.advanced;
    p.anti_alias = c.anti_alias;
    sync_detail(p);
}

// modc t08: CreateProcessW on GetModuleFileNameW; `args` is already quoted (mod_picker.hpp windows_command_line). The
// arguments came from main's narrow argv / the file system's narrow paths: the ANSI code page.
bool relaunch_self(const std::string& args, std::string* err) {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) {
        if (err) *err = "GetModuleFileNameW failed";
        return false;
    }
    exe.resize(n);
    std::string narrow_exe(n * 4 + 1, '\0');
    const int en = WideCharToMultiByte(CP_ACP, 0, exe.c_str(), static_cast<int>(n), narrow_exe.data(), static_cast<int>(narrow_exe.size()),
                                       nullptr, nullptr);
    narrow_exe.resize(static_cast<size_t>(std::max(en, 0)));
    const std::string line = "\"" + narrow_exe + "\"" + (args.empty() ? std::string() : " " + args);
    const int wn = MultiByteToWideChar(CP_ACP, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
    std::wstring wline(static_cast<size_t>(std::max(wn, 0)) + 1, L'\0');
    if (wn > 0) MultiByteToWideChar(CP_ACP, 0, line.c_str(), static_cast<int>(line.size()), wline.data(), wn);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), wline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        if (err) *err = "CreateProcessW failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace alamo::game

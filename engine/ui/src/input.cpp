#include "alamo/ui/input.hpp"

#include <bit>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace alamo::ui {

void InputState::update(const InputFrame& f) {
    const InputFrame prev = have_prev_ ? cur_ : f;
    const bool first = !have_prev_;
    cur_ = f;
    have_prev_ = true;
    ++frames_;
    mdx_ = first ? 0 : f.mouse_x - prev.mouse_x;
    mdy_ = first ? 0 : f.mouse_y - prev.mouse_y;

    for (size_t i = 0; i < 4; ++i) {
        const uint64_t was = first ? 0 : prev.keys_down.w[i];
        pressed_.w[i] = f.keys_pressed.w[i] | (f.keys_down.w[i] & ~was);
        released_.w[i] = was & ~f.keys_down.w[i];
    }

    const int64_t thr2 = static_cast<int64_t>(cfg_.drag_threshold_px * cfg_.drag_threshold_px);
    for (size_t i = 0; i < kButtonCount; ++i) {
        ButtonState& b = buttons_[i];
        const auto bit = static_cast<uint8_t>(1u << i);
        const bool was_down = !first && (prev.buttons_down & bit) != 0;
        const bool now_down = (f.buttons_down & bit) != 0;
        b.pressed = (f.buttons_pressed & bit) != 0 || (now_down && !was_down);
        b.released = (f.buttons_released & bit) != 0 || (was_down && !now_down);
        b.click = b.double_click = b.drag_ended = b.double_press = false;
        b.dx = b.dy = 0;
        if (b.pressed) {  // a tap inside one frame is pressed + released: start here, click below
            b.dragging = false;
            b.drag = {f.mouse_x, f.mouse_y, f.mouse_x, f.mouse_y};
            // Win32 rule: same button as the previous press, within the time and the rectangle centred on that press.
            const int32_t sx = f.mouse_x - b.last_press_x, sy = f.mouse_y - b.last_press_y;
            const int32_t hw = cfg_.double_click_rect_w / 2, hh = cfg_.double_click_rect_h / 2;
            const bool inside = sx >= -hw && sx <= hw && sy >= -hh && sy <= hh;
            b.double_press = last_press_button_ == static_cast<int32_t>(i) && !b.last_press_was_double && inside &&
                             f.time - b.last_press_time <= cfg_.double_click_time;
            b.in_double_press = b.double_press;
            b.last_press_was_double = b.double_press;
            b.last_press_time = f.time;
            b.last_press_x = f.mouse_x;
            b.last_press_y = f.mouse_y;
            last_press_button_ = static_cast<int32_t>(i);
        }
        if (was_down || b.pressed) {
            b.dx = was_down ? mdx_ : 0;
            b.dy = was_down ? mdy_ : 0;
            b.drag.x1 = f.mouse_x;
            b.drag.y1 = f.mouse_y;
            const int64_t ddx = b.drag.x1 - b.drag.x0, ddy = b.drag.y1 - b.drag.y0;
            if (!b.dragging && ddx * ddx + ddy * ddy > thr2) b.dragging = true;
        }
        if (b.released) {
            if (b.dragging) {
                b.drag_ended = true;
            } else {
                b.click = true;
                const int32_t sx = f.mouse_x - b.last_click_x, sy = f.mouse_y - b.last_click_y;
                const int32_t slop = cfg_.double_click_slop_px;
                const bool close_by = sx <= slop && sx >= -slop && sy <= slop && sy >= -slop;
                if (!b.last_click_was_double && close_by && f.time - b.last_click_time <= cfg_.double_click_time) {
                    b.double_click = true;
                    b.last_click_was_double = true;  // a third click starts a new pair
                } else {
                    b.last_click_was_double = false;
                }
                b.last_click_time = f.time;
                b.last_click_x = f.mouse_x;
                b.last_click_y = f.mouse_y;
            }
            b.dragging = false;
        }
        b.down = now_down;
    }
}

InputConfig system_input_config(InputConfig base) {
#ifdef _WIN32
    base.double_click_time = static_cast<double>(GetDoubleClickTime()) / 1000.0;
    base.double_click_rect_w = GetSystemMetrics(SM_CXDOUBLECLK);
    base.double_click_rect_h = GetSystemMetrics(SM_CYDOUBLECLK);
#endif
    return base;
}

// ---- actions -----------------------------------------------------------------------------------------
std::string_view action_name(Action a) {
    static constexpr std::string_view kNames[kActionCount] = {
        "ScrollLeft",    "ScrollRight",   "ScrollUp",      "ScrollDown",    "RotateLeft",    "RotateRight",
        "ZoomIn",        "ZoomOut",       "ResetCamera",   "SelectAll",     "Stop",          "ControlGroup0",
        "ControlGroup1", "ControlGroup2", "ControlGroup3", "ControlGroup4", "ControlGroup5", "ControlGroup6",
        "ControlGroup7", "ControlGroup8", "ControlGroup9", "PitchUp",       "PitchDown",     "RollLeft",
        "RollRight",     "Pause"};
    const auto i = static_cast<size_t>(a);
    return i < kActionCount ? kNames[i] : std::string_view("Unknown");
}

uint8_t current_mods(const InputState& in) {
    uint8_t m = kModNone;
    if (in.shift()) m |= kModShift;
    if (in.ctrl()) m |= kModCtrl;
    if (in.alt()) m |= kModAlt;
    return m;
}

Bindings::Bindings() {
    // The defaults are the default hotkey table (UI.md §4.3, hotkeys.hpp) as HotkeyTable::apply_to_bindings maps it, so a
    // host that never loads a profile gets the same keys:
    // [port: Gabriel] W A S D scroll with any modifier (nothing else uses them; the original scrolls on the arrows).
    bind(Action::ScrollLeft, 0, {vk::key('A'), kModAny});
    bind(Action::ScrollRight, 0, {vk::key('D'), kModAny});
    bind(Action::ScrollUp, 0, {vk::key('W'), kModAny});
    bind(Action::ScrollDown, 0, {vk::key('S'), kModAny});
    // [port: Gabriel] free camera axes on the arrows / Numpad 4 6, zoom on = / - (the original has no keyboard rotate or
    // zoom), pause on Pause.
    bind(Action::RotateLeft, 0, {vk::kLeft, kModNone});
    bind(Action::RotateRight, 0, {vk::kRight, kModNone});
    bind(Action::PitchUp, 0, {vk::kUp, kModNone});
    bind(Action::PitchDown, 0, {vk::kDown, kModNone});
    bind(Action::RollLeft, 0, {vk::kNumpad4, kModNone});
    bind(Action::RollRight, 0, {vk::kNumpad6, kModNone});
    bind(Action::ZoomIn, 0, {vk::kOemPlus, kModNone});
    bind(Action::ZoomOut, 0, {vk::kOemMinus, kModNone});
    bind(Action::Pause, 0, {vk::kPause, kModNone});
    bind(Action::ResetCamera, 0, {vk::kEnd, kModAny});  // slot 73
    bind(Action::SelectAll, 0, {vk::key('Y'), kModCtrl});  // [port: Gabriel] Ctrl+A -> Ctrl+Y (A moves the camera)
    bind(Action::Stop, 0, {vk::key('H'), kModNone});  // [port: Gabriel] S scrolls
    for (uint16_t g = 0; g < 10; ++g)
        bind(static_cast<Action>(static_cast<uint8_t>(Action::ControlGroup0) + g), 0,
             {static_cast<uint16_t>(vk::k0 + g), kModAny});
}

namespace {
// Hotkey modifier rule (P1_CAMERA_INPUT.md §6, `0x2127f0`): an unmodified binding matches only with Shift, Ctrl and Alt
// all up; a modified one when its modifier(s) are held, whatever else is. kModAny [port: Gabriel] matches always.
bool mods_match(uint8_t want, uint8_t have) {
    if ((want & kModAny) != 0) return true;
    if (want == kModNone) return have == kModNone;
    return (have & want) == want;
}
}  // namespace

bool Bindings::held(const InputState& in, Action a) const {
    const uint8_t m = current_mods(in);
    for (const KeyBinding& b : map_[static_cast<size_t>(a)])
        if (b.key != 0 && in.key_down(b.key) && mods_match(b.mods, m)) return true;
    return false;
}

bool Bindings::queue_held(const InputState& in) const {
    return queue_mods_ != 0 && (current_mods(in) & queue_mods_) == queue_mods_;
}

bool Bindings::pressed(const InputState& in, Action a) const {
    const uint8_t m = current_mods(in);
    for (const KeyBinding& b : map_[static_cast<size_t>(a)])
        if (b.key != 0 && in.key_pressed(b.key) && mods_match(b.mods, m)) return true;
    return false;
}

// ---- replay ------------------------------------------------------------------------------------------
namespace {
void put_u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
struct Reader {
    std::span<const uint8_t> b;
    size_t p = 0;
    bool ok = true;
    uint64_t un(int n) {
        if (p + static_cast<size_t>(n) > b.size()) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(b[p + static_cast<size_t>(i)]) << (8 * i);
        p += static_cast<size_t>(n);
        return v;
    }
    uint8_t u8() { return static_cast<uint8_t>(un(1)); }
    uint32_t u32() { return static_cast<uint32_t>(un(4)); }
    uint64_t u64() { return un(8); }
};
}  // namespace

std::vector<uint8_t> InputReplay::serialize() const {
    std::vector<uint8_t> o;
    o.reserve(12 + frames.size() * kReplayFrameBytes);
    o.insert(o.end(), {'A', 'U', 'I', 'R'});
    put_u32(o, kReplayVersion);
    put_u32(o, static_cast<uint32_t>(frames.size()));
    for (const InputFrame& f : frames) {
        put_u64(o, std::bit_cast<uint64_t>(f.time));
        put_u32(o, std::bit_cast<uint32_t>(f.dt));
        put_u32(o, f.width);
        put_u32(o, f.height);
        put_u32(o, static_cast<uint32_t>(f.mouse_x));
        put_u32(o, static_cast<uint32_t>(f.mouse_y));
        put_u8(o, f.mouse_inside ? 1 : 0);
        put_u8(o, f.buttons_down);
        put_u8(o, f.buttons_pressed);
        put_u8(o, f.buttons_released);
        put_u32(o, std::bit_cast<uint32_t>(f.wheel));
        for (uint64_t w : f.keys_down.w) put_u64(o, w);
        for (uint64_t w : f.keys_pressed.w) put_u64(o, w);
    }
    return o;
}

bool InputReplay::deserialize(std::span<const uint8_t> bytes, InputReplay& out) {
    out.frames.clear();
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "AUIR", 4) != 0) return false;
    Reader r{bytes, 4};
    if (r.u32() != kReplayVersion) return false;
    const uint32_t n = r.u32();
    if ((bytes.size() - 12) != static_cast<size_t>(n) * kReplayFrameBytes) return false;
    out.frames.resize(n);
    for (InputFrame& f : out.frames) {
        f.time = std::bit_cast<double>(r.u64());
        f.dt = std::bit_cast<float>(r.u32());
        f.width = r.u32();
        f.height = r.u32();
        f.mouse_x = static_cast<int32_t>(r.u32());
        f.mouse_y = static_cast<int32_t>(r.u32());
        f.mouse_inside = r.u8() != 0;
        f.buttons_down = r.u8();
        f.buttons_pressed = r.u8();
        f.buttons_released = r.u8();
        f.wheel = std::bit_cast<float>(r.u32());
        for (uint64_t& w : f.keys_down.w) w = r.u64();
        for (uint64_t& w : f.keys_pressed.w) w = r.u64();
    }
    if (!r.ok || r.p != bytes.size()) {
        out.frames.clear();
        return false;
    }
    return true;
}

}  // namespace alamo::ui

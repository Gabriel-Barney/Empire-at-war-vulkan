// P1 input layer: one raw snapshot per main-loop iteration (InputFrame), a per-frame state machine on
// top of it (InputState: held / pressed / released, clicks, double clicks, drags, wheel) and rebindable
// actions (Bindings).
//
// MAIN_LOOP.md step 3: window messages are pumped once per loop iteration, BEFORE any tick, so the host
// calls capture (window_input.hpp, or an InputReplay frame) once right after rhi::Window::pump(), then
// InputState::update(), then the camera controllers and the OrderTranslator. Nothing here touches the
// sim: orders leave as commands::EventData (orders.hpp).
//
// Keys are Win32 virtual-key codes (0..255) because that is what rhi::Window reports; the replay format
// stores them as such on every platform.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace alamo::ui {

// Win32 virtual-key codes used by the default bindings (platform independent constants).
namespace vk {
inline constexpr uint16_t kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kShift = 0x10, kControl = 0x11, kAlt = 0x12,
                          kEscape = 0x1B, kSpace = 0x20, kPageUp = 0x21, kPageDown = 0x22, kEnd = 0x23, kHome = 0x24,
                          kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28, kInsert = 0x2D, kDelete = 0x2E,
                          k0 = 0x30, kA = 0x41, kNumpadAdd = 0x6B, kNumpadSub = 0x6D, kPause = 0x13, kNumpad5 = 0x65,
                          kNumpad4 = 0x64, kNumpad6 = 0x66, kNumpad8 = 0x68, kOemPlus = 0xBB /* = */,
                          kOemMinus = 0xBD;
inline constexpr uint16_t key(char c) { return static_cast<uint16_t>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c); }
}  // namespace vk

enum class Button : uint8_t { Left = 0, Right, Middle, X1, X2, Count };
inline constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);
inline constexpr uint8_t button_bit(Button b) { return static_cast<uint8_t>(1u << static_cast<unsigned>(b)); }

struct KeySet {  // 256-bit set of virtual keys
    std::array<uint64_t, 4> w{};
    bool test(uint32_t k) const { return k < 256 && ((w[k >> 6] >> (k & 63)) & 1u) != 0; }
    void set(uint32_t k, bool v = true) {
        if (k >= 256) return;
        const uint64_t m = uint64_t{1} << (k & 63);
        w[k >> 6] = v ? (w[k >> 6] | m) : (w[k >> 6] & ~m);
    }
    friend bool operator==(const KeySet&, const KeySet&) = default;
};

// Raw snapshot of one loop iteration. `keys_pressed` / `buttons_pressed` / `buttons_released` are the
// edges seen while pumping (a tap shorter than one frame still counts); InputState also derives edges
// from the down-state change, so a source that only has down states works too.
struct InputFrame {
    double time = 0.0;          // wall clock, seconds (double click timing)
    float dt = 0.0f;            // wall seconds since the previous frame (camera motion)
    uint32_t width = 1, height = 1;  // client area in pixels
    int32_t mouse_x = 0, mouse_y = 0;  // client pixels; may be outside while a button is held (capture)
    bool mouse_inside = true;   // false: no edge scrolling (cursor left the window without capture)
    uint8_t buttons_down = 0;   // button_bit() set
    uint8_t buttons_pressed = 0;
    uint8_t buttons_released = 0;
    float wheel = 0.0f;         // notches, + = away from the user
    KeySet keys_down;
    KeySet keys_pressed;
    friend bool operator==(const InputFrame&, const InputFrame&) = default;
};

// Thresholds. MinimumDragDistance (GameConstants, 4.0) is taken as pixels [port: unit not in the docs];
// the double-click window is the Windows default 500 ms / 4 px [port].
// Press-time double clicks (ButtonState::double_press) follow the Win32 WM_xBUTTONDBLCLK rule (P1_DOUBLE_CLICK_MOVE.md
// §1.1): the second press of the same button within double_click_time of the first press and inside a
// double_click_rect_w x double_click_rect_h rectangle centred on it (SM_CXDOUBLECLK / SM_CYDOUBLECLK, default 4 x 4).
struct InputConfig {
    float drag_threshold_px = 4.0f;
    double double_click_time = 0.5;
    int32_t double_click_slop_px = 4;
    int32_t double_click_rect_w = 4, double_click_rect_h = 4;
};
// The user's Windows settings (GetDoubleClickTime, SM_CXDOUBLECLK / SM_CYDOUBLECLK) on top of `base`, for exact parity
// with the original in live play. Not for replays / smoke runs (machine dependent). Elsewhere returns `base`.
InputConfig system_input_config(InputConfig base = {});

struct DragRect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // start, current (not normalised)
    int32_t min_x() const { return x0 < x1 ? x0 : x1; }
    int32_t max_x() const { return x0 < x1 ? x1 : x0; }
    int32_t min_y() const { return y0 < y1 ? y0 : y1; }
    int32_t max_y() const { return y0 < y1 ? y1 : y0; }
};

struct ButtonState {
    bool down = false;
    bool pressed = false;     // went down this frame
    bool released = false;   // went up this frame
    bool click = false;       // released this frame without having become a drag
    bool double_click = false;  // this click is the second within double_click_time / slop
    bool dragging = false;    // held and moved beyond drag_threshold_px since the press
    bool drag_ended = false;  // released this frame after dragging (drag holds the final rect)
    DragRect drag;            // press position -> current position (valid while down / on release)
    int32_t dx = 0, dy = 0;   // mouse motion this frame while held
    // Win32 double click (WM_xBUTTONDBLCLK): this press is the second of a pair (press-time rule, InputConfig). A third
    // press starts a new pair. in_double_press stays set while that press is held and on its release (the release the
    // original swallows: P1_DOUBLE_CLICK_MOVE.md §1.1), cleared by the next press.
    bool double_press = false;
    bool in_double_press = false;
    // internal
    double last_click_time = -1e30;
    int32_t last_click_x = 0, last_click_y = 0;
    bool last_click_was_double = false;
    double last_press_time = -1e30;
    int32_t last_press_x = 0, last_press_y = 0;
    bool last_press_was_double = false;
};

class InputState {
public:
    explicit InputState(InputConfig cfg = {}) : cfg_(cfg) {}
    void update(const InputFrame& f);

    const InputFrame& frame() const { return cur_; }
    const InputConfig& config() const { return cfg_; }
    bool key_down(uint32_t k) const { return cur_.keys_down.test(k); }
    bool key_pressed(uint32_t k) const { return pressed_.test(k); }
    bool key_released(uint32_t k) const { return released_.test(k); }
    const ButtonState& button(Button b) const { return buttons_[static_cast<size_t>(b)]; }
    int32_t mouse_x() const { return cur_.mouse_x; }
    int32_t mouse_y() const { return cur_.mouse_y; }
    int32_t mouse_dx() const { return mdx_; }
    int32_t mouse_dy() const { return mdy_; }
    float wheel() const { return cur_.wheel; }
    bool shift() const { return key_down(vk::kShift); }
    bool ctrl() const { return key_down(vk::kControl); }
    bool alt() const { return key_down(vk::kAlt); }
    uint64_t frame_index() const { return frames_; }

private:
    InputConfig cfg_;
    InputFrame cur_;
    bool have_prev_ = false;
    KeySet pressed_, released_;
    std::array<ButtonState, kButtonCount> buttons_{};
    int32_t mdx_ = 0, mdy_ = 0;
    uint64_t frames_ = 0;
    int32_t last_press_button_ = -1;  // Win32: a press of another button breaks a double click
};

// ---- rebindable actions ----------------------------------------------------------------------------
enum class Action : uint8_t {
    ScrollLeft, ScrollRight, ScrollUp, ScrollDown,  // camera pan (held)
    RotateLeft, RotateRight,                        // camera yaw (held)
    ZoomIn, ZoomOut,                                // camera distance (held)
    ResetCamera,
    SelectAll, Stop,
    ControlGroup0, ControlGroup1, ControlGroup2, ControlGroup3, ControlGroup4,
    ControlGroup5, ControlGroup6, ControlGroup7, ControlGroup8, ControlGroup9,
    // [port: Gabriel] (docs/DECISIONS.md "Camera and controls"), appended: free tactical camera axes and pause.
    PitchUp, PitchDown, RollLeft, RollRight,
    Pause,
    Count
};
inline constexpr size_t kActionCount = static_cast<size_t>(Action::Count);
std::string_view action_name(Action a);

enum Modifier : uint8_t { kModNone = 0, kModShift = 1, kModCtrl = 2, kModAlt = 4, kModAny = 0x80 };

struct KeyBinding {
    uint16_t key = 0;      // virtual key; 0 = unbound
    uint8_t mods = kModNone;  // required modifiers; kModAny = ignore modifiers
    friend bool operator==(const KeyBinding&, const KeyBinding&) = default;
};

// Up to two bindings per action. Defaults = the default hotkey table (UI.md §4.3) through
// HotkeyTable::apply_to_bindings: W A S D scroll with any modifier [port: Gabriel], arrows yaw / pitch and Numpad 4 / 6
// roll, = / - zoom, Pause [port: Gabriel], End resets, Ctrl+Y select all (W A S D only move the camera), H stop (S
// scrolls), digits recall control groups (Ctrl+digit assigns, Ctrl+Shift+digit adds: OrderTranslator reads the mods).
// Modifier rule (P1_CAMERA_INPUT.md §6): kModNone matches only with Shift / Ctrl / Alt all up, a modified binding when
// its modifiers are held (others ignored). The queue modifier (default Shift) [port: Gabriel] makes a right-click order
// queued (orders.hpp).
class Bindings {
public:
    static constexpr size_t kSlots = 2;
    Bindings();  // defaults
    // [port: Gabriel] queue modifier (kModShift / kModCtrl / kModAlt bits, 0 = never queue): held -> orders queued.
    uint8_t queue_mods() const { return queue_mods_; }
    void set_queue_mods(uint8_t m) { queue_mods_ = static_cast<uint8_t>(m & (kModShift | kModCtrl | kModAlt)); }
    bool queue_held(const InputState& in) const;
    void bind(Action a, size_t slot, KeyBinding b) {
        if (slot < kSlots) map_[static_cast<size_t>(a)][slot] = b;
    }
    void clear(Action a) { map_[static_cast<size_t>(a)] = {}; }
    const KeyBinding& binding(Action a, size_t slot) const { return map_[static_cast<size_t>(a)][slot < kSlots ? slot : 0]; }

    bool held(const InputState& in, Action a) const;
    bool pressed(const InputState& in, Action a) const;

private:
    std::array<std::array<KeyBinding, kSlots>, kActionCount> map_{};
    uint8_t queue_mods_ = kModShift;
};
uint8_t current_mods(const InputState& in);

// ---- headless input replay ---------------------------------------------------------------------------
// Binary, little-endian, independent of host endianness and struct layout:
//   "AUIR" | u32 version (1) | u32 frame count | frames
//   frame := f64 time | f32 dt | u32 width | u32 height | i32 mouse_x | i32 mouse_y | u8 mouse_inside
//            | u8 buttons_down | u8 buttons_pressed | u8 buttons_released | f32 wheel
//            | 4 x u64 keys_down | 4 x u64 keys_pressed                         (= 100 bytes)
inline constexpr uint32_t kReplayVersion = 1;
inline constexpr size_t kReplayFrameBytes = 100;

struct InputReplay {
    std::vector<InputFrame> frames;
    void record(const InputFrame& f) { frames.push_back(f); }
    std::vector<uint8_t> serialize() const;
    // false on bad magic / version / truncation / trailing bytes (out is left empty then).
    static bool deserialize(std::span<const uint8_t> bytes, InputReplay& out);
};

}  // namespace alamo::ui

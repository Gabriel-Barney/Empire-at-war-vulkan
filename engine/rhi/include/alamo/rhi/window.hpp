// Minimal Win32 window (class registration + message pump) so tools and the viewer can open a
// swapchain window without a windowing library. On other platforms create() returns false.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace alamo::rhi {

enum class MouseButton : uint8_t { Left = 0, Right, Middle, X1, X2, Count };

struct WindowDesc {
    const char* title = "alamo";  // UTF-8
    uint32_t width = 1280, height = 720;  // client area
    bool resizable = true;
    bool visible = true;
};

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const WindowDesc& desc);
    void destroy();

    // Processes pending messages without blocking. Returns false once the window was closed.
    bool pump();
    bool is_open() const { return hwnd_ != nullptr && !close_requested_; }

    uint32_t width() const { return width_; }   // client size
    uint32_t height() const { return height_; }
    bool minimized() const { return width_ == 0 || height_ == 0; }
    bool take_resized() {  // true once after every size change
        const bool r = resized_;
        resized_ = false;
        return r;
    }
    // Virtual-key state (VK_ESCAPE = 0x1B ...), updated by pump().
    bool key_down(uint32_t vk) const { return vk < 256 && keys_[vk]; }
    bool key_pressed(uint32_t vk) {  // edge-triggered
        if (vk >= 256 || !pressed_[vk]) return false;
        pressed_[vk] = false;
        return true;
    }
    // Mouse, updated by pump(). Position is in client pixels (may go outside while a button is held:
    // the window captures the mouse between button down and up).
    int32_t mouse_x() const { return mouse_x_; }
    int32_t mouse_y() const { return mouse_y_; }
    bool mouse_down(MouseButton b) const { return buttons_[static_cast<size_t>(b)]; }
    bool mouse_pressed(MouseButton b) {  // edge-triggered, consumed
        const bool r = pressed_buttons_[static_cast<size_t>(b)];
        pressed_buttons_[static_cast<size_t>(b)] = false;
        return r;
    }
    bool mouse_released(MouseButton b) {  // edge-triggered, consumed
        const bool r = released_buttons_[static_cast<size_t>(b)];
        released_buttons_[static_cast<size_t>(b)] = false;
        return r;
    }
    // Accumulated wheel movement in notches (+ = away from the user) since the last call.
    float take_wheel() {
        const float w = static_cast<float>(wheel_) / 120.0f;
        wheel_ = 0;
        return w;
    }
    // Text input (integration fixes, alamo_game): the characters typed since the last call, in order, from
    // WM_CHAR / WM_UNICHAR (layout, dead keys, IME and Alt codes applied by Windows; UTF-16 surrogate pairs joined;
    // lone surrogates dropped). Control characters are included (0x08 backspace, 0x09 tab, 0x0D enter, 0x1B
    // escape ...; filter with < 0x20 / 0x7F), so a text field can take them from here or from key_pressed().
    // At most kMaxTextPerFrame codepoints are kept between calls (older ones dropped first).
    static constexpr size_t kMaxTextPerFrame = 256;
    std::u32string take_text();
    std::string take_text_utf8();  // the same, UTF-8 encoded
    void set_title(const char* utf8);

    void* native_handle() const { return hwnd_; }      // HWND
    void* native_instance() const { return hinstance_; }  // HINSTANCE

    // Window procedure entry (public for the C callback).
    intptr_t handle_message(uint32_t msg, uintptr_t wparam, intptr_t lparam);

private:
    void* hwnd_ = nullptr;
    void* hinstance_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    bool resized_ = false;
    bool close_requested_ = false;
    bool keys_[256] = {};
    bool pressed_[256] = {};
    void set_button(MouseButton b, bool down);
    int32_t mouse_x_ = 0, mouse_y_ = 0;
    int32_t wheel_ = 0;  // WHEEL_DELTA units
    bool buttons_[static_cast<size_t>(MouseButton::Count)] = {};
    bool pressed_buttons_[static_cast<size_t>(MouseButton::Count)] = {};
    bool released_buttons_[static_cast<size_t>(MouseButton::Count)] = {};
    uint32_t capture_count_ = 0;
    std::u32string text_;           // text input since the last take_text()
    uint16_t high_surrogate_ = 0;   // pending UTF-16 high surrogate of a WM_CHAR pair
    void push_char(uint32_t cp);
    void push_utf16(uint16_t unit);
};

}  // namespace alamo::rhi

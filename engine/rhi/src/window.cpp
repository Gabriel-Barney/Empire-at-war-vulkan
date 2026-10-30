#include "alamo/rhi/window.hpp"

#include <string>

#include "alamo/core/log.hpp"

#include <windows.h>

namespace alamo::rhi {

// ---- text input (integration fixes): platform-independent queue ----
void Window::push_char(uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (text_.size() >= kMaxTextPerFrame) text_.erase(text_.begin());
    text_.push_back(static_cast<char32_t>(cp));
}

void Window::push_utf16(uint16_t unit) {
    if (unit >= 0xD800 && unit <= 0xDBFF) {  // high surrogate: wait for its pair
        high_surrogate_ = unit;
        return;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {  // low surrogate: joined with a pending high one, else dropped
        if (high_surrogate_)
            push_char(0x10000u + ((static_cast<uint32_t>(high_surrogate_) - 0xD800u) << 10) + (static_cast<uint32_t>(unit) - 0xDC00u));
        high_surrogate_ = 0;
        return;
    }
    high_surrogate_ = 0;  // a lone high surrogate is dropped
    push_char(unit);
}

std::u32string Window::take_text() {
    std::u32string t;
    t.swap(text_);
    return t;
}

std::string Window::take_text_utf8() {
    std::string out;
    for (const char32_t c : take_text()) {
        const auto cp = static_cast<uint32_t>(c);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}


namespace {

constexpr wchar_t kClassName[] = L"AlamoRhiWindow";

std::wstring widen(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg != WM_NCCREATE) {
        const intptr_t r = self->handle_message(msg, static_cast<uintptr_t>(wp), static_cast<intptr_t>(lp));
        if (r != -1) return static_cast<LRESULT>(r);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool register_class(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    if (GetClassInfoExW(inst, kClassName, &wc)) return true;
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

}  // namespace

Window::~Window() { destroy(); }

bool Window::create(const WindowDesc& desc) {
    destroy();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!register_class(inst)) {
        log::errorf("rhi", "RegisterClassExW failed (%lu)", GetLastError());
        return false;
    }
    const DWORD style = desc.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    RECT r{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    AdjustWindowRect(&r, style, FALSE);
    const std::wstring title = widen(desc.title);
    HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, inst, this);
    if (!hwnd) {
        log::errorf("rhi", "CreateWindowExW failed (%lu)", GetLastError());
        return false;
    }
    hwnd_ = hwnd;
    hinstance_ = inst;
    close_requested_ = false;
    RECT cr{};
    GetClientRect(hwnd, &cr);
    width_ = static_cast<uint32_t>(cr.right - cr.left);
    height_ = static_cast<uint32_t>(cr.bottom - cr.top);
    if (desc.visible) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return true;
}

void Window::destroy() {
    if (hwnd_) {
        HWND h = static_cast<HWND>(hwnd_);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    hwnd_ = nullptr;
    hinstance_ = nullptr;
}

bool Window::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) close_requested_ = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return is_open();
}

void Window::set_title(const char* utf8) {
    if (hwnd_) SetWindowTextW(static_cast<HWND>(hwnd_), widen(utf8).c_str());
}

void Window::set_button(MouseButton b, bool down) {
    const auto i = static_cast<size_t>(b);
    if (down == buttons_[i]) return;
    buttons_[i] = down;
    (down ? pressed_buttons_ : released_buttons_)[i] = true;
    if (down) {
        if (capture_count_++ == 0) SetCapture(static_cast<HWND>(hwnd_));
    } else if (capture_count_ && --capture_count_ == 0) {
        ReleaseCapture();
    }
}

intptr_t Window::handle_message(uint32_t msg, uintptr_t wparam, intptr_t lparam) {
    switch (msg) {
        case WM_CLOSE: close_requested_ = true; return 0;
        case WM_SIZE: {
            const uint32_t w = static_cast<uint32_t>(LOWORD(static_cast<LPARAM>(lparam)));
            const uint32_t h = static_cast<uint32_t>(HIWORD(static_cast<LPARAM>(lparam)));
            if (w != width_ || h != height_) resized_ = true;
            width_ = w;
            height_ = h;
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wparam < 256) {
                if (!keys_[wparam]) pressed_[wparam] = true;
                keys_[wparam] = true;
            }
            break;  // let DefWindowProc handle Alt+F4 etc.
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wparam < 256) keys_[wparam] = false;
            break;
        case WM_KILLFOCUS:
            for (bool& k : keys_) k = false;
            for (size_t i = 0; i < static_cast<size_t>(MouseButton::Count); ++i)
                if (buttons_[i]) set_button(static_cast<MouseButton>(i), false);
            break;
        case WM_MOUSEMOVE:
            mouse_x_ = static_cast<int16_t>(LOWORD(static_cast<LPARAM>(lparam)));  // GET_X_LPARAM (signed)
            mouse_y_ = static_cast<int16_t>(HIWORD(static_cast<LPARAM>(lparam)));
            return 0;
        case WM_LBUTTONDOWN: set_button(MouseButton::Left, true); return 0;
        case WM_LBUTTONUP: set_button(MouseButton::Left, false); return 0;
        case WM_RBUTTONDOWN: set_button(MouseButton::Right, true); return 0;
        case WM_RBUTTONUP: set_button(MouseButton::Right, false); return 0;
        case WM_MBUTTONDOWN: set_button(MouseButton::Middle, true); return 0;
        case WM_MBUTTONUP: set_button(MouseButton::Middle, false); return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            set_button(HIWORD(static_cast<WPARAM>(wparam)) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2,
                       msg == WM_XBUTTONDOWN);
            return 1;  // TRUE for X buttons
        case WM_MOUSEWHEEL: wheel_ += static_cast<int16_t>(HIWORD(static_cast<WPARAM>(wparam))); return 0;
        case WM_MOUSEHWHEEL: hwheel_ += static_cast<int16_t>(HIWORD(static_cast<WPARAM>(wparam))); return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lparam) != static_cast<HWND>(hwnd_)) capture_count_ = 0;
            break;
        // Text input (integration fixes): UTF-16 units (TranslateMessage in pump()); WM_UNICHAR: whole codepoints.
        // (WM_SYSCHAR = Alt+key menu accelerators: not text, left to DefWindowProc.)
        case WM_CHAR:
            push_utf16(static_cast<uint16_t>(wparam & 0xFFFFu));
            return 0;
        case WM_UNICHAR:
            if (wparam == UNICODE_NOCHAR) return 1;  // "yes, this window takes WM_UNICHAR"
            push_char(static_cast<uint32_t>(wparam));
            return 0;
        default: break;
    }
    return -1;
}


}  // namespace alamo::rhi

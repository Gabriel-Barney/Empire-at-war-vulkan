"""g1_ui.py - drives the game's menus for the G1 kit: window-relative input + window screenshots (ctypes only).

Used by g1.py (`--drive K,E`, a thread next to the run's monitor loop) and on its own for calibration:

  python -I g1_ui.py shot  <out.png>                 screenshot of the game's client area
  python -I g1_ui.py click X Y [--method input|post] click at client coords given in 1920x1080 space
  python -I g1_ui.py key   NAME [--repeat N]         key press (ESC, RETURN, SPACE, F1..F12, A..Z, 0..9, ...)
  python -I g1_ui.py wheel X Y DELTA [--count N]     mouse wheel (DELTA 120 = one notch away from the user = zoom in)
  python -I g1_ui.py sig   <png> [...]                print a 32x18 luma signature (for scenes.json "expect")

Coordinates in scenes.json are client pixels of a 1920x1080 game window; they are scaled to the real client size.
Two input methods:
  input (default): bring the game window to the front, put the cursor there and use SendInput (scan codes for keys).
                   The game reads the hardware cursor, so this is the method that works; it moves the real mouse.
  post:            PostMessage WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_KEYDOWN to the window (no focus change).
Every step's screenshot is saved, so a run can be checked afterwards.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys
import time
import zlib

REF_W, REF_H = 1920, 1080

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: real pixels
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass

ULONG_PTR = ctypes.c_size_t


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ULONG_PTR)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [EnumWindowsProc, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.IsWindow.argtypes = [wt.HWND]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.GetForegroundWindow.restype = wt.HWND
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.BringWindowToTop.argtypes = [wt.HWND]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
user32.MapVirtualKeyW.argtypes = [wt.UINT, wt.UINT]
user32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
user32.GetDC.argtypes = [wt.HWND]
user32.GetDC.restype = wt.HDC
user32.GetWindowDC.argtypes = [wt.HWND]
user32.GetWindowDC.restype = wt.HDC
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowW.restype = wt.HWND
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.HDC, ctypes.c_int,
                         ctypes.c_int, wt.DWORD]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]
kernel32.GetCurrentThreadId.restype = wt.DWORD

WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP = 0x200, 0x201, 0x202, 0x204, 0x205
WM_MOUSEWHEEL, WM_KEYDOWN, WM_KEYUP = 0x20A, 0x100, 0x101
INPUT_MOUSE, INPUT_KEYBOARD = 0, 1
MOUSEEVENTF_MOVE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x1, 0x2, 0x4
MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_WHEEL = 0x8, 0x10, 0x800
KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE, KEYEVENTF_EXTENDEDKEY = 0x2, 0x8, 0x1
VK = {"ESC": 0x1B, "ESCAPE": 0x1B, "RETURN": 0x0D, "ENTER": 0x0D, "SPACE": 0x20, "TAB": 0x09, "BACK": 0x08,
      "LEFT": 0x25, "UP": 0x26, "RIGHT": 0x27, "DOWN": 0x28, "HOME": 0x24, "END": 0x23, "PGUP": 0x21, "PGDN": 0x22,
      "SHIFT": 0x10, "CTRL": 0x11, "ALT": 0x12, "PAUSE": 0x13, "NUMMINUS": 0x6D}
VK.update({"F%d" % i: 0x6F + i for i in range(1, 13)})
EXTENDED = {0x25, 0x26, 0x27, 0x28, 0x24, 0x23, 0x21, 0x22}


def vk_of(name):
    n = name.upper()
    if n in VK:
        return VK[n]
    if len(n) == 1 and (n.isalpha() or n.isdigit()):
        return ord(n)
    raise ValueError("unknown key " + name)


# ------------------------------------------------------------------------------------------------ windows
def windows_of(pid):
    res = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            res.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return res


def client_rect(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    p = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y, r.right - r.left, r.bottom - r.top


def main_window(pids):
    best, area = None, 0
    for pid in pids:
        for h in windows_of(pid):
            _, _, w, hh = client_rect(h)
            if w * hh > area:
                best, area = h, w * hh
    return best


user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]


def fit_window(hwnd, w=REF_W, h=REF_H):
    """Resize the window so its client area is w x h again (the game window can come up stretched to the monitor
    height; the game keeps rendering its 1920x1080 back buffer and Present scales it). Keeps the top-left corner."""
    ox, oy, cw, ch = client_rect(hwnd)
    if cw >= 1.4 * w:   # display scaling: the DPI-virtualized game window is s times larger in real pixels
        s = max(1, round(cw / w))
        w, h = w * s, h * s
    if (cw, ch) == (w, h):
        return False
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    fw = (r.right - r.left) - cw
    fh = (r.bottom - r.top) - ch
    user32.SetWindowPos(hwnd, None, max(r.left, 0), max(r.top, 0), w + fw, h + fh, 0x0004 | 0x0010)  # NOZORDER|NOACTIVATE
    time.sleep(0.5)
    return True


def to_client(hwnd, x, y):
    _, _, w, h = client_rect(hwnd)
    return int(round(x * w / REF_W)), int(round(y * h / REF_H))


def foreground(hwnd):
    if user32.GetForegroundWindow() == hwnd:
        return True
    fg = user32.GetForegroundWindow()
    tid_fg = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    me = kernel32.GetCurrentThreadId()
    if tid_fg and tid_fg != me:
        user32.AttachThreadInput(me, tid_fg, True)
    # an ALT tap lets a background process take the foreground (SetForegroundWindow rules)
    _key_raw(0x12, False)
    _key_raw(0x12, True)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.BringWindowToTop(hwnd)
    ok = user32.SetForegroundWindow(hwnd)
    if tid_fg and tid_fg != me:
        user32.AttachThreadInput(me, tid_fg, False)
    time.sleep(0.15)
    return bool(ok) and user32.GetForegroundWindow() == hwnd


# ------------------------------------------------------------------------------------------------ input
def _send(inputs):
    arr = (INPUT * len(inputs))(*inputs)
    return user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))


def _mouse(flags, data=0):
    i = INPUT(type=INPUT_MOUSE)
    i.u.mi = MOUSEINPUT(0, 0, data & 0xFFFFFFFF, flags, 0, 0)
    return i


def _key_raw(vk, up):
    sc = user32.MapVirtualKeyW(vk, 0)
    fl = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0) | (KEYEVENTF_EXTENDEDKEY if vk in EXTENDED else 0)
    i = INPUT(type=INPUT_KEYBOARD)
    i.u.ki = KEYBDINPUT(0, sc, fl, 0, 0)
    _send([i])


def _lparam(x, y):
    return (y & 0xFFFF) << 16 | (x & 0xFFFF)


def move(hwnd, x, y, method="input"):
    cx, cy = to_client(hwnd, x, y)
    if method == "post":
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(cx, cy))
        return
    ox, oy, _, _ = client_rect(hwnd)
    user32.SetCursorPos(ox + cx, oy + cy)
    _send([_mouse(MOUSEEVENTF_MOVE)])


def click(hwnd, x, y, method="input", button="left", double=False):
    cx, cy = to_client(hwnd, x, y)
    n = 2 if double else 1
    if method == "post":
        dn, upm, wp = (WM_LBUTTONDOWN, WM_LBUTTONUP, 1) if button == "left" else (WM_RBUTTONDOWN, WM_RBUTTONUP, 2)
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(cx, cy))
        for _ in range(n):
            user32.PostMessageW(hwnd, dn, wp, _lparam(cx, cy))
            time.sleep(0.05)
            user32.PostMessageW(hwnd, upm, 0, _lparam(cx, cy))
            time.sleep(0.05)
        return
    foreground(hwnd)
    move(hwnd, x, y)
    time.sleep(0.12)
    move(hwnd, x, y)  # a second move after the hover state settles
    time.sleep(0.08)
    d, u = (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP) if button == "left" else (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP)
    for _ in range(n):
        _send([_mouse(d)])
        time.sleep(0.07)
        _send([_mouse(u)])
        time.sleep(0.07)


def drag(hwnd, x0, y0, x1, y1, steps=12, method="input"):
    if method == "post":
        a, b = to_client(hwnd, x0, y0)
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(a, b))
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, _lparam(a, b))
        for k in range(1, steps + 1):
            a, b = to_client(hwnd, x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps)
            user32.PostMessageW(hwnd, WM_MOUSEMOVE, 1, _lparam(a, b))  # MK_LBUTTON
            time.sleep(0.04)
        time.sleep(0.1)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, _lparam(a, b))
        return
    foreground(hwnd)
    move(hwnd, x0, y0)
    time.sleep(0.1)
    _send([_mouse(MOUSEEVENTF_LEFTDOWN)])
    for k in range(1, steps + 1):
        move(hwnd, x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps)
        time.sleep(0.03)
    time.sleep(0.1)
    _send([_mouse(MOUSEEVENTF_LEFTUP)])


def wheel(hwnd, x, y, delta, count=1, method="input"):
    for _ in range(count):
        if method == "post":
            ox, oy, _, _ = client_rect(hwnd)
            cx, cy = to_client(hwnd, x, y)
            user32.PostMessageW(hwnd, WM_MOUSEWHEEL, (delta & 0xFFFF) << 16, _lparam(ox + cx, oy + cy))
        else:
            foreground(hwnd)
            move(hwnd, x, y)
            _send([_mouse(MOUSEEVENTF_WHEEL, delta)])
        time.sleep(0.08)


def key(hwnd, name, repeat=1, method="input", hold=0.06):
    vk = vk_of(name)
    for _ in range(repeat):
        if method == "post":
            sc = user32.MapVirtualKeyW(vk, 0)
            user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | (sc << 16))
            time.sleep(hold)
            user32.PostMessageW(hwnd, WM_KEYUP, vk, 1 | (sc << 16) | (3 << 30))
        else:
            foreground(hwnd)
            _key_raw(vk, False)
            time.sleep(hold)
            _key_raw(vk, True)
        time.sleep(0.15)


# ------------------------------------------------------------------------------------------------ screenshots
def grab(hwnd):
    """Client area as (w, h, BGRA bytes). PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) first (works when the
    window is covered); if that comes back black, a screen copy of the client rect."""
    ox, oy, w, h = client_rect(hwnd)
    if w <= 0 or h <= 0:
        return None
    for use_print in (True, False):
        sdc = user32.GetDC(None)
        mdc = gdi32.CreateCompatibleDC(sdc)
        bmp = gdi32.CreateCompatibleBitmap(sdc, w, h)
        old = gdi32.SelectObject(mdc, bmp)
        if use_print:
            ok = user32.PrintWindow(hwnd, mdc, 1 | 2)
        else:
            ok = gdi32.BitBlt(mdc, 0, 0, w, h, sdc, ox, oy, 0x00CC0020 | 0x40000000)
        bi = ctypes.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0), 44)
        buf = ctypes.create_string_buffer(w * h * 4)
        gdi32.GetDIBits(mdc, bmp, 0, h, buf, bi, 0)
        gdi32.SelectObject(mdc, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mdc)
        user32.ReleaseDC(None, sdc)
        data = buf.raw
        if ok and any(data[i] for i in range(0, len(data), 4 * 997)):
            return w, h, data
    return w, h, data


def write_png(path, img):
    w, h, data = img
    rows = []
    for y in range(h):
        row = data[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        rows.append(b"\x00" + bytes(rgb))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
    return path


def signature(img, gw=32, gh=18):
    """Mean luma per cell of a gw x gh grid (0..255), sampled every 4th pixel."""
    w, h, data = img
    sig = []
    for gy in range(gh):
        y0, y1 = gy * h // gh, (gy + 1) * h // gh
        for gx in range(gw):
            x0, x1 = gx * w // gw, (gx + 1) * w // gw
            s = n = 0
            for y in range(y0, y1, 4):
                base = y * w * 4
                for x in range(x0, x1, 4):
                    i = base + x * 4
                    s += 29 * data[i] + 150 * data[i + 1] + 77 * data[i + 2]
                    n += 1
            sig.append(int(s / max(n, 1) / 256))
    return sig


def region_mean(img, x0, y0, x1, y1):
    """Mean RGB of a rectangle given in 1920x1080 space."""
    w, h, data = img
    X0, X1 = int(x0 * w / REF_W), int(x1 * w / REF_W)
    Y0, Y1 = int(y0 * h / REF_H), int(y1 * h / REF_H)
    s = [0, 0, 0]
    n = 0
    for y in range(Y0, Y1, 2):
        for x in range(X0, X1, 2):
            i = (y * w + x) * 4
            s[0] += data[i + 2]; s[1] += data[i + 1]; s[2] += data[i]; n += 1
    return [v / max(n, 1) for v in s]


def sig_distance(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / max(len(a), 1)


def read_png(path):
    """(w, h, BGRA bytes) of an 8-bit RGB PNG written by write_png (filter 0 only)."""
    raw = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(raw):
        ln, typ = struct.unpack(">I4s", raw[pos:pos + 8])
        if typ == b"IHDR":
            w, h = struct.unpack(">II", raw[pos + 8:pos + 16])
        elif typ == b"IDAT":
            idat += raw[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    px = zlib.decompress(idat)
    data = bytearray(w * h * 4)
    for y in range(h):
        row = px[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)]
        data[y * w * 4 + 0:(y + 1) * w * 4:4] = row[2::3]
        data[y * w * 4 + 1:(y + 1) * w * 4:4] = row[1::3]
        data[y * w * 4 + 2:(y + 1) * w * 4:4] = row[0::3]
    return w, h, bytes(data)


def box_signature(img, box=None, gw=16, gh=4):
    """Mean luma per cell of a gw x gh grid over a box (1920x1080 space; None = whole image)."""
    w, h, data = img
    if box is None:
        X0, Y0, X1, Y1 = 0, 0, w, h
    else:
        X0, X1 = int(box[0] * w / REF_W), int(box[2] * w / REF_W)
        Y0, Y1 = int(box[1] * h / REF_H), int(box[3] * h / REF_H)
    sig = []
    for gy in range(gh):
        y0, y1 = Y0 + gy * (Y1 - Y0) // gh, Y0 + (gy + 1) * (Y1 - Y0) // gh
        for gx in range(gw):
            x0, x1 = X0 + gx * (X1 - X0) // gw, X0 + (gx + 1) * (X1 - X0) // gw
            s = n = 0
            for y in range(y0, y1):
                base = y * w * 4
                for x in range(x0, x1):
                    i = base + x * 4
                    s += 29 * data[i] + 150 * data[i + 1] + 77 * data[i + 2]
                    n += 1
            sig.append(s / max(n, 1) / 256)
    return sig


def read_png_sig(path):
    """Signature of a PNG written by write_png / the kit (8-bit RGB, filter 0 only)."""
    raw = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(raw):
        ln, typ = struct.unpack(">I4s", raw[pos:pos + 8])
        if typ == b"IHDR":
            w, h = struct.unpack(">II", raw[pos + 8:pos + 16])
        elif typ == b"IDAT":
            idat += raw[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    px = zlib.decompress(idat)
    data = bytearray(w * h * 4)
    for y in range(h):
        row = px[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)]
        data[y * w * 4 + 0:(y + 1) * w * 4:4] = row[2::3]
        data[y * w * 4 + 1:(y + 1) * w * 4:4] = row[1::3]
        data[y * w * 4 + 2:(y + 1) * w * 4:4] = row[0::3]
    return signature((w, h, bytes(data)))


# ------------------------------------------------------------------------------------------------ script driver
class DriveError(Exception):
    pass


class Driver:
    """Runs a list of steps (scenes.json "auto") against the game window.

    ctx: object with hwnd() -> HWND or None, alive() -> bool, request(label) (trigger / quit through the kit's
    control files), log(msg), shots_dir. Steps (coordinates in 1920x1080 client space):
      {"do": "wait", "s": 5}
      {"do": "wait_window", "s": 120}                          until the game window has its full client size
      {"do": "key", "key": "ESC", "repeat": 1, "gap": 0.5}
      {"do": "click", "x": 960, "y": 540, "label": "Single Player", "button": "left"|"right", "double": false}
      {"do": "drag", "x": .., "y": .., "x1": .., "y1": ..}
      {"do": "wheel", "x": .., "y": .., "delta": 120, "count": 5}
      {"do": "shot", "label": "name"}
      {"do": "burst", "label": "B", "n": 40, "gap": 0.3}      n screenshots ui_NNN_B00.. a short time apart
      {"do": "expect", "ref": "main_menu", "timeout": 60, "max_dist": 12, "poll": 1}
               waits until the screen matches the reference signature in scenes.json "refs" (mean |luma| diff)
      {"do": "expect_rgb", "box": [x0,y0,x1,y1], "rgb": [r,g,b], "tol": 40, "timeout": 30}
      {"do": "trigger", "label": "kuat_thrawn"}               capture / screenshot / marker through the kit
      {"do": "quit"}                                          asks the kit to close the game
    "optional": true on a step turns its failure into a log line.
    """

    def __init__(self, ctx, refs, method="input"):
        self.ctx = ctx
        self.refs = refs or {}
        self.method = method
        self.n = 0
        self.log = []
        self.park_at = None     # (x, y) client point the real cursor is held at (the game edge-scrolls on it)
        self._like = {}

    def park(self):
        if self.park_at:
            h = self.ctx.hwnd()
            if h:
                ox, oy, w, hh = client_rect(h)
                user32.SetCursorPos(int(ox + self.park_at[0] * w / REF_W), int(oy + self.park_at[1] * hh / REF_H))

    def like_dist(self, img, name):
        """Distance between the live screen's box and a reference crop (scenes.json refs: {png, box})."""
        r = self.refs.get(name)
        if not isinstance(r, dict) or "png" not in r:
            raise DriveError("no reference crop '%s' in scenes.json refs" % name)
        if name not in self._like:
            self._like[name] = box_signature(read_png(os.path.join(os.path.dirname(os.path.abspath(__file__)), r["png"])))
        return sig_distance(box_signature(img, r["box"]), self._like[name])

    def h(self):
        h = self.ctx.hwnd()
        if not h:
            raise DriveError("game window not found")
        return h

    def shot(self, label):
        img = grab(self.h())
        if img is None:
            return None, None
        self.n += 1
        p = os.path.join(self.ctx.shots_dir, "ui_%03d_%s.png" % (self.n, label))
        write_png(p, img)
        return img, p

    def note(self, m):
        t = time.time()
        self.log.append("%s.%03d %s" % (time.strftime("%H:%M:%S", time.localtime(t)), int((t % 1) * 1000), m))
        self.ctx.log("ui: " + m)

    def run(self, steps, scene=""):
        for i, s in enumerate(steps):
            if not self.ctx.alive():
                raise DriveError("game exited during step %d of %s" % (i, scene))
            self.park()
            try:
                self.one(s)
            except DriveError as e:
                if s.get("optional"):
                    self.note("optional step failed: %s" % e)
                    continue
                self.shot("FAIL_%s_%d" % (scene, i))
                raise DriveError("%s step %d (%s): %s" % (scene, i, s.get("label") or s.get("do"), e))

    def one(self, s):
        d = s["do"]
        lab = s.get("label", "")
        if d == "wait":
            t = s.get("s", 1)
            end = time.time() + t
            while time.time() < end:
                if not self.ctx.alive():
                    raise DriveError("game exited")
                self.park()
                time.sleep(max(0, min(0.5, end - time.time())))
        elif d == "wait_window":
            end = time.time() + s.get("s", 120)
            while time.time() < end:
                h = self.ctx.hwnd()
                if h:
                    _, _, w, hh = client_rect(h)
                    if w >= s.get("min_w", 1280) and hh >= s.get("min_h", 720):
                        self.note("window %dx%d" % (w, hh))
                        return
                time.sleep(1)
            raise DriveError("no game window")
        elif d == "fit_window":
            h = self.h()
            if fit_window(h):
                self.note("window resized to client %s" % (client_rect(h)[2:],))
            time.sleep(s.get("gap", 1))
        elif d == "key":
            key(self.h(), s["key"], s.get("repeat", 1), self.method)
            self.note("key %s x%d %s" % (s["key"], s.get("repeat", 1), lab))
            time.sleep(s.get("gap", 0.3))
        elif d == "click":
            for _ in range(s.get("repeat", 1)):
                click(self.h(), s["x"], s["y"], self.method, s.get("button", "left"), s.get("double", False))
                self.park()
                time.sleep(s.get("gap", 0.8))
            self.note("click (%d,%d) x%d %s" % (s["x"], s["y"], s.get("repeat", 1), lab))
        elif d == "park":
            self.park_at = (s["x"], s["y"]) if s.get("on", True) else None
            self.park()
            self.note("cursor park %s" % (self.park_at,))
        elif d == "expect_like":
            end = time.time() + s.get("timeout", 60)
            best = 1e9
            while True:
                img = grab(self.h())
                if img is not None:
                    dist = self.like_dist(img, s["ref"])
                    best = min(best, dist)
                    if dist <= s.get("max_dist", 10):
                        self.note("expect_like %s ok (dist %.1f)" % (s["ref"], dist))
                        return
                if time.time() > end:
                    raise DriveError("screen never showed '%s' (best dist %.1f)" % (s["ref"], best))
                if s.get("nudge_key"):
                    key(self.h(), s["nudge_key"], 1, self.method)
                if s.get("nudge_click"):
                    click(self.h(), s["nudge_click"][0], s["nudge_click"][1], self.method)
                self.park()
                time.sleep(s.get("poll", 1))
        elif d == "drag":
            drag(self.h(), s["x"], s["y"], s["x1"], s["y1"], method=self.method)
            self.note("drag (%d,%d)->(%d,%d) %s" % (s["x"], s["y"], s["x1"], s["y1"], lab))
            time.sleep(s.get("gap", 0.5))
        elif d == "wheel":
            wheel(self.h(), s["x"], s["y"], s.get("delta", 120), s.get("count", 1), self.method)
            self.note("wheel %d x%d %s" % (s.get("delta", 120), s.get("count", 1), lab))
        elif d == "burst":          # n screenshots, `gap` seconds apart (the grab itself takes ~0.1-0.2 s)
            for k in range(s.get("n", 10)):
                if not self.ctx.alive():
                    raise DriveError("game exited")
                t0 = time.time()
                self.shot("%s%02d" % (lab or "burst", k))
                time.sleep(max(0, s.get("gap", 0.3) - (time.time() - t0)))
            self.note("burst %s x%d" % (lab, s.get("n", 10)))
        elif d == "shot":
            _, p = self.shot(lab or "shot")
            self.note("shot " + str(p))
        elif d == "expect":
            ref = self.refs.get(s["ref"])
            if ref is None:
                raise DriveError("no reference '%s' in scenes.json refs" % s["ref"])
            end = time.time() + s.get("timeout", 60)
            best = 1e9
            while True:
                img = grab(self.h())
                if img is not None:
                    dist = sig_distance(signature(img), ref)
                    best = min(best, dist)
                    if dist <= s.get("max_dist", 14):
                        self.note("expect %s ok (dist %.1f)" % (s["ref"], dist))
                        self.shot(s["ref"])
                        return
                if time.time() > end:
                    raise DriveError("screen never matched '%s' (best dist %.1f)" % (s["ref"], best))
                if s.get("nudge_key"):
                    key(self.h(), s["nudge_key"], 1, self.method)
                time.sleep(s.get("poll", 1))
        elif d == "expect_rgb":
            end = time.time() + s.get("timeout", 30)
            while True:
                img = grab(self.h())
                if img is not None:
                    m = region_mean(img, *s["box"])
                    if all(abs(a - b) <= s.get("tol", 40) for a, b in zip(m, s["rgb"])):
                        self.note("expect_rgb %s ok %s" % (lab, [round(v) for v in m]))
                        return
                if time.time() > end:
                    raise DriveError("region %s never had colour %s (last %s)" % (s["box"], s["rgb"], [round(v) for v in m]))
                time.sleep(s.get("poll", 1))
        elif d == "trigger":
            self.ctx.request(s["label"])
            self.note("trigger " + s["label"])
            time.sleep(s.get("gap", 2))
        elif d == "quit":
            self.ctx.request("quit")
            self.note("quit requested")
        else:
            raise DriveError("unknown step " + d)


# ------------------------------------------------------------------------------------------------ CLI
def _game_hwnd():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from g1 import all_pids, image_of
    pids = [p for p in all_pids() if os.path.basename(image_of(p) or "").lower() == "starwarsg.exe"]
    h = main_window(pids)
    if not h:
        raise SystemExit("game window not found")
    return h


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["fit", "shot", "click", "rclick", "dclick", "key", "wheel", "drag", "sig", "info", "like"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--method", default="input", choices=["input", "post"])
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--count", type=int, default=1)
    a = ap.parse_args(argv)
    if a.cmd == "like":     # like <screenshot.png> <ref name> ...: distance to each scenes.json reference crop
        refs = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes.json"))).get("refs", {})
        d = Driver(None, refs)
        img = read_png(a.args[0])
        for n in a.args[1:] or [k for k, v in refs.items() if isinstance(v, dict)]:
            print("%-16s %.1f" % (n, d.like_dist(img, n)))
        return 0
    if a.cmd == "sig":
        for p in a.args:
            print(json.dumps(read_png_sig(p)))
        return 0
    h = _game_hwnd()
    if a.cmd == "fit":
        print(fit_window(h), client_rect(h))
    elif a.cmd == "info":
        print(json.dumps({"hwnd": h, "client": client_rect(h), "foreground": user32.GetForegroundWindow() == h}))
    elif a.cmd == "shot":
        img = grab(h)
        write_png(a.args[0], img)
        print(a.args[0], img[0], img[1])
    elif a.cmd in ("click", "rclick", "dclick"):
        click(h, float(a.args[0]), float(a.args[1]), a.method, "right" if a.cmd == "rclick" else "left", a.cmd == "dclick")
    elif a.cmd == "drag":
        drag(h, *[float(v) for v in a.args[:4]], method=a.method)
    elif a.cmd == "key":
        key(h, a.args[0], a.repeat, a.method)
    elif a.cmd == "wheel":
        wheel(h, float(a.args[0]), float(a.args[1]), int(a.args[2]), a.count, a.method)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

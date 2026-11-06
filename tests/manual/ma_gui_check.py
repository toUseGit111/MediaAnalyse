#!/usr/bin/env python3
"""Launch media-analyse.exe, capture its window to PNG, optionally drive input.

Usage:
  python ma_gui_check.py --exe PATH [--arg PATH] [--wait 3] [--shot out.png]
                         [--click X,Y] [--dbl X,Y] [--text "..." ] [--key ctrl+c]
                         [--wheel X,Y,-3] [--move X,Y]
Coordinates are window-relative logical pixels (window is set topmost first).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time
import zlib

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

user32.SetProcessDPIAware()
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowW.restype = wt.HWND
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetDC.argtypes = [wt.HWND]
user32.GetDC.restype = wt.HDC
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                         ctypes.c_int, wt.HDC, ctypes.c_int, ctypes.c_int,
                         ctypes.c_ulong]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, ctypes.c_uint, ctypes.c_uint,
                            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowTextLengthW.argtypes = [wt.HWND]
user32.IsWindowVisible.argtypes = [wt.HWND]


def find_window(needle="媒体分析器"):
    found = []
    cb_type = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        if n and user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if needle in buf.value:
                found.append((hwnd, buf.value))
        return True

    cb = cb_type(cb)
    user32.EnumWindows(cb, 0)
    return found


def rect(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def shot(hwnd, path, printwindow=False):
    l, t, rr, b = rect(hwnd)
    w, h = rr - l, b - t
    hdc = user32.GetDC(0)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    if printwindow:
        user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, ctypes.c_uint]
        user32.PrintWindow.restype = ctypes.c_bool
        ok = user32.PrintWindow(hwnd, mdc, 2)  # PW_RENDERFULLCONTENT
        print("PrintWindow ->", ok)
    else:
        gdi32.BitBlt(mdc, 0, 0, w, h, hdc, l, t, 0x00CC0020)  # SRCCOPY

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                    ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth, bi.biHeight = w, -h  # top-down
    bi.biPlanes, bi.biBitCount, bi.biCompression = 1, 32, 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    # BGRA -> RGB
    raw = bytearray(buf.raw)
    rows = []
    for y in range(h):
        line = bytearray(w * 3)
        base = y * w * 4
        for x in range(w):
            i = base + x * 4
            line[x * 3:x * 3 + 3] = raw[i + 2:i + 3] + raw[i + 1:i + 2] + raw[i:i + 1]
        rows.append(b"\x00" + bytes(line))
    data = b"".join(rows)

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + \
               struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(data, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(0, hdc)
    return (w, h)


def click(x, y, double=False):
    user32.SetCursorPos(int(x), int(y))
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    if double:
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        user32.mouse_event(0x0004, 0, 0, 0, 0)


def drag(x0, y0, x1, y1):
    user32.SetCursorPos(int(x0), int(y0))
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.08)
    for i in range(1, 13):
        user32.SetCursorPos(int(x0 + (x1 - x0) * i / 12),
                            int(y0 + (y1 - y0) * i / 12))
        time.sleep(0.02)
    time.sleep(0.08)
    user32.mouse_event(0x0004, 0, 0, 0, 0)


def wheel(x, y, notches):
    user32.SetCursorPos(int(x), int(y))
    for _ in range(abs(notches)):
        user32.mouse_event(0x0800, 0, 0, 120 * (1 if notches > 0 else -1), 0)
        time.sleep(0.03)


def paste_text(text):
    # CF_UNICODETEXT via clipboard + Ctrl+V
    user32.OpenClipboard.argtypes = [wt.HWND]
    user32.OpenClipboard.restype = ctypes.c_bool
    user32.EmptyClipboard.restype = ctypes.c_bool
    user32.SetClipboardData.argtypes = [ctypes.c_uint, wt.HANDLE]
    user32.SetClipboardData.restype = wt.HANDLE
    kernel32.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    data = text.encode("utf-16-le") + b"\x00\x00"
    h = kernel32.GlobalAlloc(0x0002, len(data))  # GMEM_MOVEABLE
    p = kernel32.GlobalLock(h)
    ctypes.memmove(p, data, len(data))
    kernel32.GlobalUnlock(h)
    user32.OpenClipboard(0)
    user32.EmptyClipboard()
    user32.SetClipboardData(13, h)
    user32.CloseClipboard()
    user32.keybd_event(0x11, 0, 0, 0)  # Ctrl
    user32.keybd_event(0x56, 0, 0, 0)  # V
    user32.keybd_event(0x56, 0, 2, 0)
    user32.keybd_event(0x11, 0, 2, 0)


def client_origin(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    p = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y, r.right - r.left, r.bottom - r.top


def post_mouse(hwnd, msg, x, y, buttons=0):
    user32.SendMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
    user32.SendMessageW.restype = ctypes.c_ssize_t
    lp = ((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF)
    return user32.SendMessageW(hwnd, msg, buttons, lp)


def post_click(hwnd, ox, oy, cx0, cy0, px, py):
    # px/py are physical offsets from the OUTER window rect; convert to the
    # client coordinate space expected by native mouse messages.
    x, y = px - (cx0 - ox), py - (cy0 - oy)
    post_mouse(hwnd, 0x0200, x, y, 0)             # WM_MOUSEMOVE
    post_mouse(hwnd, 0x0201, x, y, 1)             # WM_LBUTTONDOWN + MK_LBUTTON
    post_mouse(hwnd, 0x0202, x, y, 0)             # WM_LBUTTONUP


def post_drag(hwnd, ox, oy, cx0, cy0, p0x, p0y, p1x, p1y):
    x0, y0 = p0x - (cx0 - ox), p0y - (cy0 - oy)
    x1, y1 = p1x - (cx0 - ox), p1y - (cy0 - oy)
    post_mouse(hwnd, 0x0200, x0, y0, 0)
    post_mouse(hwnd, 0x0201, x0, y0, 1)
    for i in range(1, 11):
        post_mouse(hwnd, 0x0200, x0 + (x1 - x0) * i / 10,
                   y0 + (y1 - y0) * i / 10, 1)
    post_mouse(hwnd, 0x0202, x1, y1, 0)


def post_wheel(hwnd, screen_x, screen_y, notches):
    user32.SendMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
    delta = (-120 * int(notches)) & 0xFFFF
    wparam = delta << 16
    lp = ((int(screen_y) & 0xFFFF) << 16) | (int(screen_x) & 0xFFFF)
    return user32.SendMessageW(hwnd, 0x020A, wparam, lp)  # WM_MOUSEWHEEL


def post_text(hwnd, text):
    user32.SendMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
    for ch in text:
        user32.SendMessageW(hwnd, 0x0102, ord(ch), 0)   # WM_CHAR
        time.sleep(0.01)


def read_clipboard():
    user32.OpenClipboard.argtypes = [wt.HWND]
    user32.OpenClipboard.restype = ctypes.c_bool
    user32.GetClipboardData.argtypes = [ctypes.c_uint]
    user32.GetClipboardData.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    opened = False
    for _ in range(8):
        if user32.OpenClipboard(0):
            opened = True
            break
        time.sleep(0.12)
    if not opened:
        return "<clipboard busy>"
    h = user32.GetClipboardData(13)  # CF_UNICODETEXT
    text = ""
    if h:
        p = kernel32.GlobalLock(h)
        if p:
            text = ctypes.wstring_at(p)
            kernel32.GlobalUnlock(h)
    user32.CloseClipboard()
    return text


def post_key(hwnd, combo):
    keys = combo.lower().split("+")
    codes = {"ctrl": 0x11, "shift": 0x10, "c": 0x43, "a": 0x41,
             "v": 0x56, "enter": 0x0D, "esc": 0x1B}
    seq = [codes[k] for k in keys if k in codes]
    user32.SendMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
    for c in seq:
        user32.SendMessageW(hwnd, 0x0100, c, 0)   # WM_KEYDOWN
    for c in reversed(seq):
        user32.SendMessageW(hwnd, 0x0101, c, 0)   # WM_KEYUP


def key(combo):
    keys = combo.lower().split("+")
    codes = {"ctrl": 0x11, "shift": 0x10, "alt": 0x12, "enter": 0x0D,
             "esc": 0x1B, "c": 0x43, "a": 0x41, "v": 0x56}
    down = [codes[k] for k in keys if k in codes]
    for c in down:
        user32.keybd_event(c, 0, 0, 0)
    for c in reversed(down):
        user32.keybd_event(c, 0, 2, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--arg", default="")
    ap.add_argument("--wait", type=float, default=3.0)
    ap.add_argument("--shot", default="")
    ap.add_argument("--click", action="append", default=[])
    ap.add_argument("--drag", action="append", default=[])
    ap.add_argument("--text", default="")
    ap.add_argument("--key", default="")
    ap.add_argument("--wheel", default="")
    ap.add_argument("--log", default="")
    ap.add_argument("--no-launch", action="store_true")
    ap.add_argument("--printwindow", action="store_true")
    ap.add_argument("--software", action="store_true")
    ap.add_argument("--post-click", action="append", default=[])
    ap.add_argument("--post-drag", action="append", default=[])
    ap.add_argument("--post-key", default="")
    ap.add_argument("--post-text", action="append", default=[])
    ap.add_argument("--clipboard", action="store_true")
    ap.add_argument("--post-wheel", action="append", default=[])
    args = ap.parse_args()

    exe_dir = os.path.dirname(os.path.abspath(args.exe))
    log = open(args.log or os.path.join(exe_dir, "gui_stderr.log"), "w",
               encoding="utf-8", errors="replace")
    if not args.no_launch:
        cmd = [args.exe] + ([args.arg] if args.arg else [])
        env = dict(os.environ)
        if args.software:
            env["QT_QUICK_BACKEND"] = "software"
            env["QSG_RHI_BACKEND"] = "software"
        subprocess.Popen(cmd, cwd=exe_dir, stdout=log, stderr=log,
                         creationflags=0x00000008, env=env)
    time.sleep(args.wait)
    wins = find_window()
    if not wins:
        print("WINDOW NOT FOUND")
        sys.exit(2)
    hwnd, title = wins[0]
    user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002)  # TOPMOST NOSIZE|NOMOVE
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.4)
    l, t, rr, b = rect(hwnd)
    print("WINDOW", repr(title), "RECT", (l, t, rr, b), "SIZE", (rr - l, b - t))
    ox, oy = l, t

    for c in args.click:
        x, y = [float(v) for v in c.split(",")]
        click(ox + x, oy + y)
        time.sleep(0.25)
    for d in args.drag:
        x0, y0, x1, y1 = [float(v) for v in d.split(",")]
        drag(ox + x0, oy + y0, ox + x1, oy + y1)
        time.sleep(0.25)
    if args.wheel:
        x, y, n = [float(v) for v in args.wheel.split(",")]
        wheel(ox + x, oy + y, int(n))
        time.sleep(0.3)
    if args.text:
        paste_text(args.text)
        time.sleep(0.3)
    if args.key:
        key(args.key)
        time.sleep(0.3)
    cx0, cy0, cw, ch = client_origin(hwnd)
    print("CLIENT", (cx0, cy0, cw, ch))
    for d in args.post_drag:
        x0, y0, x1, y1 = [float(v) for v in d.split(",")]
        post_drag(hwnd, l, t, cx0, cy0, x0, y0, x1, y1)
        time.sleep(0.25)
    for c in args.post_click:
        x, y = [float(v) for v in c.split(",")]
        post_click(hwnd, l, t, cx0, cy0, x, y)
        time.sleep(0.25)
    for item in args.post_text:
        # "X,Y|text" -> click that spot, then send WM_CHAR for each character.
        pos, _, text = item.partition("|")
        if "," in pos:
            x, y = [float(v) for v in pos.split(",")]
            post_click(hwnd, l, t, cx0, cy0, x, y)
            time.sleep(0.2)
        post_text(hwnd, text)
        time.sleep(0.6)
    for item in args.post_wheel:
        x, y, n = [float(v) for v in item.split(",")]
        post_wheel(hwnd, l + x, t + y, int(n))
        time.sleep(0.35)
    if args.post_key:
        post_key(hwnd, args.post_key)
        time.sleep(0.35)
    if args.clipboard:
        print("CLIPBOARD:", repr(read_clipboard()[:600]))
    if args.shot:
        print("SHOT", args.shot, shot(hwnd, args.shot, args.printwindow))
    log.flush()


if __name__ == "__main__":
    main()

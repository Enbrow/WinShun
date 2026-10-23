"""Shooting a window with its real transparency: a backdrop is put right
behind it, black and then white, and a pixel the backdrop shows through by
some fraction differs between the two shots by that fraction. Rounded corners
and the border come out as they are, nothing is masked by hand. A third shot,
black again, must equal the first, or the caret blinked or an animation was
still running: then it shoots again."""
import ctypes
import pathlib
import sys
import time
from ctypes import wintypes as wt

import numpy as np
from PIL import Image, ImageGrab


def moved(black, check):
    """Where two shots that should be the same differ, if anywhere."""
    differ = np.abs(np.asarray(check, np.float32) - np.asarray(black, np.float32)).max(axis=2) > 6
    if differ.sum() <= 4:
        return None
    ys, xs = np.nonzero(differ)
    return f"{differ.sum()} pixels around ({xs.min()}..{xs.max()}, {ys.min()}..{ys.max()})"


def matte(black, white):
    """The window with its transparency, from the same spot shot with a black
    and a white backdrop, cropped to what isn't fully transparent."""
    black = np.asarray(black.convert("RGB"), np.float32)
    white = np.asarray(white.convert("RGB"), np.float32)
    alpha = np.clip(1 - (white - black).mean(axis=2) / 255, 0, 1)
    colour = np.where(alpha[..., None] > 1 / 255, black / np.maximum(alpha[..., None], 1 / 255), 0)
    ys, xs = np.nonzero(alpha > 2 / 255)
    pad = 2
    top, left = max(ys.min() - pad, 0), max(xs.min() - pad, 0)
    bottom, right = ys.max() + 1 + pad, xs.max() + 1 + pad
    out = np.dstack([np.clip(colour, 0, 255), alpha * 255])[top:bottom, left:right]
    return Image.fromarray(np.round(out).astype(np.uint8), "RGBA")


class Desktop:
    """The few Win32 calls the shots need, in physical pixels."""

    WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
    ENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    class WNDCLASSEXW(ctypes.Structure):
        _fields_ = [("cbSize", wt.UINT), ("style", wt.UINT), ("lpfnWndProc", ctypes.c_void_p),
                    ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int), ("hInstance", wt.HINSTANCE),
                    ("hIcon", wt.HICON), ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                    ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR), ("hIconSm", wt.HICON)]

    def __init__(self):
        self.user32 = u = ctypes.WinDLL("user32", use_last_error=True)
        self.dwmapi = ctypes.WinDLL("dwmapi")
        self.gdi32 = g = ctypes.WinDLL("gdi32")
        self.kernel32 = k = ctypes.WinDLL("kernel32", use_last_error=True)
        u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per monitor, v2
        u.GetForegroundWindow.restype = wt.HWND
        u.GetWindowLongPtrW.restype = ctypes.c_ssize_t
        u.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
        u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
        u.IsWindowVisible.argtypes = [wt.HWND]
        u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
        u.EnumWindows.argtypes = [self.ENUMPROC, wt.LPARAM]
        u.CreateWindowExW.restype = wt.HWND
        u.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p]
        u.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
        u.SetClassLongPtrW.restype = ctypes.c_size_t
        u.SetClassLongPtrW.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_ssize_t]
        u.InvalidateRect.argtypes = [wt.HWND, ctypes.c_void_p, wt.BOOL]
        u.UpdateWindow.argtypes = [wt.HWND]
        u.DestroyWindow.argtypes = [wt.HWND]
        u.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]
        u.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
        u.RegisterClassExW.argtypes = [ctypes.POINTER(self.WNDCLASSEXW)]
        u.UnregisterClassW.argtypes = [wt.LPCWSTR, wt.HINSTANCE]
        u.DefWindowProcW.restype = ctypes.c_ssize_t
        u.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]
        u.SendMessageTimeoutW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPCWSTR, wt.UINT, wt.UINT,
                                          ctypes.c_void_p]
        self.dwmapi.DwmGetWindowAttribute.argtypes = [wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD]
        g.CreateSolidBrush.restype = wt.HBRUSH
        g.DeleteObject.argtypes = [wt.HANDLE]
        k.GetModuleHandleW.restype = wt.HMODULE
        k.OpenProcess.restype = wt.HANDLE
        k.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
        k.CloseHandle.argtypes = [wt.HANDLE]

    def windows(self):
        """The visible top-level windows, front to back."""
        found = []
        self.user32.EnumWindows(self.ENUMPROC(lambda h, _: found.append(h) or True), 0)
        return [h for h in found if self.user32.IsWindowVisible(h) and not self.cloaked(h)]

    def cloaked(self, hwnd):
        value = wt.DWORD()
        self.dwmapi.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(value), 4)  # DWMWA_CLOAKED
        return value.value != 0

    def frame(self, hwnd):
        r = wt.RECT()
        self.dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(r), ctypes.sizeof(r))  # extended frame bounds
        return r.left, r.top, r.right, r.bottom

    def class_name(self, hwnd):
        name = ctypes.create_unicode_buffer(256)
        self.user32.GetClassNameW(hwnd, name, 256)
        return name.value

    def topmost(self, hwnd):
        return bool(self.user32.GetWindowLongPtrW(hwnd, -20) & 0x8)  # WS_EX_TOPMOST

    def program(self, hwnd):
        pid = wt.DWORD()
        self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        process = self.kernel32.OpenProcess(0x1000, False, pid.value)  # query limited information
        if not process:
            return ""
        name = ctypes.create_unicode_buffer(1024)
        length = wt.DWORD(1024)
        ok = self.kernel32.QueryFullProcessImageNameW(process, 0, name, ctypes.byref(length))
        self.kernel32.CloseHandle(process)
        return pathlib.PureWindowsPath(name.value).name.lower() if ok else ""

    def pump(self):
        msg = wt.MSG()
        while self.user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            self.user32.TranslateMessage(ctypes.byref(msg))
            self.user32.DispatchMessageW(ctypes.byref(msg))

    def settle(self):
        """Until DWM has put the latest change on the screen."""
        self.pump()
        for _ in range(2):
            self.dwmapi.DwmFlush()
        time.sleep(0.02)


class Backdrop:
    """A window of one colour right behind the given ones (front to back),
    never activated."""

    def __init__(self, desktop, rect, windows):
        self.d = d = desktop
        self.instance = d.kernel32.GetModuleHandleW(None)
        self.brush = d.gdi32.CreateSolidBrush(0)
        self.name = f"WinShunShotBackdrop{id(self)}"
        cls = Desktop.WNDCLASSEXW()
        cls.cbSize = ctypes.sizeof(cls)
        cls.lpfnWndProc = ctypes.cast(d.user32.DefWindowProcW, ctypes.c_void_p).value
        cls.hInstance = self.instance
        cls.hbrBackground = self.brush
        cls.lpszClassName = self.name
        if not d.user32.RegisterClassExW(ctypes.byref(cls)):
            raise ctypes.WinError(ctypes.get_last_error())
        left, top, right, bottom = rect
        topmost = all(d.topmost(h) for h in windows)
        self.hwnd = d.user32.CreateWindowExW(0x08000080 | (0x8 if topmost else 0), self.name, "", 0x80000000,
                                             left, top, right - left, bottom - top, None, None, self.instance, None)
        if not self.hwnd:  # no activate, tool window, maybe topmost; popup
            raise ctypes.WinError(ctypes.get_last_error())
        flags = 0x0010 | 0x0040  # no activate, show
        if topmost:
            # Topmost from the start: made so later (elevated, while another
            # program's window has the focus, as with the clipboard, which
            # doesn't take it) it stays below that window, success reported.
            # Above all, and the windows shot put back over it in their order.
            d.user32.SetWindowPos(self.hwnd, wt.HWND(-1), left, top, right - left, bottom - top, flags)
            for h in reversed(windows):
                d.user32.SetWindowPos(h, wt.HWND(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)  # keep size, place, focus
        else:
            d.user32.SetWindowPos(self.hwnd, windows[-1], left, top, right - left, bottom - top, flags)
        d.settle()
        stack = d.windows()
        if self.hwnd in stack:
            covering = [h for h in stack[:stack.index(self.hwnd)] if h not in windows and near(d.frame(h), rect, 0)
                        and min(d.frame(h)[2] - d.frame(h)[0], d.frame(h)[3] - d.frame(h)[1]) > 40]
            if covering:
                print(f"warning: in front of the backdrop too: {[d.class_name(h) for h in covering]} (shooting "
                      f"{[(d.class_name(h), d.frame(h), d.topmost(h)) for h in windows]})")

    def fill(self, colour):
        brush = self.d.gdi32.CreateSolidBrush(colour)
        self.d.user32.SetClassLongPtrW(self.hwnd, -10, brush)  # GCLP_HBRBACKGROUND
        self.d.gdi32.DeleteObject(self.brush)
        self.brush = brush
        self.d.user32.InvalidateRect(self.hwnd, None, True)
        self.d.user32.UpdateWindow(self.hwnd)
        self.d.settle()

    def close(self):
        self.d.user32.DestroyWindow(self.hwnd)
        self.d.pump()
        self.d.user32.UnregisterClassW(self.name, self.instance)
        self.d.gdi32.DeleteObject(self.brush)


def near(a, b, distance):
    return a[0] < b[2] + distance and b[0] < a[2] + distance and a[1] < b[3] + distance and b[1] < a[3] + distance


def overlap(a, b):
    """How much of the smaller of two rectangles the other covers, 0 to 1."""
    w = min(a[2], b[2]) - max(a[0], b[0])
    h = min(a[3], b[3]) - max(a[1], b[1])
    if w <= 0 or h <= 0:
        return 0.0
    return w * h / min((a[2] - a[0]) * (a[3] - a[1]), (b[2] - b[0]) * (b[3] - b[1]))


def shoot(delay=0.0, winshun=False, margin=64, tries=12, beside=False, hold=False, with_class=None):
    """The window in front, or with `winshun` WinShun's frontmost one (the
    clipboard pops up without taking the focus), and WinShun's windows by it
    (the logo; with `beside` also its others against it, such as the preview
    beside the launcher), with transparency. `hold`: the pointer stays where it
    is (a drag in progress, the pie under it). `with_class`: and the visible
    windows of that class (CabinetWClass: an Explorer window) that lie against
    the main one, which is then WinShun's frontmost (the launcher over Explorer)."""
    d = Desktop()
    time.sleep(delay)
    order = d.windows()
    def size(h):
        f = d.frame(h)
        return f[2] - f[0], f[3] - f[1]
    ours = [h for h in order if d.program(h) == "winshun.exe" and min(size(h)) > 4]
    main = d.user32.GetForegroundWindow()
    if winshun:
        main = next((h for h in ours if size(h)[0] >= 200 and size(h)[1] >= 150), None)  # not the taskbar box
        if main is None:
            sys.exit("no window of WinShun's is up")
    def logo(h):
        return max(size(h)) <= 160
    # A file dialog and the bar against it go together, whichever is in front.
    # The bar sits against the dialog's edge: a dialog that merely lies behind
    # (another program's, such as a licence window) overlaps it and stays out.
    targets = [main]
    if main in ours:
        targets += [h for h in order if h not in ours and d.class_name(h) == "#32770"
                    and near(d.frame(h), d.frame(main), 24) and overlap(d.frame(h), d.frame(main)) < 0.1]
        if beside:
            targets += [h for h in ours if h != main and not logo(h) and near(d.frame(h), d.frame(main), 24)]
    else:
        targets += [h for h in ours if not logo(h) and near(d.frame(h), d.frame(main), 24)]
    if with_class:
        targets += [h for h in order if h not in ours and h not in targets and d.class_name(h) == with_class
                    and overlap(d.frame(h), d.frame(main)) > 0.02 and min(size(h)) > 100]
    targets += [h for h in ours if logo(h) and any(near(d.frame(h), d.frame(t), 160) for t in targets)]
    frames = [d.frame(h) for h in targets]
    screen = (d.user32.GetSystemMetrics(76), d.user32.GetSystemMetrics(77))  # virtual screen origin
    screen += (screen[0] + d.user32.GetSystemMetrics(78), screen[1] + d.user32.GetSystemMetrics(79))
    region = (max(min(f[0] for f in frames) - margin, screen[0]), max(min(f[1] for f in frames) - margin, screen[1]),
              min(max(f[2] for f in frames) + margin, screen[2]), min(max(f[3] for f in frames) + margin, screen[3]))

    cursor = wt.POINT()
    d.user32.GetCursorPos(ctypes.byref(cursor))
    if not hold and near((cursor.x, cursor.y, cursor.x + 1, cursor.y + 1), region, 0):
        d.user32.SetCursorPos(screen[0] + 2, screen[1] + 2)  # out of the way, and hover effects gone
        time.sleep(0.4)

    targets.sort(key=lambda h: order.index(h) if h in order else len(order))
    backdrop = Backdrop(d, region, targets)
    try:
        for _ in range(tries):
            shots = []
            for colour in (0x000000, 0xFFFFFF, 0x000000):
                backdrop.fill(colour)
                shots.append(ImageGrab.grab(region, include_layered_windows=True, all_screens=True).convert("RGB"))
            change = moved(shots[0], shots[2])
            if not change:
                break
            print(f"something moved ({change}), shooting again")
            time.sleep(0.15)
        else:
            sys.exit("the window kept changing")
    finally:
        backdrop.close()
    return matte(shots[0], shots[1])

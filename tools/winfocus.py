"""Find and foreground the MGS4 window (the game ignores input unless it is the foreground window)."""
import ctypes, ctypes.wintypes as w, time

u = ctypes.windll.user32
k = ctypes.windll.kernel32
TITLE_PART = "METAL GEAR SOLID 4"


def find_window(title_part=TITLE_PART):
    found = []

    def cb(hwnd, _):
        if u.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(512)
            u.GetWindowTextW(hwnd, buf, 512)
            if title_part.upper() in buf.value.upper():
                found.append(hwnd)
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)(cb), 0)
    return found[0] if found else None


def is_foreground(hwnd):
    return hwnd is not None and u.GetForegroundWindow() == hwnd


def focus(hwnd, tries=3):
    """Alt tap + AttachThreadInput: the sequence Windows needs before it allows a foreground change."""
    for _ in range(tries):
        if is_foreground(hwnd):
            return True
        ctypes.windll.user32.keybd_event(0x12, 0, 0, 0)   # Alt down
        ctypes.windll.user32.keybd_event(0x12, 0, 2, 0)   # Alt up
        fg = u.GetWindowThreadProcessId(u.GetForegroundWindow(), None)
        me = k.GetCurrentThreadId()
        if fg and fg != me:
            u.AttachThreadInput(me, fg, True)
        u.ShowWindow(hwnd, 9)
        u.BringWindowToTop(hwnd)
        u.SetForegroundWindow(hwnd)
        if fg and fg != me:
            u.AttachThreadInput(me, fg, False)
        time.sleep(0.35)
    return is_foreground(hwnd)


if __name__ == "__main__":
    h = find_window()
    print("hwnd:", h, "foreground:", is_foreground(h))
    if h:
        print("focus ->", focus(h))


VK_ESCAPE = 0x1B


def abort_requested():
    """True while Escape is down anywhere on the machine - the manual stop for unattended runs."""
    return bool(u.GetAsyncKeyState(VK_ESCAPE) & 0x8000)

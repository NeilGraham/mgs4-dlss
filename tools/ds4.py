"""Virtual DualShock 4 via ViGEmBus, used to press Cross (X) during cutscenes so MGS4's flashback prompts fire.

Talks to ViGEmClient.dll directly (ctypes) - the ViGEmBus driver must be installed, but nothing is installed here.
Put ViGEmClient.dll (from the vgamepad package, Nefarius BSD-3) next to this file or set VIGEM_CLIENT_DLL.

  python ds4.py test                  - create the pad, press Cross 5x
  python ds4.py spam <seconds> [period_s] [hold_s]   - press Cross repeatedly for a while
"""
import ctypes, os, sys, time

DLL = os.environ.get("VIGEM_CLIENT_DLL") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "ViGEmClient.dll")

# DS4 button bits (ViGEm)
DPAD_NONE = 0x8
CROSS = 1 << 5
CIRCLE = 1 << 6
TRIANGLE = 1 << 7
SQUARE = 1 << 4
OPTIONS = 1 << 13


class DS4Report(ctypes.Structure):
    _fields_ = [("bThumbLX", ctypes.c_ubyte), ("bThumbLY", ctypes.c_ubyte),
                ("bThumbRX", ctypes.c_ubyte), ("bThumbRY", ctypes.c_ubyte),
                ("wButtons", ctypes.c_ushort), ("bSpecial", ctypes.c_ubyte),
                ("bTriggerL", ctypes.c_ubyte), ("bTriggerR", ctypes.c_ubyte)]


class DS4:
    def __init__(self):
        if not os.path.exists(DLL):
            raise FileNotFoundError("ViGEmClient.dll not found at " + DLL)
        self.lib = ctypes.WinDLL(DLL)
        self.lib.vigem_alloc.restype = ctypes.c_void_p
        self.lib.vigem_target_ds4_alloc.restype = ctypes.c_void_p
        self.lib.vigem_connect.argtypes = [ctypes.c_void_p]
        self.lib.vigem_target_add.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.lib.vigem_target_ds4_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p, DS4Report]
        self.client = self.lib.vigem_alloc()
        r = self.lib.vigem_connect(self.client)
        if r != 0x20000000:   # VIGEM_ERROR_NONE
            raise RuntimeError("vigem_connect failed: 0x%08X (is ViGEmBus installed?)" % (r & 0xFFFFFFFF))
        self.pad = self.lib.vigem_target_ds4_alloc()
        r = self.lib.vigem_target_add(self.client, self.pad)
        if r != 0x20000000:
            raise RuntimeError("vigem_target_add failed: 0x%08X" % (r & 0xFFFFFFFF))
        self.neutral()

    def _report(self, buttons=0, lx=0.0, ly=0.0, rx=0.0, ry=0.0):
        def ax(v): return max(0, min(255, int(round(128 + v * 127))))
        rep = DS4Report(ax(lx), ax(ly), ax(rx), ax(ry), DPAD_NONE | buttons, 0, 0, 0)
        self.lib.vigem_target_ds4_update(self.client, self.pad, rep)

    def stick(self, lx=0.0, ly=0.0, rx=0.0, ry=0.0, buttons=0):
        # axes in -1..1 (DS4: +x right, +y down); hold until the next report
        self._report(buttons, lx, ly, rx, ry)

    def neutral(self):
        self._report(0)

    def tap(self, button=CROSS, hold=0.10):
        self._report(button); time.sleep(hold); self._report(0)

    def close(self):
        try:
            self.lib.vigem_target_remove(self.client, self.pad)
            self.lib.vigem_target_free(self.pad)
            self.lib.vigem_disconnect(self.client)
            self.lib.vigem_free(self.client)
        except Exception:
            pass


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "test"
    pad = DS4()
    print("virtual DualShock 4 connected")
    if cmd == "test":
        for _ in range(5):
            pad.tap(); time.sleep(0.5)
        print("pressed Cross 5x")
        time.sleep(1)
    elif cmd == "spam":
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
        period = float(sys.argv[3]) if len(sys.argv) > 3 else 0.8
        hold = float(sys.argv[4]) if len(sys.argv) > 4 else 0.10
        end = time.time() + secs
        n = 0
        while time.time() < end:
            pad.tap(hold=hold); n += 1
            time.sleep(max(0.0, period - hold))
        print(f"pressed Cross {n}x over {secs:.0f}s")
    pad.close()


if __name__ == "__main__":
    main()

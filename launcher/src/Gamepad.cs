// A controller, read two ways and reported one way.
//
// XInput is Windows' own gamepad API and is what an Xbox pad speaks, along with anything that arrives dressed as
// one - Steam Input, DS4Windows, and the virtual pad a Sunshine/Moonlight stream creates. A DualSense or a
// DualShock 4 plugged straight in speaks neither: it is a plain HID device with a report layout of Sony's own, so
// those are found by vendor id and read raw. Both land in the same PadState, and the window never asks which.
//
// Polling, not events. XInput has no event; it is asked every UI tick, which at 60 Hz is cheap for a connected
// pad and is done every two seconds for an empty slot, where the call is slow. The HID read blocks, so it has a
// thread of its own that writes the latest report into a slot the tick reads.
//
// What comes out: Button for a press (with repeats while a direction is held, so a list can be walked by holding
// down), Tick with the stick positions every frame for anything that scrolls continuously, and Connection when
// a pad arrives or goes - which is what shows and hides the button guide.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Threading;
using Microsoft.Win32.SafeHandles;

namespace Mgs4Launcher
{
    enum PadButton { None, A, B, X, Y, LB, RB, LT, RT, Start, Select, Up, Down, Left, Right, LS, RS }
    enum PadKind { None, Xbox, Sony }

    struct PadState
    {
        public bool A, B, X, Y, LB, RB, LT, RT, Start, Select, Up, Down, Left, Right, LS, RS;
        public double LX, LY, RX, RY;       // -1..1, up and right positive, dead zone already taken out
        public bool Connected;

        public bool Get(PadButton b)
        {
            switch (b)
            {
                case PadButton.A: return A; case PadButton.B: return B; case PadButton.X: return X; case PadButton.Y: return Y;
                case PadButton.LB: return LB; case PadButton.RB: return RB; case PadButton.LT: return LT; case PadButton.RT: return RT;
                case PadButton.Start: return Start; case PadButton.Select: return Select;
                case PadButton.Up: return Up; case PadButton.Down: return Down; case PadButton.Left: return Left; case PadButton.Right: return Right;
                case PadButton.LS: return LS; case PadButton.RS: return RS;
            }
            return false;
        }
    }

    static class Gamepad
    {
        public const double DeadZone = 0.24;
        const double RepeatFirst = 0.38, RepeatEvery = 0.075;   // seconds: a held direction walks a list at 13 rows/s
        const double StickAsButton = 0.6;                       // how far the left stick goes before it counts as a d-pad press

        public static event Action<PadButton, bool> Button;     // (which, isRepeat)
        public static event Action<PadState, double> Tick;      // (state, seconds since the last tick)
        public static event Action<bool, PadKind> Connection;

        public static bool Connected { get; private set; }
        public static PadKind Kind { get; private set; }
        public static PadState State { get { return _last; } }

        // The left stick also acts as a d-pad (Up/Down/Left/Right with repeat) when this is on - a slider wants
        // that, a list that the stick scrolls does not. The window flips it with the tab.
        public static bool StickIsDpad;

        // The window only acts on a pad while it is the active window. A test harness showing the window off
        // screen can never activate it, so it sets this.
        public static bool IgnoreFocus;

        static DispatcherTimer _timer;
        static PadState _last;
        static DateTime _tickAt;
        static readonly Dictionary<PadButton, double> _held = new Dictionary<PadButton, double>();
        static readonly PadButton[] Repeating = { PadButton.Up, PadButton.Down, PadButton.Left, PadButton.Right };
        static readonly PadButton[] All = { PadButton.A, PadButton.B, PadButton.X, PadButton.Y, PadButton.LB, PadButton.RB,
                                            PadButton.LT, PadButton.RT, PadButton.Start, PadButton.Select, PadButton.Up,
                                            PadButton.Down, PadButton.Left, PadButton.Right, PadButton.LS, PadButton.RS };

        public static void Start(Dispatcher d)
        {
            if (_timer != null) return;
            XInput.Load();
            SonyHid.Start();
            _tickAt = DateTime.Now;
            _timer = new DispatcherTimer(DispatcherPriority.Input, d) { Interval = TimeSpan.FromMilliseconds(16) };
            _timer.Tick += (s, e) => Poll();
            _timer.Start();
        }

        public static void Stop()
        {
            if (_timer != null) { _timer.Stop(); _timer = null; }
            SonyHid.Stop();
        }

        static void Poll()
        {
            DateTime now = DateTime.Now;
            double dt = Math.Min(0.1, (now - _tickAt).TotalSeconds);
            _tickAt = now;

            PadState x = XInput.Read(now);
            PadState s = SonyHid.Read();
            PadState cur = Merge(x, s);
            PadKind kind = x.Connected ? PadKind.Xbox : s.Connected ? PadKind.Sony : PadKind.None;

            if (cur.Connected != Connected || kind != Kind)
            {
                Connected = cur.Connected;
                Kind = kind;
                if (Connection != null) Connection(Connected, Kind);
            }

            // The left stick as a d-pad, folded into the direction bits before the edges are taken.
            if (StickIsDpad)
            {
                if (cur.LY > StickAsButton) cur.Up = true;
                if (cur.LY < -StickAsButton) cur.Down = true;
                if (cur.LX < -StickAsButton) cur.Left = true;
                if (cur.LX > StickAsButton) cur.Right = true;
            }

            foreach (PadButton b in All)
            {
                bool down = cur.Get(b), was = _last.Get(b);
                if (down && !was)
                {
                    _held[b] = 0;
                    if (Button != null) Button(b, false);
                }
                else if (down && Array.IndexOf(Repeating, b) >= 0)
                {
                    double t = (_held.ContainsKey(b) ? _held[b] : 0) + dt;
                    if (t >= RepeatFirst)
                    {
                        t -= RepeatEvery;
                        if (Button != null) Button(b, true);
                    }
                    _held[b] = t;
                }
                else if (!down) _held.Remove(b);
            }
            _last = cur;
            if (Tick != null && cur.Connected) Tick(cur, dt);
        }

        static PadState Merge(PadState a, PadState b)
        {
            if (!a.Connected) return b;
            if (!b.Connected) return a;
            var m = a;
            m.A |= b.A; m.B |= b.B; m.X |= b.X; m.Y |= b.Y; m.LB |= b.LB; m.RB |= b.RB; m.LT |= b.LT; m.RT |= b.RT;
            m.Start |= b.Start; m.Select |= b.Select; m.Up |= b.Up; m.Down |= b.Down; m.Left |= b.Left; m.Right |= b.Right;
            m.LS |= b.LS; m.RS |= b.RS;
            if (Math.Abs(b.LX) > Math.Abs(m.LX)) m.LX = b.LX;
            if (Math.Abs(b.LY) > Math.Abs(m.LY)) m.LY = b.LY;
            if (Math.Abs(b.RX) > Math.Abs(m.RX)) m.RX = b.RX;
            if (Math.Abs(b.RY) > Math.Abs(m.RY)) m.RY = b.RY;
            return m;
        }

        // A stick axis with the dead zone taken out and what is left rescaled to the full range, so a stick just
        // past the zone moves slowly rather than jumping.
        public static double Axis(double raw)
        {
            double a = Math.Abs(raw);
            if (a < DeadZone) return 0;
            double v = (a - DeadZone) / (1 - DeadZone);
            return raw < 0 ? -v : v;
        }

        // ------------------------------------------------------------------------------------------- XInput

        static class XInput
        {
            [StructLayout(LayoutKind.Sequential)]
            struct XGamepad { public ushort Buttons; public byte LT, RT; public short LX, LY, RX, RY; }
            [StructLayout(LayoutKind.Sequential)]
            struct XState { public uint Packet; public XGamepad Pad; }

            [UnmanagedFunctionPointer(CallingConvention.StdCall)]
            delegate int GetStateFn(uint user, out XState state);

            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryW(string name);
            [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr module, string name);

            static GetStateFn _get;
            static readonly DateTime[] _retry = new DateTime[4];    // when each empty slot is next worth asking
            static readonly bool[] _up = new bool[4];

            public static void Load()
            {
                foreach (string dll in new[] { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" })
                {
                    try
                    {
                        IntPtr h = LoadLibraryW(dll);
                        if (h == IntPtr.Zero) continue;
                        IntPtr p = GetProcAddress(h, "XInputGetState");
                        if (p == IntPtr.Zero) continue;
                        _get = (GetStateFn)Marshal.GetDelegateForFunctionPointer(p, typeof(GetStateFn));
                        return;
                    }
                    catch { }
                }
            }

            public static PadState Read(DateTime now)
            {
                var s = new PadState();
                if (_get == null) return s;
                for (uint i = 0; i < 4; i++)
                {
                    if (!_up[i] && now < _retry[i]) continue;
                    XState xs;
                    int r;
                    try { r = _get(i, out xs); } catch { return s; }
                    if (r != 0) { _up[i] = false; _retry[i] = now.AddSeconds(2); continue; }
                    _up[i] = true;
                    ushort b = xs.Pad.Buttons;
                    var p = new PadState
                    {
                        Connected = true,
                        Up = (b & 0x0001) != 0, Down = (b & 0x0002) != 0, Left = (b & 0x0004) != 0, Right = (b & 0x0008) != 0,
                        Start = (b & 0x0010) != 0, Select = (b & 0x0020) != 0, LS = (b & 0x0040) != 0, RS = (b & 0x0080) != 0,
                        LB = (b & 0x0100) != 0, RB = (b & 0x0200) != 0,
                        A = (b & 0x1000) != 0, B = (b & 0x2000) != 0, X = (b & 0x4000) != 0, Y = (b & 0x8000) != 0,
                        LT = xs.Pad.LT > 80, RT = xs.Pad.RT > 80,
                        LX = Axis(xs.Pad.LX / 32767.0), LY = Axis(xs.Pad.LY / 32767.0),
                        RX = Axis(xs.Pad.RX / 32767.0), RY = Axis(xs.Pad.RY / 32767.0),
                    };
                    s = Merge(s, p);
                }
                return s;
            }
        }

        // ------------------------------------------------------------------------------------------- Sony HID

        // DualShock 4 and DualSense, over USB or Bluetooth, read as raw HID input reports. The layouts differ by
        // pad and by link, but in each the sticks come first and the buttons are three bytes with the same bits:
        //   buttons0: hat in the low nibble (0-7 clockwise from up, 8 = centred), square 0x10, cross 0x20,
        //             circle 0x40, triangle 0x80
        //   buttons1: L1 0x01, R1 0x02, L2 0x04, R2 0x08, share/create 0x10, options 0x20, L3 0x40, R3 0x80
        //   buttons2: PS 0x01, touchpad 0x02
        // Where those bytes sit:
        //   DualSense USB    report 0x01, 64 bytes: LX LY RX RY L2 R2 seq buttons0 buttons1 buttons2 (from byte 1)
        //   DualSense BT     report 0x31, 78 bytes: the same from byte 2; or, before the pad is asked for its
        //                    calibration, the short report 0x01: LX LY RX RY buttons0 buttons1 buttons2 L2 R2
        //   DualShock 4 USB  report 0x01, 64 bytes: LX LY RX RY buttons0 buttons1 buttons2 L2 R2 (from byte 1)
        //   DualShock 4 BT   report 0x11, 78 bytes: the same from byte 3; or the short 0x01 as the USB first bytes
        // Cross is A, circle B, square X, triangle Y; options is Start and share/create is Select.
        static class SonyHid
        {
            const int VidSony = 0x054C;
            static readonly int[] Dualsense = { 0x0CE6, 0x0DF2 };
            static readonly int[] Dualshock = { 0x05C4, 0x09CC, 0x0BA0 };

            static readonly object _lock = new object();
            static PadState _state;
            static Thread _thread;
            static volatile bool _stop;
            static SafeFileHandle _handle;
            static DateTime _reportAt;

            public static void Start()
            {
                if (_thread != null) return;
                _stop = false;
                _thread = new Thread(Run) { IsBackground = true, Name = "sony-hid" };
                _thread.Start();
            }

            public static void Stop()
            {
                _stop = true;
                try { if (_handle != null) _handle.Close(); } catch { }
            }

            public static PadState Read()
            {
                lock (_lock)
                {
                    // A pad that stopped talking half a second ago is gone, whatever the last report said.
                    if (_state.Connected && (DateTime.Now - _reportAt).TotalSeconds > 0.5) _state = new PadState();
                    return _state;
                }
            }

            static void Run()
            {
                while (!_stop)
                {
                    string path = null; bool sense = false;
                    try { path = Find(out sense); } catch { }
                    if (path == null) { Thread.Sleep(1500); continue; }
                    try { Pump(path, sense); }
                    catch { }
                    lock (_lock) _state = new PadState();
                    if (!_stop) Thread.Sleep(1000);
                }
            }

            static void Pump(string path, bool sense)
            {
                SafeFileHandle h = CreateFileW(path, 0x80000000 | 0x40000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
                if (h.IsInvalid)
                {
                    // Some pads refuse write access and give read alone; that is all this needs.
                    h = CreateFileW(path, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
                    if (h.IsInvalid) throw new IOException("open failed");
                }
                _handle = h;
                var buf = new byte[256];
                using (var fs = new FileStream(h, FileAccess.Read, 256, false))
                {
                    while (!_stop)
                    {
                        int n = fs.Read(buf, 0, buf.Length);
                        if (n <= 0) break;
                        PadState s;
                        if (!Parse(buf, n, sense, out s)) continue;
                        lock (_lock) { _state = s; _reportAt = DateTime.Now; }
                    }
                }
            }

            static bool Parse(byte[] r, int n, bool sense, out PadState s)
            {
                s = new PadState();
                int stick, buttons, l2, r2;
                byte id = r[0];
                if (sense)
                {
                    if (id == 0x31 && n >= 12) { stick = 2; l2 = 6; r2 = 7; buttons = 9; }
                    else if (id == 0x01 && n >= 11) { stick = 1; l2 = 5; r2 = 6; buttons = 8; }
                    else if (id == 0x01 && n >= 10) { stick = 1; buttons = 5; l2 = 8; r2 = 9; }
                    else return false;
                }
                else
                {
                    if (id == 0x11 && n >= 13) { stick = 3; buttons = 7; l2 = 10; r2 = 11; }
                    else if (id == 0x01 && n >= 10) { stick = 1; buttons = 5; l2 = 8; r2 = 9; }
                    else return false;
                }
                if (buttons + 2 >= n) return false;
                byte b0 = r[buttons], b1 = r[buttons + 1], b2 = r[buttons + 2];
                int hat = b0 & 0x0F;
                s.Connected = true;
                s.Up = hat == 0 || hat == 1 || hat == 7;
                s.Right = hat == 1 || hat == 2 || hat == 3;
                s.Down = hat == 3 || hat == 4 || hat == 5;
                s.Left = hat == 5 || hat == 6 || hat == 7;
                s.X = (b0 & 0x10) != 0; s.A = (b0 & 0x20) != 0; s.B = (b0 & 0x40) != 0; s.Y = (b0 & 0x80) != 0;
                s.LB = (b1 & 0x01) != 0; s.RB = (b1 & 0x02) != 0;
                s.LT = (b1 & 0x04) != 0 || (l2 < n && r[l2] > 80);
                s.RT = (b1 & 0x08) != 0 || (r2 < n && r[r2] > 80);
                s.Select = (b1 & 0x10) != 0; s.Start = (b1 & 0x20) != 0; s.LS = (b1 & 0x40) != 0; s.RS = (b1 & 0x80) != 0;
                s.LX = Axis((r[stick] - 127.5) / 127.5);
                s.LY = Axis(-(r[stick + 1] - 127.5) / 127.5);      // HID has down positive; the window wants up
                s.RX = Axis((r[stick + 2] - 127.5) / 127.5);
                s.RY = Axis(-(r[stick + 3] - 127.5) / 127.5);
                return true;
            }

            // The first Sony pad among the HID interfaces. Each is asked for its vendor and product id rather than
            // read off its path: a USB path spells them vid_054c&pid_0ce6 and a Bluetooth one
            // vid&0002054c_pid&0ce6, and the attributes are the same either way.
            static string Find(out bool sense)
            {
                sense = false;
                Guid hid = new Guid("4D1E55B2-F16F-11CF-88CB-001111000030");
                IntPtr set = SetupDiGetClassDevs(ref hid, IntPtr.Zero, IntPtr.Zero, 0x12);   // present + interface
                if (set == IntPtr.Zero || set == new IntPtr(-1)) return null;
                try
                {
                    var iface = new SP_DEVICE_INTERFACE_DATA { cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA)) };
                    for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hid, i, ref iface); i++)
                    {
                        int size = 0;
                        SetupDiGetDeviceInterfaceDetail(set, ref iface, IntPtr.Zero, 0, ref size, IntPtr.Zero);
                        if (size <= 0) continue;
                        IntPtr detail = Marshal.AllocHGlobal(size);
                        string path;
                        try
                        {
                            Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);   // cbSize, as the header packs it
                            if (!SetupDiGetDeviceInterfaceDetail(set, ref iface, detail, size, ref size, IntPtr.Zero)) continue;
                            path = Marshal.PtrToStringUni(new IntPtr(detail.ToInt64() + 4));
                        }
                        finally { Marshal.FreeHGlobal(detail); }
                        if (path == null) continue;

                        int vid, pid;
                        if (!Ids(path, out vid, out pid) || vid != VidSony) continue;
                        if (Array.IndexOf(Dualsense, pid) >= 0) { sense = true; return path; }
                        if (Array.IndexOf(Dualshock, pid) >= 0) { sense = false; return path; }
                    }
                }
                finally { SetupDiDestroyDeviceInfoList(set); }
                return null;
            }

            // Opened with no access at all, which every HID interface allows even when it is a keyboard or
            // someone else has it - enough to ask who made it.
            static bool Ids(string path, out int vid, out int pid)
            {
                vid = pid = 0;
                using (SafeFileHandle h = CreateFileW(path, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero))
                {
                    if (h.IsInvalid) return false;
                    var a = new HIDD_ATTRIBUTES { Size = Marshal.SizeOf(typeof(HIDD_ATTRIBUTES)) };
                    if (!HidD_GetAttributes(h, ref a)) return false;
                    vid = a.VendorID; pid = a.ProductID;
                    return true;
                }
            }

            [StructLayout(LayoutKind.Sequential)]
            struct HIDD_ATTRIBUTES { public int Size; public ushort VendorID, ProductID, VersionNumber; }
            [DllImport("hid.dll", SetLastError = true)]
            static extern bool HidD_GetAttributes(SafeFileHandle h, ref HIDD_ATTRIBUTES a);

            [StructLayout(LayoutKind.Sequential)]
            struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid guid; public int flags; public IntPtr reserved; }

            [DllImport("setupapi.dll", SetLastError = true)]
            static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr hwnd, int flags);
            [DllImport("setupapi.dll", SetLastError = true)]
            static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid guid, int index, ref SP_DEVICE_INTERFACE_DATA data);
            [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
            static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data, IntPtr detail, int size, ref int needed, IntPtr devInfo);
            [DllImport("setupapi.dll", SetLastError = true)]
            static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr sec, uint disposition, uint flags, IntPtr template);
        }
    }
}
